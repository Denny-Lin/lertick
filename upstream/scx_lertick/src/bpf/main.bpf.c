/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Hungyu Lin <dennylin0707@gmail.com>
 *
 * scx_lertick: weighted virtual-time scheduling with learned time slices.
 *
 * Tasks wait in one DSQ per LLC, ordered by weighted virtual time; an idle
 * CPU steals from another LLC's DSQ. The virtual clock advances with the CPU
 * time actually delivered over the total runnable weight, so tasks that keep
 * waking cannot hold back tasks that have already run. A woken task is placed
 * at most LAG_NS behind the clock.
 *
 * In online mode each task trains a small linear model inside BPF on its own
 * completed CPU bursts (wake to sleep). At the next wakeup the prediction sets
 * the task's time slice, bounded by slice_min and slice_max. Fixed mode uses
 * slice_fixed for every task. Fairness always charges the CPU time actually
 * used, never the prediction.
 *
 * The queue policy (per-LLC queues, continuation, cgroup weights) is in
 * policy.h. Queues and CPU selection follow scx_simple; cgroup weights follow
 * scx_flatcg (Tejun Heo).
 */
#include <scx/common.bpf.h>
#include "intf.h"
#include "model.h"

char _license[] SEC("license") = "GPL";

/*
 * Maximum time a task can wait in the scheduler's queue before triggering
 * a stall. With hundreds of runnable tasks per CPU a fair round alone can
 * take most of a second.
 */
#define STARVATION_MS 5000ULL

/* Model inputs and targets are scaled against this burst length. */
#define REFERENCE_NS (10ULL * NSEC_PER_MSEC)
#define QUEUE_REFERENCE 16U

/*
 * Scheduling mode and slice bounds, set by user space before load.
 */
const volatile u32 mode	       = LERTICK_MODE_ONLINE;
const volatile u64 slice_fixed = 2ULL * NSEC_PER_MSEC;
const volatile u64 slice_min   = 500ULL * NSEC_PER_USEC;
const volatile u64 slice_max   = 3ULL * NSEC_PER_MSEC;
/*
 * Completed bursts a task needs before its prediction sets its slice.
 */
const volatile u32 min_train_samples = 8;

/* Statistics, read by user space. */
u64 nr_local_dispatches, nr_shared_dispatches, nr_continuations;
u64 nr_adaptive_slices, nr_training_updates;

/*
 * Define struct user_exit_info, shared between BPF and user space to
 * communicate the exit status.
 */
UEI_DEFINE(uei);

/* MEASURE_WAKE2: one wakeup a task issued in its current burst. */
#define NR_WEV 8
struct wev {
	u64   ts;
	u32   wakee;
	u8    sync;
	__s32 f[LERTICK_FEATURES], fp[LERTICK_FEATURES];
	u64   p_last, p_ewma, p_pair, p_lin, p_linp;
};

/*
 * Per-task context: the model, the current wake-to-sleep episode and the
 * weights the task is counted with.
 */
struct task_ctx {
	struct lertick_model model;
	__s32		     features[LERTICK_FEATURES];
	u64		     runtime_start_ns;
	u64		     episode_runtime_ns;
	u64		     last_sleep_ns;
	u64		     last_burst_ns;
	u64		     ewma_burst_ns;
	u64		     prediction_ns;
	u64		     completed_bursts;
	bool		     episode_active;
	bool		     episode_valid;
	bool		     has_slept;
	bool		     running;
	bool		     started_in_select;
	u32 rq_weight; /* weight counted in runnable_weight, 0 when not counted */
	bool cg_counted; /* rq_weight is also counted in its cgroup's sums */
	u32 eff_weight; /* weight of the last charge; dispatch() cannot look up cgroups */
	char comm[16]; /* program the model was last synced to */
	u32  waker; /* LERTICK_TRACE: pid that woke the current episode */
	u32  wk_pid[4]; /* LERTICK_TRACE: recent wakers ... */
	u64  wk_burst[4]; /* ... and the last burst each of them started */
	u32  wk_next;
	u64  e_wk, e_avg; /* WAKER_PICK: recent error of each burst guess */
	bool inherited; /* the model was taken over from the program table */
	u64  dl_off; /* DEADLINE: offset added to dsq_vtime while queued */
	u64  wk_last_ts; /* MEASURE_WAKE: when this task last woke another */
	u64  ral_last, ral_ewma; /* run after the last wake, last and average */
	u32  wk_count; /* wakes issued in the current burst */
	bool wk_last_sync, ral_valid;
	/* MEASURE_WAKE2: run-after-wake predictors */
	struct wev	     ev[NR_WEV];
	u32		     nev;
	struct lertick_model ml, mlp;
	u64		     rem_last, rem_ewma;
	u32		     pp_pid[4];
	u64		     pp_val[4];
	u32		     pp_next;
	bool		     rem_valid;
	/* PRED_PLACE: where this task is queued and how much work it added */
	s64  pp_amt;
	u64  pp_ts, pp_pred;
	u32  pp_idx;
	u8   pp_where, pp_ran_where; /* 1: a CPU's local DSQ, 2: shared */
	char pp_waker[16]; /* program of the sync waker of this episode */
	__s32 pp_feat[LERTICK_FEATURES]; /* state when it was queued */
	bool  pp_feat_ok;
	u8    pp_bucket;
	u32   pp_last_wakee, pp_flips;
	u64   pp_flip_ts;
	u64   pp_wait_last;
	/* STEAL_COST: bursts that started on the CPU of the last run, or not */
	s32   sc_last_cpu;
	bool  sc_mig;
	u64   sc_stay, sc_migr;
	u32   sc_n_stay, sc_n_mig;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/*
 * Return the context of task @p, or NULL.
 */
static struct task_ctx *task_ctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

#ifndef LERTICK_MODEL_V2
#define LERTICK_MODEL_V2 0
#endif

/*
 * Experimental (TRUST_FRESH): a model that did not start from its program's
 * needs TRUST_FRESH updates before its prediction is used; one taken over
 * from the program table needs min_train_samples.
 */
#ifndef TRUST_FRESH
#define TRUST_FRESH 0
#endif

static __always_inline bool model_trusted(struct task_ctx *tctx)
{
	return tctx->model.updates >= min_train_samples &&
	       (!TRUST_FRESH || tctx->inherited ||
		tctx->model.updates >= TRUST_FRESH);
}

#ifndef LERTICK_MEASURE
#define LERTICK_MEASURE 0
#endif
/*
 * Measurement only (LERTICK_MEASURE): how well three predictors guess a
 * completed burst, and how often a slice actually ends a burst. Bursts are
 * capped at REFERENCE_NS as the model's target is.
 */
u64 m_bursts, m_err_lin, m_err_ewma, m_err_last;
u64 m_under_lin, m_under_ewma;
u64 m_over_fixed, m_over_cap;
u64 m_cls_lin, m_cls_ewma, m_cls_last;
u64 m_stops, m_expired;
/* Over all completed bursts: the value actually used (prediction when trusted, else the fixed slice). */
u64 m_all, m_err_used, m_cls_used;

#ifndef LERTICK_PROG_TABLE
#define LERTICK_PROG_TABLE 0
#endif
/*
 * A model per program (by comm) that every task of the program trains with
 * each completed burst. A task takes it over when it starts running a
 * program (at its first wakeup and after exec), so short-lived processes,
 * such as a compiler's, predict from their first burst with what all
 * earlier processes of the program learned.
 */
/* Experimental: a program model is taken over only once it has this many updates. */
#ifndef PROG_MIN_UPDATES
#define PROG_MIN_UPDATES 0ULL
#endif

#ifndef PROG_INHERIT_UPDATES
#define PROG_INHERIT_UPDATES (~0ULL)
#endif

struct prog_key {
	char comm[16];
};

struct prog_val {
	struct lertick_model model;
	u64		     ewma_burst_ns;
	u64		     burst_stay, burst_mig; /* STEAL_COST */
	u32		     n_stay, n_mig;
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 4096);
	__type(key, struct prog_key);
	__type(value, struct prog_val);
} prog_stor SEC(".maps");

u64 nr_prog_hits;

#ifndef LERTICK_LONG_KEEP
#define LERTICK_LONG_KEEP 0
#endif
u64 nr_long_keeps;

/* A trusted model predicts this task's current burst to pass the fixed slice. */
static __always_inline bool task_is_long(struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	return tctx && mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	       tctx->model.updates >= min_train_samples &&
	       tctx->prediction_ns >= slice_fixed;
}
#define LERTICK_LONG(p) (LERTICK_LONG_KEEP && task_is_long(p))
#define LERTICK_LONG_KEPT() __sync_fetch_and_add(&nr_long_keeps, 1)
#ifndef DEADLINE
#define DEADLINE 0
#endif
#ifndef DEADLINE_SCALE
#define DEADLINE_SCALE 100
#endif
/*
 * With DEADLINE, keep_prev() compares the running task's deadline: its vtime
 * plus the offset of the slice it would get next.
 */
#define LERTICK_PREV_OFF(p, tctx)                                          \
	(DEADLINE && (tctx) ?                                              \
		 slice_fixed * DEADLINE_SCALE /                            \
			 ((tctx)->eff_weight ? (tctx)->eff_weight : 100) : \
		 0)
/*
 * Measurement only (MEASURE_CLAMP): how often the lag bounds act. c_clamp[0]:
 * held to AHEAD_NS ahead; [1]: raised to BEHIND_NS behind; [2]: held by the
 * WAIT_MAX_NS cap; c_lag: wakeups raised to LAG_NS behind; c_enq: enqueues.
 */
#ifndef MEASURE_CLAMP
#define MEASURE_CLAMP 0
#endif
u64 c_clamp[3], c_lag, c_enq;
#define LERTICK_CLAMP_NOTE(kind)                                  \
	do {                                                      \
		if (MEASURE_CLAMP && (kind) == 1)                 \
			__sync_fetch_and_add(&c_clamp[1], 1);     \
		else if (MEASURE_CLAMP && (kind) == 2)            \
			__sync_fetch_and_add(&c_clamp[2], 1);     \
		else if (MEASURE_CLAMP)                           \
			__sync_fetch_and_add(&c_clamp[0], 1);     \
	} while (0)
#define LERTICK_MOVE_TO_LOCAL(dsq) scx_bpf_dsq_move_to_local(dsq, 0)
#define LERTICK_TASK_CGROUP(p) scx_bpf_task_cgroup(p)
#include "policy.h"

#define ENQ_KICK 0
u64 nr_enq_kicks;

/*
 * STEAL_PCPU (experiment): a sync wakee goes to a per-CPU DSQ of the waker's
 * CPU instead of its local DSQ. The CPU takes from that DSQ first, as it
 * would from its local DSQ, but other CPUs can steal from it when they have
 * nothing else to run, as an idle CPU pulls from a busy one in the kernel's
 * load balancing. 2 also kicks an idle CPU when the per-CPU DSQ already holds
 * a task.
 */
#define STEAL_PCPU 0
#define PCPU_DSQ(c) (0x10000ULL + (u64)(c))
u64 nr_pcpu_inserts, nr_pcpu_own, nr_pcpu_steals, nr_pcpu_kicks;

/*
 * STEAL_COST (experiment): a task's migration cost is how much longer its
 * bursts run when they start on a CPU other than the one it last ran on,
 * learned per task, or per program name until the task has enough bursts
 * of each kind. A stealing CPU takes, among the first STEAL_SCAN tasks of
 * the per-CPU DSQ it steals from, the one with the lowest known cost.
 */
#define STEAL_COST 0
#define STEAL_SCAN 4
#define SC_MIN_N 4
u64 nr_sc_task_cost, nr_sc_prog_cost, nr_sc_unknown, nr_sc_skip_head;

static __always_inline u64 steal_cost(struct task_struct *p)
{
	struct task_ctx *t = task_ctx(p);
	struct prog_key	 key = {};
	struct prog_val *v;

	if (!t)
		return 0;
	if (t->sc_n_stay >= SC_MIN_N && t->sc_n_mig >= SC_MIN_N) {
		__sync_fetch_and_add(&nr_sc_task_cost, 1);
		return t->sc_migr > t->sc_stay ? t->sc_migr - t->sc_stay : 0;
	}
	__builtin_memcpy(key.comm, p->comm, sizeof(key.comm));
	v = bpf_map_lookup_elem(&prog_stor, &key);
	if (v && v->n_stay >= SC_MIN_N && v->n_mig >= SC_MIN_N) {
		__sync_fetch_and_add(&nr_sc_prog_cost, 1);
		return v->burst_mig > v->burst_stay ? v->burst_mig - v->burst_stay : 0;
	}
	__sync_fetch_and_add(&nr_sc_unknown, 1);
	return 0;
}

/* Move the cheapest-to-migrate of the first STEAL_SCAN tasks of @dsq here. */
static __always_inline bool steal_cheapest(u64 dsq)
{
	struct task_struct *q;
	u64 best = ~0ULL;
	int i = 0, pick = 0;

	bpf_for_each(scx_dsq, q, dsq, 0) {
		u64 c;

		if (i >= STEAL_SCAN)
			break;
		c = steal_cost(q);
		if (c < best) {
			best = c;
			pick = i;
		}
		i++;
	}
	i = 0;
	bpf_for_each(scx_dsq, q, dsq, 0) {
		if (i >= STEAL_SCAN)
			break;
		if (i == pick &&
		    scx_bpf_dsq_move(BPF_FOR_EACH_ITER, q, SCX_DSQ_LOCAL, 0)) {
			if (pick)
				__sync_fetch_and_add(&nr_sc_skip_head, 1);
			return true;
		}
		i++;
	}
	return LERTICK_MOVE_TO_LOCAL(dsq);
}

/*
 * Steal from the per-CPU DSQ holding the most tasks, preferring CPUs of the
 * same LLC domain: another domain's queue is taken only when no CPU of this
 * one has a task waiting, as the shared DSQs are.
 */
static __always_inline bool pcpu_steal(s32 cpu)
{
	u32 n = scx_bpf_nr_cpu_ids(), i, home = domain_of(cpu);
	s32 best = -1, far = -1;
	u64 most = 0, most_far = 0;

	bpf_for(i, 0, n) {
		u64 q;

		if (i >= LERTICK_MAX_CPUS)
			break;
		if ((s32)i == cpu)
			continue;
		q = scx_bpf_dsq_nr_queued(PCPU_DSQ(i));
		if (domain_of(i) == home) {
			if (q > most) {
				most = q;
				best = i;
			}
		} else if (q > most_far) {
			most_far = q;
			far	 = i;
		}
	}
	if (best < 0)
		best = far;
	if (best >= 0 && (STEAL_COST ? steal_cheapest(PCPU_DSQ(best)) :
				       LERTICK_MOVE_TO_LOCAL(PCPU_DSQ(best)))) {
		__sync_fetch_and_add(&nr_pcpu_steals, 1);
		return true;
	}
	return false;
}

/*
 * PRED_PLACE (experiment): placement of sync wakees by predicted wait.
 * Each CPU's local DSQ and each shared DSQ keeps the sum of the slices its
 * queued tasks were given. Local wait: what the waker has left plus the local
 * sum. Shared wait: the shared sum over the CPUs the wakee may use.
 * 1 measures only; 2 places a wakee on the shared DSQ when that wait is
 * shorter; 3 also credits the local CPU with the cache benefit learned per
 * (waker, wakee) program pair: how much shorter the wakee's burst was after
 * local placement.
 */
#define PRED_PLACE 0
/*
 * PP_MARGIN: move a wakee to the shared DSQ only when the local wait exceeds
 * the shared one by more than the models' recent mean error.
 * PP_FEAT_ALL: replace the local DSQ length feature with the expected work on
 * all local DSQs per CPU the wakee may use (CPUs drain them first).
 */
/*
 * PP_PIECE: one local-wait model per range of the local DSQ length
 * (0, 1, 2-3, 4-7, 8-15, 16-31, 32+), so the wait can grow faster past a
 * length than below it.
 */
#define PP_PIECE 0
/*
 * PP_FLIPS: replace the local DSQ length feature with how many different
 * tasks the waker has woken lately (halved every second), as the kernel's
 * wakee_flips does for wake_wide().
 */
#define PP_FLIPS 0
/*
 * PP_COMPLETE: train the models on the wait plus the burst that followed, so
 * a placement that runs slower (a cold cache) also counts against it.
 */
#define PP_COMPLETE 0
/*
 * PP_EXPLORE: take the other place for one sync wakeup in PP_EXPLORE, so a
 * model whose place is never chosen still gets measured waits to learn from.
 */
#define PP_EXPLORE 0
u64 nr_pp_explore;
#define PP_MARGIN 0
#define PP_FEAT_ALL 0
u64 pp_abserr_local, pp_abserr_shared;
s64 pp_all_local;
s64 pp_local_work[LERTICK_MAX_CPUS];
s64 pp_shared_work[LERTICK_MAX_DOMAINS];
u64 nr_pp_sync, nr_pp_pick_local, nr_pp_pick_shared;
u64 pp_n[4], pp_wait_ns[4], pp_err_ns[4], pp_pred_ns[4]; /* [1] local, [2] shared */

struct pp_key {
	char waker[16];
	char wakee[16];
};
struct pp_val {
	u64 burst[4];
	u64 n[4];
};
struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 4096);
	__type(key, struct pp_key);
	__type(value, struct pp_val);
} pp_stor SEC(".maps");

static __always_inline s64 pp_read(s64 *v)
{
	s64 x = *v;

	return x > 0 ? x : 0;
}

static __always_inline void pp_add(struct task_ctx *tctx, u8 where, u32 idx,
				   s64 amt, u64 pred)
{
	if (where == 1) {
		__sync_fetch_and_add(&pp_local_work[idx & (LERTICK_MAX_CPUS - 1)],
				     amt);
		__sync_fetch_and_add(&pp_all_local, amt);
	}
	else
		__sync_fetch_and_add(
			&pp_shared_work[idx & (LERTICK_MAX_DOMAINS - 1)], amt);
	tctx->pp_amt   = amt;
	tctx->pp_idx   = idx;
	tctx->pp_where = where;
	tctx->pp_ts    = bpf_ktime_get_ns();
	tctx->pp_pred  = pred;
}

/* Expected CPU time of @tctx's current run: its average burst less what ran. */
static __always_inline u64 pp_expect(struct task_ctx *tctx, u64 ran)
{
	u64 e = tctx->ewma_burst_ns ? tctx->ewma_burst_ns : slice_fixed;

	return e > ran ? e - ran : 0;
}

/*
 * Wait predicted by a linear model per place (0: the CPU's local DSQ, 1: the
 * shared DSQ), trained on every measured wait. Features, as fractions:
 * 1, the local DSQ's expected work, its length, the shared DSQ's length per
 * CPU the task may use, and what the waker has left to run.
 */
#define PP_WAIT_REF (32ULL * NSEC_PER_MSEC)
struct lertick_model pp_model_local, pp_model_shared;
struct lertick_model pp_ml0, pp_ml1, pp_ml2, pp_ml3, pp_ml4, pp_ml5, pp_ml6;

static __always_inline u8 pp_bucket(u64 n)
{
	return n == 0 ? 0 : n < 2 ? 1 : n < 4 ? 2 : n < 8 ? 3 : n < 16 ? 4 :
	       n < 32 ? 5 : 6;
}

static __always_inline struct lertick_model *pp_local_model(u8 b)
{
	if (!PP_PIECE)
		return &pp_model_local;
	if (b == 0)
		return &pp_ml0;
	if (b == 1)
		return &pp_ml1;
	if (b == 2)
		return &pp_ml2;
	if (b == 3)
		return &pp_ml3;
	if (b == 4)
		return &pp_ml4;
	if (b == 5)
		return &pp_ml5;
	return &pp_ml6;
}

static __always_inline __s32 pp_q(u64 v, u64 ref)
{
	return v >= ref ? LERTICK_Q : (__s32)(v * LERTICK_Q / ref);
}

static __always_inline void pp_features(__s32 *f, s32 cpu, u32 d, u64 ncpu,
					u64 rest, u64 flips)
{
	u64 nl = scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL_ON | cpu);
	u64 ns = scx_bpf_dsq_nr_queued(d);

	f[0] = LERTICK_Q;
	f[1] = pp_q(pp_read(&pp_local_work[cpu & (LERTICK_MAX_CPUS - 1)]),
		    PP_WAIT_REF);
	if (PP_FLIPS)
		f[2] = pp_q(flips, 64);
	else if (PP_FEAT_ALL)
		f[2] = pp_q(pp_read(&pp_all_local) / (ncpu ? ncpu : 1), PP_WAIT_REF);
	else
		f[2] = pp_q(nl, 64);
	f[3] = pp_q(ns * 4 / (ncpu ? ncpu : 1), 64);
	f[4] = pp_q(rest, 10ULL * NSEC_PER_MSEC);
}

static __always_inline u64 pp_predict(bool shared, const __s32 *f, u8 b)
{
	struct lertick_model *m = shared ? &pp_model_shared : pp_local_model(b);

	return (u64)lertick_model_predict_q16(m, f) * PP_WAIT_REF / LERTICK_Q;
}

static __always_inline void pp_remove(struct task_ctx *tctx)
{
	if (tctx->pp_where == 1) {
		__sync_fetch_and_sub(
			&pp_local_work[tctx->pp_idx & (LERTICK_MAX_CPUS - 1)],
			tctx->pp_amt);
		__sync_fetch_and_sub(&pp_all_local, tctx->pp_amt);
	}
	else if (tctx->pp_where == 2)
		__sync_fetch_and_sub(
			&pp_shared_work[tctx->pp_idx & (LERTICK_MAX_DOMAINS - 1)],
			tctx->pp_amt);
	tctx->pp_where = 0;
}

/*
 * LERTICK_MODEL_V2: the model starts as the burst average (weight 1 on that
 * feature, 0 elsewhere), so it predicts no worse than the average before it
 * has learned anything, and learns with a step that starts large and shrinks
 * (1/8 for 8 updates, 1/16 up to 32, then the usual 1/64).
 */
static void model_train_v2(struct lertick_model *model,
			   const __s32 features[LERTICK_FEATURES],
			   __s32 target_q16)
{
	__s64 error = (__s64)lertick_clip_q16(target_q16) -
		      lertick_model_raw_q16(model, features);
	__s64 alpha = model->updates < 8  ? LERTICK_Q / 8 :
		      model->updates < 32 ? LERTICK_Q / 16 :
					    LERTICK_ALPHA_Q16;
	int   i;

	if (error > LERTICK_Q)
		error = LERTICK_Q;
	if (error < -LERTICK_Q)
		error = -LERTICK_Q;
	bpf_for(i, 0, LERTICK_FEATURES)
	{
		__s64 update;

		if (i >= LERTICK_FEATURES)
			break;
		update = lertick_trunc_shift(
			alpha * error * lertick_clip_q16(features[i]), 32);
		model->weight_q16[i] = lertick_clip_weight(
			(__s64)lertick_clip_weight(model->weight_q16[i]) +
			update);
	}
	if (model->updates != ~(__u64)0)
		model->updates++;
}

/*
 * A wakeup starts an episode. Only an episode that starts with a verified
 * wakeup and ends in sleep is a training example: a task that keeps running
 * across slices is not mistaken for one that completed a burst.
 */
#ifndef LERTICK_TRACE
#define LERTICK_TRACE 0
#endif
/*
 * Experimental: the burst feature is the last burst started by the same
 * waker, when one of the last four wakers woke this task before. A thread
 * that alternates between two kinds of work, each started by a different
 * waker, then gets the right length for each.
 */
#ifndef LERTICK_WAKER_FEAT
#define LERTICK_WAKER_FEAT 0
#endif
/* Start new models from the burst feature instead of the average. */
#ifndef WAKER_INIT
#define WAKER_INIT 0
#endif
/* Share of the initial weight on the burst feature: 0, half or all. */
#define WAKER_INIT_Q (WAKER_INIT == 2 ? LERTICK_Q / 2 : WAKER_INIT ? LERTICK_Q : 0)
/*
 * Per task, use the same-waker burst only while it has been the better
 * guess recently; otherwise the burst average.
 */
#ifndef WAKER_PICK
#define WAKER_PICK 0
#endif
/* Keep a decaying average per waker instead of its last burst. */
#ifndef WAKER_EWMA
#define WAKER_EWMA 0
#endif
u64 nr_waker_hits;

static __always_inline int waker_slot(struct task_ctx *tctx)
{
	int i, slot = -1;

	bpf_for(i, 0, 4) {
		if (tctx->wk_pid[i & 3] == tctx->waker)
			slot = i & 3;
	}
	return slot;
}
u64 t_n, t_switch, t_wk_found, t_cls_wk, t_err_wk;

static void begin_episode(struct task_ctx *tctx, u64 enq_flags)
{
	if (LERTICK_TRACE || LERTICK_WAKER_FEAT)
		tctx->waker = (u32)bpf_get_current_pid_tgid();
	u64   now      = bpf_ktime_get_ns();
	u64   sleep_ns = tctx->last_sleep_ns ? now - tctx->last_sleep_ns : 0;
	u64   queued   = nr_queued_all();
	__s32 pred;

	tctx->episode_active = true;
	tctx->episode_valid  = tctx->has_slept && (enq_flags & SCX_ENQ_WAKEUP);
	tctx->episode_runtime_ns = 0;
	tctx->prediction_ns	 = 0;

	tctx->features[0]	 = LERTICK_Q;
	tctx->features[1] =
		lertick_scale_ns_q16(tctx->last_burst_ns, REFERENCE_NS);
	if (LERTICK_WAKER_FEAT && WAKER_PICK)
		tctx->features[1] = lertick_scale_ns_q16(tctx->ewma_burst_ns,
							 REFERENCE_NS);
	if (LERTICK_WAKER_FEAT) {
		int slot = waker_slot(tctx);

		if (slot >= 0 && (!WAKER_PICK || tctx->e_wk < tctx->e_avg)) {
			tctx->features[1] = lertick_scale_ns_q16(
				tctx->wk_burst[slot & 3], REFERENCE_NS);
			__sync_fetch_and_add(&nr_waker_hits, 1);
		}
	}
	tctx->features[2] =
		lertick_scale_ns_q16(tctx->ewma_burst_ns, REFERENCE_NS);
	tctx->features[3] = lertick_scale_ns_q16(sleep_ns, REFERENCE_NS);
	tctx->features[4] = queued >= QUEUE_REFERENCE ?
				    LERTICK_Q :
				    queued * LERTICK_Q / QUEUE_REFERENCE;

	if (mode != LERTICK_MODE_ONLINE || !tctx->episode_valid)
		return;
	pred = lertick_model_predict_q16(&tctx->model, tctx->features);
	tctx->prediction_ns = (u64)pred * REFERENCE_NS / LERTICK_Q;
}

#ifndef LERTICK_SLICE_FINISH
#define LERTICK_SLICE_FINISH 0
#endif
/*
 * Experimental (DEADLINE): order the queue by a virtual deadline, the vtime
 * plus the slice the task is given scaled by its weight, as EEVDF does. A
 * task predicted to run briefly gets a short slice and so an earlier
 * deadline. The offset is removed when the task starts running, so the
 * charge and the clock still use the vtime alone. DEADLINE_SCALE (percent)
 * scales the offset.
 */

/* A burst predicted past slice_max gets slice_fixed, not slice_max. */
#ifndef LONG_TO_FIXED
#define LONG_TO_FIXED 0
#endif
/* With LONG_TO_FIXED: the slice of such a burst in us, 0 for slice_fixed. */
#ifndef LONG_SLICE_US
#define LONG_SLICE_US 0
#endif
/*
 * Experimental: a wakee predicted to run less than SHORT_KICK_US preempts the
 * task on its CPU when that task is predicted past slice_max and has run for
 * KICK_RAN_US.
 */
#ifndef SHORT_KICK
#define SHORT_KICK 0
#endif
#ifndef SHORT_KICK_US
#define SHORT_KICK_US 500
#endif
#ifndef KICK_RAN_US
#define KICK_RAN_US 1000
#endif
u64 nr_short_kicks;

/* Remaining slice: the predicted burst less what already ran, within bounds. */
static u64 task_slice(struct task_ctx *tctx, u64 consumed_ns)
{
	u64 slice = slice_fixed;

	if (tctx && mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	    model_trusted(tctx) &&
	    tctx->prediction_ns > consumed_ns) {
		u64 rest = tctx->prediction_ns - consumed_ns;

		if (LONG_TO_FIXED && tctx->prediction_ns > slice_max) {
			/*
			 * The burst will be cut anyway: a longer slice would
			 * only make waiting tasks wait longer.
			 */
			if (LONG_SLICE_US)
				slice = LONG_SLICE_US * NSEC_PER_USEC;
		} else if (!LERTICK_SLICE_FINISH) {
			slice = rest;
			__sync_fetch_and_add(&nr_adaptive_slices, 1);
		} else if (rest > slice_fixed && rest <= slice_max) {
			/*
			 * A burst predicted to end a little past the fixed
			 * slice gets the rest of it, plus an eighth for the
			 * prediction's error, so that it ends without being
			 * preempted and queued again. Shorter bursts end
			 * within the fixed slice anyway, and longer ones would
			 * only make waiting tasks wait longer.
			 */
			slice = rest + rest / 8;
			__sync_fetch_and_add(&nr_adaptive_slices, 1);
		}
	}

	return CLAMP(slice, slice_min, slice_max);
}

/*
 * Pick a CPU for a waking task. An idle CPU, or the waker's CPU on a sync
 * wakeup that sync_local() accepts, gets the task on its local DSQ right
 * away; otherwise ops.enqueue() queues it.
 */
static bool same_comm(const char *a, const char *b)
{
	u64 x[2], y[2];

	__builtin_memcpy(x, a, sizeof(x));
	__builtin_memcpy(y, b, sizeof(y));
	return x[0] == y[0] && x[1] == y[1];
}

/* When @p runs a program its model is not synced to, take the program's. */
static void prog_sync(struct task_struct *p, struct task_ctx *tctx)
{
	struct prog_key	 key = {};
	struct prog_val *v;

	if (!LERTICK_PROG_TABLE || same_comm(tctx->comm, p->comm))
		return;
	__builtin_memcpy(tctx->comm, p->comm, sizeof(tctx->comm));
	__builtin_memcpy(key.comm, p->comm, sizeof(key.comm));
	v = bpf_map_lookup_elem(&prog_stor, &key);
	if (!v || v->model.updates < PROG_MIN_UPDATES)
		return;
	tctx->model	    = v->model;
	tctx->ewma_burst_ns = v->ewma_burst_ns;
	tctx->inherited	    = true;
	/*
	 * Keep the program's weights but not its update count, so the task
	 * still learns with a large step and adapts to its own behaviour
	 * (threads of one program can behave differently).
	 */
	if (tctx->model.updates > PROG_INHERIT_UPDATES)
		tctx->model.updates = PROG_INHERIT_UPDATES;
	__sync_fetch_and_add(&nr_prog_hits, 1);
}

static void model_train_v2(struct lertick_model *model,
			   const __s32 features[LERTICK_FEATURES],
			   __s32 target_q16);

/* Train @p's program model with the burst that just completed. */
static void prog_learn(struct task_struct *p, struct task_ctx *tctx,
		       __s32 target, u64 burst)
{
	struct prog_key	 key = {};
	struct prog_val *v;

	if (!LERTICK_PROG_TABLE)
		return;
	__builtin_memcpy(key.comm, p->comm, sizeof(key.comm));
	v = bpf_map_lookup_elem(&prog_stor, &key);
	if (!v) {
		struct prog_val init = {};

		lertick_model_init(&init.model);
		if (LERTICK_MODEL_V2) {
			init.model.weight_q16[0] = 0;
			init.model.weight_q16[2] = LERTICK_Q - WAKER_INIT_Q;
			init.model.weight_q16[1] = WAKER_INIT_Q;
		}
		bpf_map_update_elem(&prog_stor, &key, &init, BPF_NOEXIST);
		v = bpf_map_lookup_elem(&prog_stor, &key);
		if (!v)
			return;
	}
	if (LERTICK_MODEL_V2)
		model_train_v2(&v->model, tctx->features, target);
	else
		lertick_model_train(&v->model, tctx->features, target);
	v->ewma_burst_ns = v->ewma_burst_ns ?
				   v->ewma_burst_ns * 3 / 4 + burst / 4 :
				   burst;
}

/*
 * Measurement only (MEASURE_WAKE): how long a waker keeps running after its
 * last wakeup before it sleeps, whether the SCX_WAKE_SYNC hint predicts a
 * sleep within 50 us, and how well the last value and a decaying average do.
 */
#ifndef MEASURE_WAKE
#define MEASURE_WAKE 0
#endif
/*
 * Experimental (LEARNED_SYNC): treat a wakeup as sync, as SCX_WAKE_SYNC
 * does, also when the waker slept within RAL_SHORT_NS of its last wakeup the
 * previous time, so the wakee can take the waker's CPU when it sleeps.
 */
#ifndef LEARNED_SYNC
#define LEARNED_SYNC 0
#endif
#define RAL_SHORT_NS (50ULL * NSEC_PER_USEC)
u64 w_bursts, w_wakes, w_hist[5], w_sync_n, w_sync_ok, w_nosync_n, w_nosync_ok;
u64 w_pred_n, w_last_ok, w_ewma_ok, w_err_last, w_err_ewma;

static __always_inline void note_wake(u64 wake_flags)
{
	struct task_struct *cur = bpf_get_current_task_btf();
	struct task_ctx	   *w	= task_ctx(cur);

	if (!w || !w->running)
		return;
	w->wk_last_ts	= bpf_ktime_get_ns();
	w->wk_last_sync = wake_flags & SCX_WAKE_SYNC;
	w->wk_count++;
}

static __always_inline void note_sleep(struct task_ctx *t)
{
	u64  ral;
	bool shrt;

	if (!t->wk_count)
		return;
	ral  = bpf_ktime_get_ns() - t->wk_last_ts;
	shrt = ral < RAL_SHORT_NS;
	if (!MEASURE_WAKE)
		goto out;
	__sync_fetch_and_add(&w_bursts, 1);
	__sync_fetch_and_add(&w_wakes, t->wk_count);
	if (ral < 10 * NSEC_PER_USEC)
		__sync_fetch_and_add(&w_hist[0], 1);
	else if (shrt)
		__sync_fetch_and_add(&w_hist[1], 1);
	else if (ral < 200 * NSEC_PER_USEC)
		__sync_fetch_and_add(&w_hist[2], 1);
	else if (ral < NSEC_PER_MSEC)
		__sync_fetch_and_add(&w_hist[3], 1);
	else
		__sync_fetch_and_add(&w_hist[4], 1);
	if (t->wk_last_sync) {
		__sync_fetch_and_add(&w_sync_n, 1);
		if (shrt)
			__sync_fetch_and_add(&w_sync_ok, 1);
	} else {
		__sync_fetch_and_add(&w_nosync_n, 1);
		if (!shrt)
			__sync_fetch_and_add(&w_nosync_ok, 1);
	}
	if (t->ral_valid) {
		__sync_fetch_and_add(&w_pred_n, 1);
		if ((t->ral_last < RAL_SHORT_NS) == shrt)
			__sync_fetch_and_add(&w_last_ok, 1);
		if ((t->ral_ewma < RAL_SHORT_NS) == shrt)
			__sync_fetch_and_add(&w_ewma_ok, 1);
		__sync_fetch_and_add(&w_err_last, t->ral_last > ral ?
							  t->ral_last - ral :
							  ral - t->ral_last);
		__sync_fetch_and_add(&w_err_ewma, t->ral_ewma > ral ?
							  t->ral_ewma - ral :
							  ral - t->ral_ewma);
	}
out:
	t->ral_ewma  = t->ral_valid ? t->ral_ewma * 3 / 4 + ral / 4 : ral;
	t->ral_last  = ral;
	t->ral_valid = true;
	t->wk_count  = 0;
}

/*
 * Measurement only (MEASURE_WAKE2): at each wakeup a task issues, predict how
 * long the waker keeps running before it sleeps, with the SCX_WAKE_SYNC hint,
 * the last value, a decaying average, the last value after waking the same
 * wakee (pair table, 4 per task), a linear model over [1, sync, last,
 * average, wakeups so far in this burst], and the same model with the pair
 * value in place of the last. At sleep, score each against the run to that
 * sleep, as "within RAL_SHORT_NS" or not, and train both models.
 */
#ifndef MEASURE_WAKE2
#define MEASURE_WAKE2 0
#endif
#define RAL_REF_NS NSEC_PER_MSEC
u64 v_n, v_short, v_hint_ok, v_last_ok, v_ewma_ok, v_pair_ok, v_lin_ok,
	v_linp_ok, v_pair_hit, v_err_last, v_err_lin, v_err_linp, v_err_pair;

static __always_inline s32 ral_q(u64 ns)
{
	return lertick_scale_ns_q16(ns, RAL_REF_NS);
}

static __always_inline void wake2_note(struct task_struct *p, u64 wake_flags)
{
	struct task_ctx *w = task_ctx(bpf_get_current_task_btf());
	struct wev	*e;
	u32		 n, i;
	int		 slot = -1;
	__s32		 pred;

	if (!w || !w->running)
		return;
	n = w->nev;
	if (n >= NR_WEV)
		return;
	e	  = &w->ev[n & (NR_WEV - 1)];
	e->ts	  = bpf_ktime_get_ns();
	e->wakee  = p->pid;
	e->sync	  = !!(wake_flags & SCX_WAKE_SYNC);
	e->f[0]	  = LERTICK_Q;
	e->f[1]	  = e->sync ? LERTICK_Q : 0;
	e->f[2]	  = ral_q(w->rem_last);
	e->f[3]	  = ral_q(w->rem_ewma);
	e->f[4]	  = n * LERTICK_Q / NR_WEV;
	bpf_for(i, 0, 4)
	{
		if (w->pp_pid[i & 3] == p->pid)
			slot = i & 3;
	}
	e->p_pair = slot >= 0 ? w->pp_val[slot & 3] : w->rem_last;
	if (slot >= 0)
		__sync_fetch_and_add(&v_pair_hit, 1);
	bpf_for(i, 0, LERTICK_FEATURES)
	{
		if (i >= LERTICK_FEATURES)
			break;
		e->fp[i] = e->f[i];
	}
	e->fp[2]  = ral_q(e->p_pair);
	e->p_last = w->rem_last;
	e->p_ewma = w->rem_ewma;
	pred	  = lertick_model_predict_q16(&w->ml, e->f);
	e->p_lin  = (u64)pred * RAL_REF_NS / LERTICK_Q;
	pred	  = lertick_model_predict_q16(&w->mlp, e->fp);
	e->p_linp = (u64)pred * RAL_REF_NS / LERTICK_Q;
	w->nev	  = n + 1;
}

#define ABSD(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))
#define SHORT(x) ((x) < RAL_SHORT_NS)

static __always_inline void wake2_sleep(struct task_ctx *t)
{
	u64 now = bpf_ktime_get_ns();
	u32 i, k;

	bpf_for(i, 0, NR_WEV)
	{
		struct wev *e;
		u64	    rem;
		bool	    shrt;
		int	    slot = -1;

		if (i >= t->nev)
			break;
		e    = &t->ev[i & (NR_WEV - 1)];
		rem  = now > e->ts ? now - e->ts : 0;
		shrt = SHORT(rem);
		__sync_fetch_and_add(&v_n, 1);
		if (shrt)
			__sync_fetch_and_add(&v_short, 1);
		if (!!e->sync == shrt)
			__sync_fetch_and_add(&v_hint_ok, 1);
		if (t->rem_valid) {
			if (SHORT(e->p_last) == shrt)
				__sync_fetch_and_add(&v_last_ok, 1);
			if (SHORT(e->p_ewma) == shrt)
				__sync_fetch_and_add(&v_ewma_ok, 1);
			if (SHORT(e->p_pair) == shrt)
				__sync_fetch_and_add(&v_pair_ok, 1);
			if (SHORT(e->p_lin) == shrt)
				__sync_fetch_and_add(&v_lin_ok, 1);
			if (SHORT(e->p_linp) == shrt)
				__sync_fetch_and_add(&v_linp_ok, 1);
			__sync_fetch_and_add(&v_err_last, ABSD(MIN(e->p_last, RAL_REF_NS), MIN(rem, RAL_REF_NS)));
			__sync_fetch_and_add(&v_err_pair, ABSD(MIN(e->p_pair, RAL_REF_NS), MIN(rem, RAL_REF_NS)));
			__sync_fetch_and_add(&v_err_lin, ABSD(e->p_lin, MIN(rem, RAL_REF_NS)));
			__sync_fetch_and_add(&v_err_linp, ABSD(e->p_linp, MIN(rem, RAL_REF_NS)));
		}
		model_train_v2(&t->ml, e->f, ral_q(rem));
		model_train_v2(&t->mlp, e->fp, ral_q(rem));
		bpf_for(k, 0, 4)
		{
			if (t->pp_pid[k & 3] == e->wakee)
				slot = k & 3;
		}
		if (slot < 0) {
			slot = t->pp_next & 3;
			t->pp_next++;
			t->pp_pid[slot & 3] = e->wakee;
		}
		t->pp_val[slot & 3] = rem;
		t->rem_ewma = t->rem_valid ? t->rem_ewma * 3 / 4 + rem / 4 : rem;
		t->rem_last = rem;
		t->rem_valid = true;
	}
	t->nev = 0;
}

s32 BPF_STRUCT_OPS(lertick_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	bool		 idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	if (MEASURE_WAKE || LEARNED_SYNC)
		note_wake(wake_flags);
	if (MEASURE_WAKE2)
		wake2_note(p, wake_flags);
	if (LEARNED_SYNC && !(wake_flags & SCX_WAKE_SYNC)) {
		struct task_ctx *w = task_ctx(bpf_get_current_task_btf());

		if (w && w->running && w->ral_valid &&
		    w->ral_last < RAL_SHORT_NS &&
		    (LEARNED_SYNC < 2 ||
		     !scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL)))
			wake_flags |= SCX_WAKE_SYNC;
	}
	/* select_cpu() precedes runnable() on a wakeup and can insert directly. */
	if (tctx && !tctx->episode_active) {
		prog_sync(p, tctx);
		begin_episode(tctx, SCX_ENQ_WAKEUP);
		tctx->started_in_select = true;
	}
	bool sync_ok = !idle && (wake_flags & SCX_WAKE_SYNC) && sync_local(p, &cpu);
	u64  pred_local = 0;

	if (PRED_PLACE && tctx)
		tctx->pp_waker[0] = 0;
	if (PRED_PLACE && sync_ok && tctx) {
		struct task_struct *w  = bpf_get_current_task_btf();
		struct task_ctx	   *wt = task_ctx(w);
		u64 rest = 0, pred_shared, ncpu = p->nr_cpus_allowed;
		s64 benefit = 0;

		if (wt && wt->running && w->se.sum_exec_runtime >= wt->runtime_start_ns)
			rest = pp_expect(wt, wt->episode_runtime_ns +
				w->se.sum_exec_runtime - wt->runtime_start_ns);
		u64 flips = 0;

		if (wt) {
			u64 now = bpf_ktime_get_ns();

			if (now - wt->pp_flip_ts > NSEC_PER_SEC) {
				wt->pp_flips >>= 1;
				wt->pp_flip_ts = now;
			}
			if (wt->pp_last_wakee != p->pid) {
				wt->pp_last_wakee = p->pid;
				wt->pp_flips++;
			}
			flips = wt->pp_flips;
		}
		pp_features(tctx->pp_feat, cpu, domain_of(cpu), ncpu, rest, flips);
		tctx->pp_feat_ok = true;
		tctx->pp_bucket	 = pp_bucket(scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL_ON | cpu));
		pred_local  = pp_predict(false, tctx->pp_feat, tctx->pp_bucket);
		pred_shared = pp_predict(true, tctx->pp_feat, 0);
		__builtin_memcpy(tctx->pp_waker, w->comm, sizeof(tctx->pp_waker));
		if (PRED_PLACE >= 3) {
			struct pp_key k = {};
			struct pp_val *v;

			__builtin_memcpy(k.waker, w->comm, sizeof(k.waker));
			__builtin_memcpy(k.wakee, p->comm, sizeof(k.wakee));
			v = bpf_map_lookup_elem(&pp_stor, &k);
			if (v && v->n[1] >= 4 && v->n[2] >= 4)
				benefit = (s64)v->burst[2] - (s64)v->burst[1];
		}
		__sync_fetch_and_add(&nr_pp_sync, 1);
		if (PP_MARGIN)
			benefit += (s64)((pp_abserr_local + pp_abserr_shared) / 2);
		bool want_shared = (s64)pred_local > (s64)pred_shared + benefit;

		if (PP_EXPLORE && !(bpf_get_prandom_u32() % (PP_EXPLORE ? PP_EXPLORE : 1))) {
			want_shared = !want_shared;
			__sync_fetch_and_add(&nr_pp_explore, 1);
		}
		if (want_shared) {
			__sync_fetch_and_add(&nr_pp_pick_shared, 1);
			if (PRED_PLACE >= 2)
				sync_ok = false; /* enqueue() queues it on the shared DSQ */
		} else {
			__sync_fetch_and_add(&nr_pp_pick_local, 1);
		}
	}
	if (STEAL_PCPU && !idle && sync_ok) {
		u64 slice =
			task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

		scx_bpf_dsq_insert_vtime(p, PCPU_DSQ(cpu), slice,
					 p->scx.dsq_vtime, 0);
		__sync_fetch_and_add(&nr_pcpu_inserts, 1);
		if (STEAL_PCPU >= 2 && scx_bpf_dsq_nr_queued(PCPU_DSQ(cpu)) > 1) {
			s32 c = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);

			if (c >= 0) {
				scx_bpf_kick_cpu(c, SCX_KICK_IDLE);
				__sync_fetch_and_add(&nr_pcpu_kicks, 1);
			}
		}
		return cpu;
	}
	if (idle || sync_ok) {
		u64 slice =
			task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		__sync_fetch_and_add(&nr_local_dispatches, 1);
		if (PRED_PLACE && tctx) {
			if (idle)
				tctx->pp_feat_ok = false;
			if (tctx->pp_where)
				pp_remove(tctx);
			pp_add(tctx, 1, cpu, pp_expect(tctx, tctx->episode_runtime_ns),
			       idle ? 0 : pred_local);
		}
	}

	return cpu;
}

/*
 * Queue @p on its LLC's DSQ in vtime order, with its vtime held within the
 * lag bounds.
 */
/*
 * Experimental (SHORT_KICK): whether @cpu runs a task predicted past
 * slice_max that has run for KICK_RAN_US; returns how long it has run, or 0.
 */
static __always_inline u64 long_runner(s32 cpu, struct task_struct *p)
{
	struct task_struct *cur = __COMPAT_scx_bpf_cpu_curr(cpu);
	struct task_ctx	   *cc;
	u64		    ran;

	if (!cur || cur == p)
		return 0;
	cc = task_ctx(cur);
	if (!cc || !cc->running || cc->model.updates < min_train_samples ||
	    cc->prediction_ns <= slice_max ||
	    cur->se.sum_exec_runtime < cc->runtime_start_ns)
		return 0;
	ran = cur->se.sum_exec_runtime - cc->runtime_start_ns;
	return ran >= KICK_RAN_US * NSEC_PER_USEC ? ran : 0;
}

/*
 * SHORT_KICK 1: preempt the long runner on @p's CPU. SHORT_KICK 2: preempt the
 * long runner that has run longest among the CPUs @p may use in its domain.
 */
static __always_inline void short_kick(struct task_struct *p)
{
	s32 cpu = scx_bpf_task_cpu(p), best = -1, c;
	u64 best_ran = 0, ran;
	u32 home = domain_of(cpu);

	if (SHORT_KICK == 1) {
		if (long_runner(cpu, p)) {
			scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
			__sync_fetch_and_add(&nr_short_kicks, 1);
		}
		return;
	}
	bpf_for(c, 0, LERTICK_MAX_CPUS)
	{
		if (c >= scx_bpf_nr_cpu_ids())
			break;
		if (domain_of(c) != home || !bpf_cpumask_test_cpu(c, p->cpus_ptr))
			continue;
		ran = long_runner(c, p);
		if (ran > best_ran) {
			best_ran = ran;
			best	 = c;
		}
	}
	if (best >= 0) {
		scx_bpf_kick_cpu(best, SCX_KICK_PREEMPT);
		__sync_fetch_and_add(&nr_short_kicks, 1);
	}
}

/*
 * Experimental (WAKE_PREEMPT): a woken task (1: any trusted one, 2: one
 * predicted shorter than SHORT_KICK_US) whose vtime is before that of a CPU's
 * running task predicted past slice_max, which has run for KICK_RAN_US, goes
 * straight onto that CPU and preempts it. Of several such CPUs in its domain,
 * the one whose task has run longest is taken.
 */
#ifndef WAKE_PREEMPT
#define WAKE_PREEMPT 0
#endif
u64 nr_wake_preempts;

static __always_inline s32 wake_victim(struct task_struct *p, u64 vtime)
{
	s32 cpu = scx_bpf_task_cpu(p), best = -1, c;
	u64 best_ran = 0, ran;
	u32 home = domain_of(cpu);

	bpf_for(c, 0, LERTICK_MAX_CPUS)
	{
		struct task_struct *cur;

		if (c >= scx_bpf_nr_cpu_ids())
			break;
		if (domain_of(c) != home || !bpf_cpumask_test_cpu(c, p->cpus_ptr))
			continue;
		ran = long_runner(c, p);
		if (ran <= best_ran)
			continue;
		cur = __COMPAT_scx_bpf_cpu_curr(c);
		if (!cur || !time_before(vtime, cur->scx.dsq_vtime))
			continue;
		best_ran = ran;
		best	 = c;
	}
	return best;
}

void BPF_STRUCT_OPS(lertick_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx  = task_ctx(p);
	u64		 vtime = p->scx.dsq_vtime;
	u64 slice = task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

	/* A task dequeued without running still carries its offset. */
	if (DEADLINE && tctx && tctx->dl_off) {
		vtime -= tctx->dl_off;
		tctx->dl_off = 0;
	}

	/*
	 * Only a wakeup earns sleeper credit (LAG_NS); a task requeued after its
	 * slice keeps its vtime within the wider bounds of clamp_ahead(), or
	 * tasks with larger weights would be pulled back to the clock and lose
	 * their share.
	 */
	if (MEASURE_CLAMP)
		__sync_fetch_and_add(&c_enq, 1);
	if ((enq_flags & SCX_ENQ_WAKEUP) &&
	    time_before(vtime, vtime_now - LAG_NS)) {
		vtime = vtime_now - LAG_NS;
		if (MEASURE_CLAMP)
			__sync_fetch_and_add(&c_lag, 1);
	}
	vtime = clamp_ahead(p, vtime);

	if (WAKE_PREEMPT && (enq_flags & SCX_ENQ_WAKEUP) && tctx &&
	    mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	    tctx->model.updates >= min_train_samples &&
	    (WAKE_PREEMPT == 1 ||
	     tctx->prediction_ns < SHORT_KICK_US * NSEC_PER_USEC)) {
		s32 victim = wake_victim(p, vtime);

		if (victim >= 0) {
			p->scx.dsq_vtime = vtime;
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | victim, slice,
					   enq_flags | SCX_ENQ_PREEMPT);
			__sync_fetch_and_add(&nr_wake_preempts, 1);
			return;
		}
	}

	if (DEADLINE && tctx) {
		u32 w = tctx->eff_weight ? tctx->eff_weight :
			p->scx.weight	 ? p->scx.weight :
					   1;

		tctx->dl_off = slice * DEADLINE_SCALE / w;
		vtime += tctx->dl_off;
	}
	scx_bpf_dsq_insert_vtime(p, domain_of(scx_bpf_task_cpu(p)), slice,
				 vtime, enq_flags);
	__sync_fetch_and_add(&nr_shared_dispatches, 1);
	if (PRED_PLACE && tctx) {
		u32 d = domain_of(scx_bpf_task_cpu(p));
		u64 n = p->nr_cpus_allowed;

		if (tctx->pp_where)
			pp_remove(tctx);
		pp_features(tctx->pp_feat, scx_bpf_task_cpu(p), d, n, 0, 0);
		tctx->pp_feat_ok = true;
		pp_add(tctx, 2, d, pp_expect(tctx, tctx->episode_runtime_ns),
		       pp_predict(true, tctx->pp_feat, 0));
	}
	/* ENQ_KICK (experiment): wake an idle CPU to take the queued task. */
	if (ENQ_KICK) {
		s32 idle_cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);

		if (idle_cpu >= 0) {
			scx_bpf_kick_cpu(idle_cpu, SCX_KICK_IDLE);
			__sync_fetch_and_add(&nr_enq_kicks, 1);
		}
	}

	if (SHORT_KICK && (enq_flags & SCX_ENQ_WAKEUP) && tctx &&
	    mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	    tctx->model.updates >= min_train_samples &&
	    tctx->prediction_ns < SHORT_KICK_US * NSEC_PER_USEC)
		short_kick(p);
}

/* Own per-CPU DSQ first (STEAL_PCPU), then the shared DSQs, then steal. */
static __always_inline bool take_next_pcpu(s32 cpu)
{
	if (STEAL_PCPU && LERTICK_MOVE_TO_LOCAL(PCPU_DSQ(cpu))) {
		__sync_fetch_and_add(&nr_pcpu_own, 1);
		return true;
	}
	if (take_next(cpu))
		return true;
	return STEAL_PCPU && pcpu_steal(cpu);
}

/*
 * @cpu needs a task. @prev continues if keep_prev() allows it; otherwise the
 * next queued task runs, and @prev continues only when nothing is queued.
 */
void BPF_STRUCT_OPS(lertick_dispatch, s32 cpu, struct task_struct *prev)
{
	bool continuing = prev && (prev->scx.flags & SCX_TASK_QUEUED);
	struct task_ctx *tctx;
	u64		 consumed;

	if (!((continuing && keep_prev(prev, task_ctx(prev), cpu)) ||
	      (!take_next_pcpu(cpu) && continuing)))
		return;

	/*
	 * prev continues. The kernel may update sum_exec_runtime before
	 * dispatch() and keep prev without stopping() or running(), so read
	 * the pending segment for the slice decision without charging it:
	 * stopping() charges it.
	 */
	tctx	 = task_ctx(prev);
	consumed = tctx ? tctx->episode_runtime_ns : 0;
	if (tctx && tctx->running &&
	    prev->se.sum_exec_runtime > tctx->runtime_start_ns)
		consumed += prev->se.sum_exec_runtime - tctx->runtime_start_ns;

	scx_bpf_dsq_insert(prev, SCX_DSQ_LOCAL, task_slice(tctx, consumed), 0);
	__sync_fetch_and_add(&nr_continuations, 1);
}

/*
 * @p becomes runnable: count its weight toward the clock and its cgroup, and
 * start an episode unless ops.select_cpu() already did.
 */
void BPF_STRUCT_OPS(lertick_runnable, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;

	if (!tctx->rq_weight) {
		tctx->rq_weight = p->scx.weight ? p->scx.weight : 1;
		__sync_fetch_and_add(&runnable_weight, tctx->rq_weight);
		cgrp_task_runnable(p, tctx, true);
	}

	/* Migration can produce runnable notifications without a new wakeup. */
	if (!tctx->episode_active) {
		prog_sync(p, tctx);
		begin_episode(tctx, enq_flags);
	} else if (tctx->started_in_select) {
		/* Confirm the wakeup before admitting this episode as a label. */
		if (!(enq_flags & SCX_ENQ_WAKEUP))
			tctx->episode_valid = false;
		tctx->started_in_select = false;
	}
}

/*
 * @p starts running: raise the clock to its vtime if the clock is behind,
 * and note where this run starts.
 */
void BPF_STRUCT_OPS(lertick_running, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	if (DEADLINE && tctx && tctx->dl_off) {
		p->scx.dsq_vtime -= tctx->dl_off;
		tctx->dl_off = 0;
	}
	clock_raise(p->scx.dsq_vtime);

	if (!tctx)
		return;
	tctx->runtime_start_ns = p->se.sum_exec_runtime;
	tctx->running	       = true;
	if (STEAL_COST && !tctx->episode_runtime_ns)
		tctx->sc_mig = scx_bpf_task_cpu(p) != tctx->sc_last_cpu;
	if (PRED_PLACE && tctx->pp_where) {
		u8  wh	 = tctx->pp_where & 3;
		u64 wait = bpf_ktime_get_ns() - tctx->pp_ts;

		if (!tctx->episode_runtime_ns)
			tctx->pp_ran_where = wh;
		u64 err = wait > tctx->pp_pred ? wait - tctx->pp_pred :
						 tctx->pp_pred - wait;

		if (!tctx->episode_runtime_ns)
			tctx->pp_wait_last = wait;
		if (!PP_COMPLETE && tctx->pp_feat_ok && (wh == 1 || wh == 2)) {
			__s32 target = pp_q(wait, PP_WAIT_REF);

			if (wh == 1)
				model_train_v2(pp_local_model(tctx->pp_bucket), tctx->pp_feat, target);
			else
				model_train_v2(&pp_model_shared, tctx->pp_feat, target);
			tctx->pp_feat_ok = false;
		}
		if (wh == 1 && tctx->pp_pred)
			pp_abserr_local = pp_abserr_local * 15 / 16 + err / 16;
		else if (wh == 2)
			pp_abserr_shared = pp_abserr_shared * 15 / 16 + err / 16;
		if (wh == 1) {
			__sync_fetch_and_add(&pp_n[1], 1);
			__sync_fetch_and_add(&pp_wait_ns[1], wait);
			__sync_fetch_and_add(&pp_pred_ns[1], tctx->pp_pred);
			__sync_fetch_and_add(&pp_err_ns[1], err);
		} else if (wh == 2) {
			__sync_fetch_and_add(&pp_n[2], 1);
			__sync_fetch_and_add(&pp_wait_ns[2], wait);
			__sync_fetch_and_add(&pp_pred_ns[2], tctx->pp_pred);
			__sync_fetch_and_add(&pp_err_ns[2], err);
		}
		pp_remove(tctx);
	}
}

/*
 * @p stops running: charge the CPU time it used to its vtime, scaled by its
 * effective weight, and advance the clock.
 */
void BPF_STRUCT_OPS(lertick_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx	= task_ctx(p);
	u32		 weight = p->scx.weight ? p->scx.weight : 1;
	u64		 delta, rw;

	if (LERTICK_MEASURE) {
		__sync_fetch_and_add(&m_stops, 1);
		if (runnable)
			__sync_fetch_and_add(&m_expired, 1);
	}

	if (!tctx || !tctx->running)
		return;
	tctx->running = false;
	if (STEAL_COST)
		tctx->sc_last_cpu = scx_bpf_task_cpu(p);
	if (p->se.sum_exec_runtime < tctx->runtime_start_ns) {
		tctx->episode_valid = false;
		return;
	}
	delta		 = p->se.sum_exec_runtime - tctx->runtime_start_ns;

	tctx->eff_weight = cgrp_effective_weight(p, tctx, weight);
	p->scx.dsq_vtime += delta * 100 / tctx->eff_weight;

	/*
	 * Advance the clock by the service delivered over the runnable weight,
	 * as the average vtime of runnable tasks moves. Otherwise a stream of
	 * short sleepers, each placed just behind the clock, keeps it still and
	 * starves tasks that have already run.
	 */
	rw = runnable_weight;
	clock_advance(delta * 100 / (rw < 100 ? 100 : rw));
	tctx->episode_runtime_ns += delta;
}

/*
 * @p is no longer runnable. When it goes to sleep, the burst that just ended
 * becomes a training example for its model.
 */
void BPF_STRUCT_OPS(lertick_quiescent, struct task_struct *p, u64 deq_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	u64		 burst;

	if (!tctx)
		return;

	if (tctx->rq_weight) {
		cgrp_task_runnable(p, tctx, false);
		__sync_fetch_and_sub(&runnable_weight, tctx->rq_weight);
		tctx->rq_weight = 0;
	}

	/* Non-sleep quiescence keeps the episode across migration and preemption. */
	if (!(deq_flags & SCX_DEQ_SLEEP))
		return;
	if (MEASURE_WAKE || LEARNED_SYNC)
		note_sleep(tctx);
	if (MEASURE_WAKE2)
		wake2_sleep(tctx);

	/*
	 * A running task gets stopping(p, false) before this callback, so the
	 * last service segment is already in episode_runtime_ns exactly once.
	 */
	burst = tctx->episode_runtime_ns;
	if (tctx->episode_active && tctx->episode_valid && burst &&
	    !(p->flags & PF_EXITING)) {
		__s32 target = lertick_scale_ns_q16(burst, REFERENCE_NS);

		if (LERTICK_MEASURE) {
			u64  b	  = MIN(burst, REFERENCE_NS);
			bool tr	  = model_trusted(tctx);
			u64  used = tr ? tctx->prediction_ns : slice_fixed;

			__sync_fetch_and_add(&m_all, 1);
			__sync_fetch_and_add(&m_err_used,
					     used > b ? used - b : b - used);
			if ((used <= slice_fixed) == (b <= slice_fixed))
				__sync_fetch_and_add(&m_cls_used, 1);
		}
		if (LERTICK_MEASURE && model_trusted(tctx)) {
			u64 b	 = MIN(burst, REFERENCE_NS);
			u64 lin	 = tctx->prediction_ns;
			u64 ew	 = tctx->ewma_burst_ns;
			u64 last = MIN(tctx->last_burst_ns, REFERENCE_NS);
			bool fin = b <= slice_fixed;

			__sync_fetch_and_add(&m_bursts, 1);
			__sync_fetch_and_add(&m_err_lin, lin > b ? lin - b : b - lin);
			__sync_fetch_and_add(&m_err_ewma, ew > b ? ew - b : b - ew);
			__sync_fetch_and_add(&m_err_last,
					     last > b ? last - b : b - last);
			if (lin < b)
				__sync_fetch_and_add(&m_under_lin, 1);
			if (ew < b)
				__sync_fetch_and_add(&m_under_ewma, 1);
			if (burst > slice_fixed)
				__sync_fetch_and_add(&m_over_fixed, 1);
			if (burst > slice_max)
				__sync_fetch_and_add(&m_over_cap, 1);
			if ((lin <= slice_fixed) == fin)
				__sync_fetch_and_add(&m_cls_lin, 1);
			if ((ew <= slice_fixed) == fin)
				__sync_fetch_and_add(&m_cls_ewma, 1);
			if ((last <= slice_fixed) == fin)
				__sync_fetch_and_add(&m_cls_last, 1);
		}
		if (LERTICK_TRACE && tctx->model.updates >= min_train_samples) {
			u64  b	  = MIN(burst, REFERENCE_NS);
			u64  last = MIN(tctx->last_burst_ns, REFERENCE_NS);
			u64  wk	  = last;
			bool fin  = b <= slice_fixed;
			int  i, slot = -1;

			bpf_for(i, 0, 4) {
				if (tctx->wk_pid[i & 3] == tctx->waker) {
					slot = i & 3;
					wk   = tctx->wk_burst[i & 3];
				}
			}
			__sync_fetch_and_add(&t_n, 1);
			if ((last <= slice_fixed) != fin)
				__sync_fetch_and_add(&t_switch, 1);
			if (slot >= 0)
				__sync_fetch_and_add(&t_wk_found, 1);
			if ((wk <= slice_fixed) == fin)
				__sync_fetch_and_add(&t_cls_wk, 1);
			__sync_fetch_and_add(&t_err_wk, wk > b ? wk - b : b - wk);
			if (slot < 0) {
				slot = tctx->wk_next & 3;
				tctx->wk_next++;
				tctx->wk_pid[slot & 3] = tctx->waker;
			}
			tctx->wk_burst[slot & 3] = b;
		}

		if (LERTICK_WAKER_FEAT && !LERTICK_TRACE) {
			int slot = waker_slot(tctx);

			u64 b = MIN(burst, REFERENCE_NS);

			if (WAKER_PICK) {
				u64 g = slot >= 0 ? tctx->wk_burst[slot & 3] :
						    MIN(tctx->last_burst_ns,
							REFERENCE_NS);
				u64 a = tctx->ewma_burst_ns;

				tctx->e_wk = tctx->e_wk * 7 / 8 +
					     (g > b ? g - b : b - g) / 8;
				tctx->e_avg = tctx->e_avg * 7 / 8 +
					      (a > b ? a - b : b - a) / 8;
			}
			if (slot < 0) {
				slot = tctx->wk_next & 3;
				tctx->wk_next++;
				tctx->wk_pid[slot & 3] = tctx->waker;
			} else if (WAKER_EWMA) {
				b = tctx->wk_burst[slot & 3] * 3 / 4 + b / 4;
			}
			tctx->wk_burst[slot & 3] = b;
		}
		if (mode == LERTICK_MODE_ONLINE) {
			if (LERTICK_MODEL_V2)
				model_train_v2(&tctx->model, tctx->features,
					       target);
			else
				lertick_model_train(&tctx->model,
						    tctx->features, target);
			__sync_fetch_and_add(&nr_training_updates, 1);
			prog_learn(p, tctx, target, MIN(burst, REFERENCE_NS));
		}
		if (PRED_PLACE && tctx->pp_waker[0] &&
		    (tctx->pp_ran_where == 1 || tctx->pp_ran_where == 2)) {
			struct pp_key k = {};
			struct pp_val *v;
			u8 wh = tctx->pp_ran_where;

			__builtin_memcpy(k.waker, tctx->pp_waker, sizeof(k.waker));
			__builtin_memcpy(k.wakee, p->comm, sizeof(k.wakee));
			v = bpf_map_lookup_elem(&pp_stor, &k);
			if (!v) {
				struct pp_val init = {};

				bpf_map_update_elem(&pp_stor, &k, &init, BPF_NOEXIST);
				v = bpf_map_lookup_elem(&pp_stor, &k);
			}
			if (v) {
				u64 b = MIN(burst, REFERENCE_NS);

				if (wh == 1) {
					v->burst[1] = v->n[1] ? v->burst[1] * 7 / 8 + b / 8 : b;
					v->n[1]++;
				} else {
					v->burst[2] = v->n[2] ? v->burst[2] * 7 / 8 + b / 8 : b;
					v->n[2]++;
				}
			}
		}
		if (PP_COMPLETE && tctx->pp_feat_ok &&
		    (tctx->pp_ran_where == 1 || tctx->pp_ran_where == 2)) {
			__s32 target = pp_q(tctx->pp_wait_last + burst, PP_WAIT_REF);

			if (tctx->pp_ran_where == 1)
				model_train_v2(pp_local_model(tctx->pp_bucket),
					       tctx->pp_feat, target);
			else
				model_train_v2(&pp_model_shared, tctx->pp_feat, target);
		}
		tctx->pp_feat_ok   = false;
		tctx->pp_ran_where = 0;
		if (STEAL_COST && tctx->has_slept) {
			u64 b = MIN(burst, REFERENCE_NS);
			struct prog_key k = {};
			struct prog_val *v;

			if (tctx->sc_mig) {
				tctx->sc_migr = tctx->sc_n_mig ? tctx->sc_migr * 7 / 8 + b / 8 : b;
				tctx->sc_n_mig++;
			} else {
				tctx->sc_stay = tctx->sc_n_stay ? tctx->sc_stay * 7 / 8 + b / 8 : b;
				tctx->sc_n_stay++;
			}
			__builtin_memcpy(k.comm, p->comm, sizeof(k.comm));
			v = bpf_map_lookup_elem(&prog_stor, &k);
			if (v) {
				if (tctx->sc_mig) {
					v->burst_mig = v->n_mig ? v->burst_mig * 15 / 16 + b / 16 : b;
					v->n_mig++;
				} else {
					v->burst_stay = v->n_stay ? v->burst_stay * 15 / 16 + b / 16 : b;
					v->n_stay++;
				}
			}
		}
		tctx->completed_bursts++;
		tctx->last_burst_ns = burst;
		burst		    = MIN(burst, REFERENCE_NS);
		tctx->ewma_burst_ns =
			tctx->ewma_burst_ns ?
				tctx->ewma_burst_ns * 3 / 4 + burst / 4 :
				burst;
	}

	tctx->last_sleep_ns  = bpf_ktime_get_ns();
	tctx->has_slept	     = true;
	tctx->episode_active = false;
	tctx->episode_valid  = false;
}

/*
 * Allocate and initialize the context of a new task.
 */
s32 BPF_STRUCT_OPS(lertick_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct task_ctx *tctx;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;
	lertick_model_init(&tctx->model);
	if (LERTICK_MODEL_V2) {
		tctx->model.weight_q16[0] = 0;
		tctx->model.weight_q16[2] = LERTICK_Q - WAKER_INIT_Q;
		tctx->model.weight_q16[1] = WAKER_INIT_Q;
	}
	if (MEASURE_WAKE2) {
		/* Both run-after-wake models start as their feature 2. */
		tctx->ml.weight_q16[2]	= LERTICK_Q;
		tctx->mlp.weight_q16[2] = LERTICK_Q;
	}

	return 0;
}

/*
 * @p joins the scheduler: start it at the clock.
 */
void BPF_STRUCT_OPS(lertick_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = vtime_now;
}

/*
 * @p leaves the scheduler: drop its unfinished episode.
 */
void BPF_STRUCT_OPS(lertick_disable, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;
	tctx->episode_active = false;
	tctx->episode_valid  = false;
	tctx->has_slept	     = false;
	if (PRED_PLACE && tctx->pp_where)
		pp_remove(tctx);
}

/*
 * Allocate the context of a new cgroup and record its weight.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_cgroup_init, struct cgroup *cgrp,
			     struct scx_cgroup_init_args *args)
{
	struct cgrp_ctx *cgc;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx_stor, cgrp, 0,
				   BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!cgc)
		return -ENOMEM;
	cgc->weight  = args->weight;
	cgc->hweight = HWEIGHT_ONE;

	return 0;
}

/*
 * cpu.weight of @cgrp changed: update its parent's sum and invalidate the
 * cached hierarchical weights.
 */
void BPF_STRUCT_OPS(lertick_cgroup_set_weight, struct cgroup *cgrp, u32 weight)
{
	struct cgrp_ctx *cgc = cgrp_ctx(cgrp), *pc = NULL;

	if (!cgc)
		return;
	if (cgrp->level) {
		pc = cgrp_ancestor_ctx(cgrp, cgrp->level - 1);
		if (!pc)
			return;
	}

	bpf_spin_lock(&cgrp_lock);
	if (pc && cgc->nr_active)
		pc->child_weight_sum += (s64)weight - cgc->weight;
	cgc->weight = weight;
	bpf_spin_unlock(&cgrp_lock);

	__sync_fetch_and_add(&hweight_gen, 1);
}

/* A runnable task changing cgroups takes its weight with it. */
void BPF_STRUCT_OPS(lertick_cgroup_move, struct task_struct *p,
		    struct cgroup *from, struct cgroup *to)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx || !tctx->cg_counted)
		return;
	cgrp_account(from, tctx, false);
	cgrp_account(to, tctx, true);
}

/*
 * Create one vtime-ordered DSQ per LLC.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_init)
{
	u32 d;
	s32 ret;

	bpf_for(d, 0, domain_count())
	{
		ret = scx_bpf_create_dsq(d, -1);
		if (ret)
			return ret;
	}
	if (STEAL_PCPU) {
		u32 c;

		bpf_for(c, 0, scx_bpf_nr_cpu_ids()) {
			ret = scx_bpf_create_dsq(PCPU_DSQ(c), -1);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/*
 * Debug dump, printed by the kernel when the scheduler exits with an error
 * (for example a runnable-task stall): the clock and queue lengths, and for
 * each task its vtime relative to the clock and its burst prediction.
 */
void BPF_STRUCT_OPS(lertick_dump, struct scx_dump_ctx *dctx)
{
	scx_bpf_dump(
		"lertick clock=%llu runnable_weight=%llu queued=%llu domains=%u",
		vtime_now, runnable_weight, nr_queued_all(), domain_count());
}

void BPF_STRUCT_OPS(lertick_dump_task, struct scx_dump_ctx *dctx,
		    struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;
	scx_bpf_dump(
		"lertick vs_clock=%lld eff_weight=%u predicted=%llu ran=%llu ewma=%llu updates=%llu valid=%d",
		(s64)(p->scx.dsq_vtime - vtime_now), tctx->eff_weight,
		tctx->prediction_ns, tctx->episode_runtime_ns,
		tctx->ewma_burst_ns, tctx->model.updates, tctx->episode_valid);
}

/*
 * Record the exit reason for user space.
 */
void BPF_STRUCT_OPS(lertick_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(lertick_ops, .select_cpu = (void *)lertick_select_cpu,
	       .enqueue		  = (void *)lertick_enqueue,
	       .dispatch	  = (void *)lertick_dispatch,
	       .runnable	  = (void *)lertick_runnable,
	       .running		  = (void *)lertick_running,
	       .stopping	  = (void *)lertick_stopping,
	       .quiescent	  = (void *)lertick_quiescent,
	       .cgroup_init	  = (void *)lertick_cgroup_init,
	       .cgroup_set_weight = (void *)lertick_cgroup_set_weight,
	       .cgroup_move	  = (void *)lertick_cgroup_move,
	       .init_task	  = (void *)lertick_init_task,
	       .enable		  = (void *)lertick_enable,
	       .disable = (void *)lertick_disable, .init = (void *)lertick_init,
	       .exit = (void *)lertick_exit, .dump = (void *)lertick_dump,
	       .dump_task  = (void *)lertick_dump_task,
	       .timeout_ms = STARVATION_MS, .name = "lertick");
