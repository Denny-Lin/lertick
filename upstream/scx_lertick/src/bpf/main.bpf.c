/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Hungyu Lin <dennylin0707@gmail.com>
 *
 * scx_lertick: weighted virtual-time scheduling with learned time slices.
 *
 * Tasks wait in one queue per last-level cache, ordered by weighted virtual
 * time; an idle CPU steals from another cache's queue. The virtual
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

const volatile u32 mode		     = LERTICK_MODE_ONLINE;
const volatile u64 slice_fixed	     = 2ULL * NSEC_PER_MSEC;
const volatile u64 slice_min	     = 500ULL * NSEC_PER_USEC;
const volatile u64 slice_max	     = 5ULL * NSEC_PER_MSEC;
const volatile u32 min_train_samples = 8;

/* Statistics, read by user space. */
u64 nr_local_dispatches, nr_shared_dispatches, nr_continuations;
u64 nr_adaptive_slices, nr_training_updates;

UEI_DEFINE(uei);

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
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor		SEC(".maps");

static struct task_ctx *task_ctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

#define LERTICK_MOVE_TO_LOCAL(dsq) scx_bpf_dsq_move_to_local(dsq, 0)
#define LERTICK_TASK_CGROUP(p) scx_bpf_task_cgroup(p)
#include "policy.h"

/*
 * A wakeup starts an episode. Only an episode that starts with a verified
 * wakeup and ends in sleep is a training example: a task that keeps running
 * across slices is not mistaken for one that completed a burst.
 */
static void begin_episode(struct task_ctx *tctx, u64 enq_flags)
{
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
	if (LERTICK_OVERRUN_SLICE_NS && overran(tctx, consumed_ns))
		slice = LERTICK_OVERRUN_SLICE_NS;

	return CLAMP(slice, slice_min, slice_max);
}

s32 BPF_STRUCT_OPS(lertick_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	bool		 idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	/* select_cpu() precedes runnable() on a wakeup and can insert directly. */
	if (tctx && !tctx->episode_active) {
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

void BPF_STRUCT_OPS(lertick_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx  = task_ctx(p);
	u64		 vtime = p->scx.dsq_vtime;
	u64 slice = task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

	/*
	 * Only a wakeup earns sleeper credit: a task requeued after its slice
	 * keeps its vtime, or tasks with larger weights would be pulled back to
	 * the clock and lose their share.
	 */
	if ((enq_flags & SCX_ENQ_WAKEUP) &&
	    time_before(vtime, vtime_now - LAG_NS))
		vtime = vtime_now - LAG_NS;

	scx_bpf_dsq_insert_vtime(p, enqueue_dsq(p, tctx, enq_flags), slice,
				 vtime, enq_flags);
	__sync_fetch_and_add(&nr_shared_dispatches, 1);
}

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
	tctx->running	       = true;
}

void BPF_STRUCT_OPS(lertick_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx	= task_ctx(p);
	u32		 weight = p->scx.weight ? p->scx.weight : 1;
	u64		 delta, rw;

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
	vtime_now += delta * 100 / (rw < 100 ? 100 : rw);
	tctx->episode_runtime_ns += delta;
}

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

		if (mode == LERTICK_MODE_ONLINE) {
			lertick_model_train(&tctx->model, tctx->features,
					    target);
			__sync_fetch_and_add(&nr_training_updates, 1);
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
	tctx->episode_valid  = false;
	tctx->has_slept	     = false;
}

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
	return policy_init();
}

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
	       .exit = (void *)lertick_exit, .timeout_ms = STARVATION_MS,
	       .name = "lertick");
