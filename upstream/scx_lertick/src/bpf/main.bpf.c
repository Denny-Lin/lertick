/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Hungyu Lin <dennylin0707@gmail.com>
 *
 * scx_lertick: weighted virtual-time scheduling with learned time slices.
 *
 * All tasks share one queue ordered by weighted virtual time. The virtual
 * clock advances with the CPU time actually delivered over the total runnable
 * weight, so tasks that keep waking cannot hold back tasks that have already
 * run. A woken task is placed at most LAG_NS behind the clock.
 *
 * In online mode each task trains a small linear model inside BPF on its own
 * completed CPU bursts (wake to sleep). At the next wakeup the prediction sets
 * the task's time slice, bounded by slice_min and slice_max. Fixed mode uses
 * slice_fixed for every task. Fairness always charges the CPU time actually
 * used, never the prediction.
 *
 * The queue and CPU selection follow scx_simple; cgroup weights follow
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
#define STARVATION_MS		5000ULL

#define SHARED_DSQ		0

/* Sleeper credit: a woken task is placed at most this far behind the clock. */
#define LAG_NS			(2ULL * NSEC_PER_MSEC)

/*
 * When taking the next task, prefer one that last ran on this CPU among the
 * first AFFINE_DEPTH queued tasks within AFFINE_WINDOW_NS of the head.
 */
#define AFFINE_WINDOW_NS	(2ULL * NSEC_PER_MSEC)
#define AFFINE_DEPTH		8

/* A sync wakee may queue on the waker's CPU if within this much of the head. */
#define SYNC_WINDOW_NS		(2ULL * NSEC_PER_MSEC)

/* Queued tasks examined, and the longest unbroken run, when prev keeps its CPU. */
#define HEAD_SCAN		16
#define KEEP_MAX_NS		(20ULL * NSEC_PER_MSEC)

/* Model inputs and targets are scaled against this burst length. */
#define REFERENCE_NS		(10ULL * NSEC_PER_MSEC)
#define QUEUE_REFERENCE		16U

#define HWEIGHT_ONE		(1ULL << 16)

const volatile u32 mode = LERTICK_MODE_ONLINE;
const volatile u64 slice_fixed = 2ULL * NSEC_PER_MSEC;
const volatile u64 slice_min = 500ULL * NSEC_PER_USEC;
const volatile u64 slice_max = 5ULL * NSEC_PER_MSEC;
const volatile u32 min_train_samples = 8;

/* Statistics, read by user space. */
u64 nr_local_dispatches, nr_shared_dispatches, nr_continuations;
u64 nr_adaptive_slices, nr_training_updates;

static u64 vtime_now;
/* Sum of runnable tasks' weights: the virtual clock advances by service / weight. */
static u64 runnable_weight;

UEI_DEFINE(uei);

struct task_ctx {
	struct lertick_model model;
	__s32 features[LERTICK_FEATURES];
	u64 runtime_start_ns;
	u64 episode_runtime_ns;
	u64 last_sleep_ns;
	u64 last_burst_ns;
	u64 ewma_burst_ns;
	u64 prediction_ns;
	u64 completed_bursts;
	bool episode_active;
	bool episode_valid;
	bool has_slept;
	bool running;
	bool started_in_select;
	u32 rq_weight;		/* weight counted in runnable_weight, 0 when not counted */
	bool cg_counted;	/* rq_weight is also counted in its cgroup's sums */
	u32 eff_weight;		/* weight of the last charge; dispatch() cannot look up cgroups */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

static struct task_ctx *task_ctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

/*
 * A wakeup starts an episode. Only an episode that starts with a verified
 * wakeup and ends in sleep is a training example: a task that keeps running
 * across slices is not mistaken for one that completed a burst.
 */
static void begin_episode(struct task_ctx *tctx, u64 enq_flags)
{
	u64 now = bpf_ktime_get_ns();
	u64 sleep_ns = tctx->last_sleep_ns ? now - tctx->last_sleep_ns : 0;
	u64 queued = scx_bpf_dsq_nr_queued(SHARED_DSQ);
	__s32 pred;

	tctx->episode_active = true;
	tctx->episode_valid = tctx->has_slept && (enq_flags & SCX_ENQ_WAKEUP);
	tctx->episode_runtime_ns = 0;
	tctx->prediction_ns = 0;

	tctx->features[0] = LERTICK_Q;
	tctx->features[1] = lertick_scale_ns_q16(tctx->last_burst_ns, REFERENCE_NS);
	tctx->features[2] = lertick_scale_ns_q16(tctx->ewma_burst_ns, REFERENCE_NS);
	tctx->features[3] = lertick_scale_ns_q16(sleep_ns, REFERENCE_NS);
	tctx->features[4] = queued >= QUEUE_REFERENCE ? LERTICK_Q :
			    queued * LERTICK_Q / QUEUE_REFERENCE;

	if (mode != LERTICK_MODE_ONLINE || !tctx->episode_valid)
		return;
	pred = lertick_model_predict_q16(&tctx->model, tctx->features);
	tctx->prediction_ns = (u64)pred * REFERENCE_NS / LERTICK_Q;
}

/* Remaining slice: the predicted burst less what already ran, within bounds. */
static u64 task_slice(struct task_ctx *tctx, u64 consumed_ns)
{
	u64 slice = slice_fixed;

	if (tctx && mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	    tctx->model.updates >= min_train_samples &&
	    tctx->prediction_ns > consumed_ns) {
		slice = tctx->prediction_ns - consumed_ns;
		__sync_fetch_and_add(&nr_adaptive_slices, 1);
	}

	return CLAMP(slice, slice_min, slice_max);
}

/*
 * A sync waker is about to sleep, so its CPU is the next free one and its
 * cache holds what the wakee will read. Queue the wakee there, unless that
 * would put it well ahead of tasks already waiting in fair order.
 */
static bool sync_local(struct task_struct *p, s32 *cpu)
{
	struct bpf_iter_scx_dsq it;
	struct task_struct *head;
	s32 this = bpf_get_smp_processor_id();
	u64 vtime = p->scx.dsq_vtime;
	bool ok = true;

	if (!bpf_cpumask_test_cpu(this, p->cpus_ptr))
		return false;
	if (time_before(vtime, vtime_now - LAG_NS))
		vtime = vtime_now - LAG_NS;
	if (bpf_iter_scx_dsq_new(&it, SHARED_DSQ, 0) == 0) {
		head = bpf_iter_scx_dsq_next(&it);
		ok = !head || !time_before(head->scx.dsq_vtime + SYNC_WINDOW_NS, vtime);
	}
	bpf_iter_scx_dsq_destroy(&it);
	if (!ok)
		return false;

	p->scx.dsq_vtime = vtime;
	*cpu = this;

	return true;
}

s32 BPF_STRUCT_OPS(lertick_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	bool idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	/* select_cpu() precedes runnable() on a wakeup and can insert directly. */
	if (tctx && !tctx->episode_active) {
		begin_episode(tctx, SCX_ENQ_WAKEUP);
		tctx->started_in_select = true;
	}
	if (idle || ((wake_flags & SCX_WAKE_SYNC) && sync_local(p, &cpu))) {
		u64 slice = task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		__sync_fetch_and_add(&nr_local_dispatches, 1);
	}

	return cpu;
}

/*
 * cgroup CPU weights (cpu.weight, systemd CPUWeight=). The hierarchy is
 * flattened as in scx_flatcg: a cgroup's hierarchical share (hweight) is the
 * product, at each level, of its weight over the weights of its active
 * siblings. With one global queue the share becomes a per-task effective
 * weight: the task's part of its cgroup's runnable weight times the cgroup's
 * share of all runnable weight. With every cgroup at the default weight, and
 * runnable tasks in one cgroup, this equals the task weight.
 */
struct cgrp_ctx {
	u32 nr_active;		/* this cgroup or a descendant has runnable tasks */
	u32 nr_runnable;	/* runnable tasks directly in this cgroup */
	u32 weight;
	u32 hweight;		/* share of the whole machine, HWEIGHT_ONE = all */
	u64 child_weight_sum;	/* weights of active children */
	u64 hweight_gen;
	u64 task_weight_sum;	/* weights of runnable tasks directly in this cgroup */
};

struct {
	__uint(type, BPF_MAP_TYPE_CGRP_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct cgrp_ctx);
} cgrp_ctx_stor SEC(".maps");

private(LERTICK_CGRP) struct bpf_spin_lock cgrp_lock;
static u64 hweight_gen = 1;

static struct cgrp_ctx *cgrp_ctx(struct cgroup *cgrp)
{
	return bpf_cgrp_storage_get(&cgrp_ctx_stor, cgrp, 0, 0);
}

static struct cgrp_ctx *cgrp_ancestor_ctx(struct cgroup *cgrp, int level)
{
	struct cgrp_ctx *cgc;

	cgrp = bpf_cgroup_ancestor(cgrp, level);
	if (!cgrp)
		return NULL;
	cgc = cgrp_ctx(cgrp);
	bpf_cgroup_release(cgrp);

	return cgc;
}

static void cgrp_refresh_hweight(struct cgroup *cgrp, struct cgrp_ctx *cgc)
{
	int level;

	if (!cgc->nr_active || cgc->hweight_gen == hweight_gen)
		return;

	bpf_for(level, 0, cgrp->level + 1) {
		struct cgrp_ctx *c, *pc;
		bool active;

		c = cgrp_ancestor_ctx(cgrp, level);
		if (!c)
			break;
		if (!level) {
			c->hweight = HWEIGHT_ONE;
			c->hweight_gen = hweight_gen;
			continue;
		}
		pc = cgrp_ancestor_ctx(cgrp, level - 1);
		if (!pc)
			break;

		bpf_spin_lock(&cgrp_lock);
		active = c->nr_active;
		if (active && pc->child_weight_sum) {
			c->hweight_gen = pc->hweight_gen;
			c->hweight = (u64)pc->hweight * c->weight / pc->child_weight_sum;
			if (!c->hweight)
				c->hweight = 1;
		}
		bpf_spin_unlock(&cgrp_lock);
		if (!active)
			break;
	}
}

/* A cgroup gains or loses its first or last runnable task: update active sums upward. */
static void cgrp_update_active(struct cgroup *cgrp, struct cgrp_ctx *cgc, bool runnable)
{
	bool updated = false;
	int idx;

	if (runnable) {
		if (__sync_fetch_and_add(&cgc->nr_runnable, 1))
			return;
	} else {
		if (__sync_sub_and_fetch(&cgc->nr_runnable, 1))
			return;
		cgrp_refresh_hweight(cgrp, cgc);
	}

	bpf_for(idx, 0, cgrp->level) {
		int level = cgrp->level - idx;
		struct cgrp_ctx *c, *pc = NULL;
		bool propagate = false;

		c = cgrp_ancestor_ctx(cgrp, level);
		if (!c)
			break;
		if (level) {
			pc = cgrp_ancestor_ctx(cgrp, level - 1);
			if (!pc)
				break;
		}

		bpf_spin_lock(&cgrp_lock);
		if (runnable) {
			if (!c->nr_active++) {
				updated = true;
				if (pc) {
					propagate = true;
					pc->child_weight_sum += c->weight;
				}
			}
		} else if (c->nr_active) {
			if (!--c->nr_active) {
				updated = true;
				if (pc) {
					propagate = true;
					pc->child_weight_sum -= c->weight;
				}
			}
		}
		bpf_spin_unlock(&cgrp_lock);
		if (!propagate)
			break;
	}

	if (updated)
		__sync_fetch_and_add(&hweight_gen, 1);
	if (runnable)
		cgrp_refresh_hweight(cgrp, cgc);
}

static void cgrp_account(struct cgroup *cgrp, struct task_ctx *tctx, bool runnable)
{
	struct cgrp_ctx *cgc = cgrp_ctx(cgrp);

	if (!cgc)
		return;
	if (runnable)
		__sync_fetch_and_add(&cgc->task_weight_sum, tctx->rq_weight);
	else
		__sync_fetch_and_sub(&cgc->task_weight_sum, tctx->rq_weight);
	cgrp_update_active(cgrp, cgc, runnable);
	tctx->cg_counted = runnable;
}

static void cgrp_task_runnable(struct task_struct *p, struct task_ctx *tctx, bool runnable)
{
	struct cgroup *cgrp;

	if (runnable == tctx->cg_counted)
		return;
	cgrp = scx_bpf_task_cgroup(p);
	if (!cgrp)
		return;
	cgrp_account(cgrp, tctx, runnable);
	bpf_cgroup_release(cgrp);
}

/* Weight to charge service against: the task's share of its cgroup's share. */
static u32 cgrp_effective_weight(struct task_struct *p, struct task_ctx *tctx, u32 weight)
{
	struct cgrp_ctx *cgc;
	struct cgroup *cgrp;
	u64 eff, tws, rw = runnable_weight;

	if (!tctx->cg_counted)
		return weight;
	cgrp = scx_bpf_task_cgroup(p);
	if (!cgrp)
		return weight;
	cgc = cgrp_ctx(cgrp);
	if (!cgc) {
		bpf_cgroup_release(cgrp);
		return weight;
	}
	cgrp_refresh_hweight(cgrp, cgc);
	tws = cgc->task_weight_sum;
	eff = (u64)weight * cgc->hweight / HWEIGHT_ONE;
	bpf_cgroup_release(cgrp);
	if (!tws || !rw)
		return weight;

	return CLAMP(eff * rw / tws, 1, 1000000);
}

void BPF_STRUCT_OPS(lertick_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 vtime = p->scx.dsq_vtime;
	u64 slice = task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

	/*
	 * Only a wakeup earns sleeper credit: a task requeued after its slice
	 * keeps its vtime, or tasks with larger weights would be pulled back to
	 * the clock and lose their share.
	 */
	if ((enq_flags & SCX_ENQ_WAKEUP) && time_before(vtime, vtime_now - LAG_NS))
		vtime = vtime_now - LAG_NS;

	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, slice, vtime, enq_flags);
	__sync_fetch_and_add(&nr_shared_dispatches, 1);
}

/*
 * Whether prev should keep its CPU instead of taking the next queued task.
 * A task whose slice ends otherwise always yields, so two tasks sharing a CPU
 * alternate and get equal time whatever their weights. prev stays while it is
 * ahead in vtime (its current run included) of the first queued task that can
 * run here. It yields after KEEP_MAX_NS without a break, and when no such task
 * is within the first HEAD_SCAN queued: a task that can run only here must not
 * wait behind prev indefinitely.
 */
static bool keep_prev(struct task_struct *prev, struct task_ctx *tctx, s32 cpu)
{
	struct task_struct *q;
	u64 vtime = prev->scx.dsq_vtime, ran = 0;
	u32 weight = tctx && tctx->eff_weight ? tctx->eff_weight :
		     prev->scx.weight ? prev->scx.weight : 1;
	int n = 0;

	if (tctx && tctx->running && prev->se.sum_exec_runtime > tctx->runtime_start_ns)
		ran = prev->se.sum_exec_runtime - tctx->runtime_start_ns;
	if (ran >= KEEP_MAX_NS)
		return false;
	vtime += ran * 100 / weight;

	bpf_for_each(scx_dsq, q, SHARED_DSQ, 0) {
		if (n++ >= HEAD_SCAN)
			return false;
		if (bpf_cpumask_test_cpu(cpu, q->cpus_ptr))
			return !time_before(q->scx.dsq_vtime, vtime);
	}

	return true;
}

/*
 * Take the next task for this CPU. Among the first few queued tasks whose
 * vtime is within a small window of the head, prefer one that last ran here
 * and may still have a warm cache; fair order moves by at most the window.
 */
static bool take_next(s32 cpu)
{
	struct task_struct *q;
	u64 head = 0;
	int n = 0;

	bpf_for_each(scx_dsq, q, SHARED_DSQ, 0) {
		if (!n)
			head = q->scx.dsq_vtime;
		if (n >= AFFINE_DEPTH || time_before(head + AFFINE_WINDOW_NS, q->scx.dsq_vtime))
			break;
		if (scx_bpf_task_cpu(q) == cpu &&
		    scx_bpf_dsq_move(BPF_FOR_EACH_ITER, q, SCX_DSQ_LOCAL, 0))
			return true;
		n++;
	}

	return scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

void BPF_STRUCT_OPS(lertick_dispatch, s32 cpu, struct task_struct *prev)
{
	bool continuing = prev && (prev->scx.flags & SCX_TASK_QUEUED);
	struct task_ctx *tctx;
	u64 consumed;

	if (!((continuing && keep_prev(prev, task_ctx(prev), cpu)) ||
	      (!take_next(cpu) && continuing)))
		return;

	/*
	 * prev continues. The kernel may update sum_exec_runtime before
	 * dispatch() and keep prev without stopping() or running(), so read
	 * the pending segment for the slice decision without charging it:
	 * stopping() charges it.
	 */
	tctx = task_ctx(prev);
	consumed = tctx ? tctx->episode_runtime_ns : 0;
	if (tctx && tctx->running && prev->se.sum_exec_runtime > tctx->runtime_start_ns)
		consumed += prev->se.sum_exec_runtime - tctx->runtime_start_ns;

	scx_bpf_dsq_insert(prev, SCX_DSQ_LOCAL, task_slice(tctx, consumed), 0);
	__sync_fetch_and_add(&nr_continuations, 1);
}

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
		begin_episode(tctx, enq_flags);
	} else if (tctx->started_in_select) {
		/* Confirm the wakeup before admitting this episode as a label. */
		if (!(enq_flags & SCX_ENQ_WAKEUP))
			tctx->episode_valid = false;
		tctx->started_in_select = false;
	}
}

void BPF_STRUCT_OPS(lertick_running, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	/* As in scx_simple, this cross-CPU monotonic update tolerates small races. */
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;

	if (!tctx)
		return;
	tctx->runtime_start_ns = p->se.sum_exec_runtime;
	tctx->running = true;
}

void BPF_STRUCT_OPS(lertick_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx = task_ctx(p);
	u32 weight = p->scx.weight ? p->scx.weight : 1;
	u64 delta, rw;

	if (!tctx || !tctx->running)
		return;
	tctx->running = false;
	if (p->se.sum_exec_runtime < tctx->runtime_start_ns) {
		tctx->episode_valid = false;
		return;
	}
	delta = p->se.sum_exec_runtime - tctx->runtime_start_ns;

	tctx->eff_weight = cgrp_effective_weight(p, tctx, weight);
	p->scx.dsq_vtime += delta * 100 / tctx->eff_weight;

	/*
	 * Advance the clock by the service delivered over the runnable weight,
	 * as the average vtime of runnable tasks moves. Otherwise a stream of
	 * short sleepers, each placed just behind the clock, keeps it still and
	 * starves tasks that have already run.
	 */
	rw = runnable_weight;
	vtime_now += delta * 100 / (rw < 100 ? 100 : rw);
	tctx->episode_runtime_ns += delta;
}

void BPF_STRUCT_OPS(lertick_quiescent, struct task_struct *p, u64 deq_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 burst;

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

		if (mode == LERTICK_MODE_ONLINE) {
			lertick_model_train(&tctx->model, tctx->features, target);
			__sync_fetch_and_add(&nr_training_updates, 1);
		}
		tctx->completed_bursts++;
		tctx->last_burst_ns = burst;
		burst = MIN(burst, REFERENCE_NS);
		tctx->ewma_burst_ns = tctx->ewma_burst_ns ?
				      tctx->ewma_burst_ns * 3 / 4 + burst / 4 : burst;
	}

	tctx->last_sleep_ns = bpf_ktime_get_ns();
	tctx->has_slept = true;
	tctx->episode_active = false;
	tctx->episode_valid = false;
}

s32 BPF_STRUCT_OPS(lertick_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct task_ctx *tctx;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;
	lertick_model_init(&tctx->model);

	return 0;
}

void BPF_STRUCT_OPS(lertick_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = vtime_now;
}

void BPF_STRUCT_OPS(lertick_disable, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;
	tctx->episode_active = false;
	tctx->episode_valid = false;
	tctx->has_slept = false;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_cgroup_init, struct cgroup *cgrp,
			     struct scx_cgroup_init_args *args)
{
	struct cgrp_ctx *cgc;

	cgc = bpf_cgrp_storage_get(&cgrp_ctx_stor, cgrp, 0,
				   BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!cgc)
		return -ENOMEM;
	cgc->weight = args->weight;
	cgc->hweight = HWEIGHT_ONE;

	return 0;
}

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

s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_init)
{
	return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

void BPF_STRUCT_OPS(lertick_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(lertick_ops,
	       .select_cpu		= (void *)lertick_select_cpu,
	       .enqueue			= (void *)lertick_enqueue,
	       .dispatch		= (void *)lertick_dispatch,
	       .runnable		= (void *)lertick_runnable,
	       .running			= (void *)lertick_running,
	       .stopping		= (void *)lertick_stopping,
	       .quiescent		= (void *)lertick_quiescent,
	       .cgroup_init		= (void *)lertick_cgroup_init,
	       .cgroup_set_weight	= (void *)lertick_cgroup_set_weight,
	       .cgroup_move		= (void *)lertick_cgroup_move,
	       .init_task		= (void *)lertick_init_task,
	       .enable			= (void *)lertick_enable,
	       .disable			= (void *)lertick_disable,
	       .init			= (void *)lertick_init,
	       .exit			= (void *)lertick_exit,
	       .timeout_ms		= STARVATION_MS,
	       .name			= "lertick");
