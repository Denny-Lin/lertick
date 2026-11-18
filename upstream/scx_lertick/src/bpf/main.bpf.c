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
#ifndef PROG_INHERIT_UPDATES
#define PROG_INHERIT_UPDATES (~0ULL)
#endif

struct prog_key {
	char comm[16];
};

struct prog_val {
	struct lertick_model model;
	u64		     ewma_burst_ns;
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
#define LERTICK_MOVE_TO_LOCAL(dsq) scx_bpf_dsq_move_to_local(dsq, 0)
#define LERTICK_TASK_CGROUP(p) scx_bpf_task_cgroup(p)
#include "policy.h"

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
#define SHORT_KICK_US 500
#ifndef KICK_RAN_US
#define KICK_RAN_US 1000
#endif
u64 nr_short_kicks;

/* Remaining slice: the predicted burst less what already ran, within bounds. */
static u64 task_slice(struct task_ctx *tctx, u64 consumed_ns)
{
	u64 slice = slice_fixed;

	if (tctx && mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	    tctx->model.updates >= min_train_samples &&
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
	if (!v)
		return;
	tctx->model	    = v->model;
	tctx->ewma_burst_ns = v->ewma_burst_ns;
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

s32 BPF_STRUCT_OPS(lertick_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	bool		 idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	/* select_cpu() precedes runnable() on a wakeup and can insert directly. */
	if (tctx && !tctx->episode_active) {
		prog_sync(p, tctx);
		begin_episode(tctx, SCX_ENQ_WAKEUP);
		tctx->started_in_select = true;
	}
	if (idle || ((wake_flags & SCX_WAKE_SYNC) && sync_local(p, &cpu))) {
		u64 slice =
			task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		__sync_fetch_and_add(&nr_local_dispatches, 1);
	}

	return cpu;
}

/*
 * Queue @p on its LLC's DSQ in vtime order, with its vtime held within the
 * lag bounds.
 */
void BPF_STRUCT_OPS(lertick_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx  = task_ctx(p);
	u64		 vtime = p->scx.dsq_vtime;
	u64 slice = task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

	/*
	 * Only a wakeup earns sleeper credit (LAG_NS); a task requeued after its
	 * slice keeps its vtime within the wider bounds of clamp_ahead(), or
	 * tasks with larger weights would be pulled back to the clock and lose
	 * their share.
	 */
	if ((enq_flags & SCX_ENQ_WAKEUP) &&
	    time_before(vtime, vtime_now - LAG_NS))
		vtime = vtime_now - LAG_NS;
	vtime = clamp_ahead(p, vtime);

	scx_bpf_dsq_insert_vtime(p, domain_of(scx_bpf_task_cpu(p)), slice,
				 vtime, enq_flags);
	__sync_fetch_and_add(&nr_shared_dispatches, 1);

	if (SHORT_KICK && (enq_flags & SCX_ENQ_WAKEUP) && tctx &&
	    mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	    tctx->model.updates >= min_train_samples &&
	    tctx->prediction_ns < SHORT_KICK_US * NSEC_PER_USEC) {
		s32		    cpu = scx_bpf_task_cpu(p);
		struct task_struct *cur = __COMPAT_scx_bpf_cpu_curr(cpu);
		struct task_ctx	   *cc;

		if (!cur || cur == p)
			return;
		cc = task_ctx(cur);
		if (cc && cc->running &&
		    cc->model.updates >= min_train_samples &&
		    cc->prediction_ns > slice_max &&
		    cur->se.sum_exec_runtime >=
			    cc->runtime_start_ns + KICK_RAN_US * NSEC_PER_USEC) {
			scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
			__sync_fetch_and_add(&nr_short_kicks, 1);
		}
	}
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
	      (!take_next(cpu) && continuing)))
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

	clock_raise(p->scx.dsq_vtime);

	if (!tctx)
		return;
	tctx->runtime_start_ns = p->se.sum_exec_runtime;
	tctx->running	       = true;
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
			bool tr	  = tctx->model.updates >= min_train_samples;
			u64  used = tr ? tctx->prediction_ns : slice_fixed;

			__sync_fetch_and_add(&m_all, 1);
			__sync_fetch_and_add(&m_err_used,
					     used > b ? used - b : b - used);
			if ((used <= slice_fixed) == (b <= slice_fixed))
				__sync_fetch_and_add(&m_cls_used, 1);
		}
		if (LERTICK_MEASURE && tctx->model.updates >= min_train_samples) {
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
