/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ScxSprout Phase 1: global weighted-vtime scheduling with bounded slices.
 *
 * DSQ policy and CPU selection derive from the official scx_simple example;
 * task-storage allocation follows scx_qmap. Reference revision:
 * scx-c-examples at 370b1cea95c6bb668d5b9261f069d433142b9986.
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 * Copyright (c) 2026 ScxSprout contributors.
 *
 * Model parameters never determine dispatch order or fairness charge. Runtime
 * labels use sum_exec_runtime, including every preempted execution segment.
 * Only a verified wake-to-sleep episode is a training example. This deliberately
 * excludes continuously runnable work from training rather than mistaking a
 * slice expiration for a completed CPU burst.
 */
#include <scx/common.bpf.h>
#include "shared/sprout.h"

char _license[] SEC("license") = "GPL";

const volatile u32 mode = SPROUT_MODE_FIXED;
const volatile u64 fixed_slice_ns = SPROUT_DEFAULT_SLICE_NS;
const volatile u64 min_slice_ns = SPROUT_MIN_SLICE_NS;
const volatile u64 max_slice_ns = SPROUT_MAX_SLICE_NS;
const volatile u32 min_train_samples = SPROUT_MIN_TRAIN_SAMPLES;
/* Zero selects comm names beginning with "sprout"; otherwise select a TGID. */
const volatile u32 tracked_tgid;
/* Disposable-VM watchdog test only; userspace gates this behind partial mode. */
const volatile bool fault_stall;
const volatile u32 stall_tgid;

#define SHARED_DSQ 0
#define SPROUT_QUEUE_REFERENCE 16U

static u64 vtime_now;
UEI_DEFINE(uei);

struct task_ctx {
	struct sprout_model model;
	__s32 features[SPROUT_FEATURES];
	u64 runtime_start_ns;
	u64 episode_runtime_ns;
	u64 last_sleep_ns;
	u64 last_burst_ns;
	u64 ewma_burst_ns;
	u64 prediction_ns;
	u64 selected_slice_ns;
	bool episode_active;
	bool episode_valid;
	bool has_slept;
	bool running;
	bool tracked;
	bool slice_was_adaptive;
	bool started_in_select;
	struct sprout_task_snapshot snapshot;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, SPROUT_MAX_TASK_SNAPSHOTS);
	__type(key, u32);
	__type(value, struct sprout_task_snapshot);
} task_snapshots SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, SPROUT_STAT_COUNT);
	__type(key, u32);
	__type(value, u64);
} stats SEC(".maps");

static __always_inline void stat_add(u32 idx, u64 amount)
{
	u64 *value = bpf_map_lookup_elem(&stats, &idx);

	if (value)
		*value += amount;
}

static __always_inline bool is_tracked(struct task_struct *p)
{
	if (tracked_tgid)
		return p->tgid == tracked_tgid;
	return p->comm[0] == 's' && p->comm[1] == 'p' &&
	       p->comm[2] == 'r' && p->comm[3] == 'o' &&
	       p->comm[4] == 'u' && p->comm[5] == 't';
}

static __always_inline bool is_fault_target(struct task_struct *p)
{
	return fault_stall && stall_tgid && p->tgid == stall_tgid;
}

static __always_inline struct task_ctx *task_ctx(struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
}

static __always_inline void refresh_tracking(struct task_struct *p,
					    struct task_ctx *tctx)
{
	/* A workload can set its comm after fork, so refresh in live callbacks. */
	if (!tctx->tracked && is_tracked(p)) {
		tctx->tracked = true;
		tctx->snapshot.pid = p->pid;
		tctx->snapshot.tgid = p->tgid;
		/* Features were not captured if opt-in occurred during execution. */
		if (tctx->episode_active)
			tctx->episode_valid = false;
	}
	if (tctx->tracked)
		__builtin_memcpy(tctx->snapshot.comm, p->comm, SPROUT_COMM_LEN);
}

static __always_inline void publish(struct task_ctx *tctx)
{
	u32 pid;

	if (!tctx->tracked)
		return;
	tctx->snapshot.episode_runtime_ns = tctx->episode_runtime_ns;
	tctx->snapshot.episode_active = tctx->episode_active;
	tctx->snapshot.episode_valid = tctx->episode_valid;
	tctx->snapshot.prediction_ns = tctx->prediction_ns;
	tctx->snapshot.training_updates = tctx->model.updates;
#pragma unroll
	for (int j = 0; j < SPROUT_FEATURES; j++) {
		tctx->snapshot.model_weight_q16[j] = tctx->model.weight_q16[j];
		tctx->snapshot.features_q16[j] = tctx->features[j];
	}
	pid = tctx->snapshot.pid;
	if (bpf_map_update_elem(&task_snapshots, &pid, &tctx->snapshot, BPF_ANY))
		stat_add(SPROUT_STAT_SNAPSHOT_FAILURES, 1);
}

static __always_inline void censor_episode(struct task_ctx *tctx, u32 reason)
{
	if (!tctx->tracked || !tctx->episode_active)
		return;
	tctx->snapshot.censored_bursts++;
	tctx->snapshot.last_censor_reason = reason;
	stat_add(SPROUT_STAT_CENSORED_BURSTS, 1);
}

static __always_inline void begin_episode(struct task_struct *p,
					 struct task_ctx *tctx, u64 enq_flags)
{
	u64 now = bpf_ktime_get_ns();
	u64 sleep_ns = tctx->last_sleep_ns ? now - tctx->last_sleep_ns : 0;
	u64 queued = scx_bpf_dsq_nr_queued(SHARED_DSQ);

	tctx->episode_active = true;
	tctx->episode_valid = tctx->has_slept && (enq_flags & SCX_ENQ_WAKEUP);
	tctx->episode_runtime_ns = 0;
	tctx->prediction_ns = 0;
	if (!tctx->tracked)
		return;
	tctx->snapshot.last_censor_reason = tctx->episode_valid ? SPROUT_CENSOR_NONE :
		SPROUT_CENSOR_ATTACH_OR_UNVERIFIED_WAKE;
	tctx->features[0] = SPROUT_Q;
	tctx->features[1] = sprout_scale_ns_q16(tctx->last_burst_ns,
					      SPROUT_REFERENCE_NS);
	tctx->features[2] = sprout_scale_ns_q16(tctx->ewma_burst_ns,
					      SPROUT_REFERENCE_NS);
	tctx->features[3] = sprout_scale_ns_q16(sleep_ns, SPROUT_REFERENCE_NS);
	tctx->features[4] = queued >= SPROUT_QUEUE_REFERENCE ? SPROUT_Q :
		queued * SPROUT_Q / SPROUT_QUEUE_REFERENCE;
	tctx->snapshot.last_sleep_interval_ns = sleep_ns;
	tctx->snapshot.last_queue_depth = queued;
	if (mode != SPROUT_MODE_FIXED && tctx->episode_valid) {
		__s32 pred = sprout_model_predict_q16(&tctx->model, tctx->features);

		tctx->prediction_ns = (__u64)pred * SPROUT_REFERENCE_NS / SPROUT_Q;
		tctx->snapshot.last_prediction_q16 = pred;
		tctx->snapshot.inferences++;
		stat_add(SPROUT_STAT_INFERENCES, 1);
		stat_add(SPROUT_STAT_PREDICTION_NS_SUM, tctx->prediction_ns);
	}
}

static __always_inline u64 choose_slice(struct task_ctx *tctx)
{
	u64 slice = fixed_slice_ns;
	bool adaptive = false;

	if (tctx && mode == SPROUT_MODE_ONLINE && tctx->tracked &&
	    tctx->episode_valid && tctx->model.updates >= min_train_samples &&
	    tctx->prediction_ns > tctx->episode_runtime_ns) {
		slice = tctx->prediction_ns - tctx->episode_runtime_ns;
		adaptive = true;
	}
	if (slice < min_slice_ns)
		slice = min_slice_ns;
	if (slice > max_slice_ns)
		slice = max_slice_ns;
	if (!tctx)
		return slice;
	tctx->slice_was_adaptive = adaptive;
	tctx->selected_slice_ns = slice;
	if (!tctx->tracked)
		return slice;
	if (tctx->snapshot.selected_slice_ns &&
	    tctx->snapshot.selected_slice_ns != slice) {
		tctx->snapshot.slice_changes++;
		stat_add(SPROUT_STAT_SLICE_CHANGES, 1);
	}
	tctx->snapshot.selected_slice_ns = slice;
	if (!tctx->snapshot.min_selected_slice_ns ||
	    slice < tctx->snapshot.min_selected_slice_ns)
		tctx->snapshot.min_selected_slice_ns = slice;
	if (slice > tctx->snapshot.max_selected_slice_ns)
		tctx->snapshot.max_selected_slice_ns = slice;
	tctx->snapshot.enqueue_count++;
	tctx->snapshot.slice_sum_ns += slice;
	stat_add(SPROUT_STAT_SELECTED_SLICE_NS_SUM, slice);
	if (adaptive) {
		tctx->snapshot.adaptive_slices++;
		stat_add(SPROUT_STAT_ADAPTIVE_SLICES, 1);
		if (!tctx->snapshot.min_adaptive_slice_ns ||
		    slice < tctx->snapshot.min_adaptive_slice_ns)
			tctx->snapshot.min_adaptive_slice_ns = slice;
		if (slice > tctx->snapshot.max_adaptive_slice_ns)
			tctx->snapshot.max_adaptive_slice_ns = slice;
	}
	return slice;
}

s32 BPF_STRUCT_OPS(sprout_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	bool idle = false;
	s32 cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	if (tctx) {
		refresh_tracking(p, tctx);
		/* select_cpu precedes runnable on a wakeup and can insert directly. */
		if (!tctx->episode_active) {
			begin_episode(p, tctx, SCX_ENQ_WAKEUP);
			tctx->started_in_select = true;
		}
	}
	if (idle && !is_fault_target(p)) {
		u64 slice = choose_slice(tctx);

		stat_add(SPROUT_STAT_LOCAL_ENQUEUE, 1);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		if (tctx)
			publish(tctx);
	}
	return cpu;
}

void BPF_STRUCT_OPS(sprout_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 vtime = p->scx.dsq_vtime;
	u64 slice;

	if (is_fault_target(p)) {
		stat_add(SPROUT_STAT_FAULT_STALLS, 1);
		return;
	}
	if (tctx)
		refresh_tracking(p, tctx);
	slice = choose_slice(tctx);
	/* Sleeper credit is bounded independently of model output. */
	if (time_before(vtime, vtime_now - max_slice_ns))
		vtime = vtime_now - max_slice_ns;
	stat_add(SPROUT_STAT_GLOBAL_ENQUEUE, 1);
	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, slice, vtime, enq_flags);
	if (tctx)
		publish(tctx);
}

void BPF_STRUCT_OPS(sprout_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Explicitly reinsert a continuing task to avoid the kernel's DFL refill. */
	if (!scx_bpf_dsq_move_to_local(SHARED_DSQ) && prev &&
	    (prev->scx.flags & SCX_TASK_QUEUED) && !is_fault_target(prev)) {
		struct task_ctx *tctx = task_ctx(prev);
		u64 slice = choose_slice(tctx);

		stat_add(SPROUT_STAT_LOCAL_ENQUEUE, 1);
		scx_bpf_dsq_insert(prev, SCX_DSQ_LOCAL, slice, 0);
		if (tctx)
			publish(tctx);
	}
}

void BPF_STRUCT_OPS(sprout_runnable, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;
	refresh_tracking(p, tctx);
	/* Migration can produce runnable notifications without a new wakeup. */
	if (!tctx->episode_active)
		begin_episode(p, tctx, enq_flags);
	else if (tctx->started_in_select) {
		/* Confirm the wakeup before admitting this episode as a label. */
		if (!(enq_flags & SCX_ENQ_WAKEUP))
			tctx->episode_valid = false;
		tctx->started_in_select = false;
	}
	publish(tctx);
}

void BPF_STRUCT_OPS(sprout_running, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 observed_slice = p->scx.slice;

	/* As in scx_simple, this cross-CPU monotonic update tolerates small races. */
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
	stat_add(SPROUT_STAT_RUNS, 1);
	if (!tctx)
		return;
	refresh_tracking(p, tctx);
	tctx->runtime_start_ns = p->se.sum_exec_runtime;
	tctx->running = true;
	if (tctx->tracked) {
		tctx->snapshot.runs++;
		tctx->snapshot.observed_running_slice_ns = observed_slice;
		if (!tctx->snapshot.min_running_slice_ns ||
		    observed_slice < tctx->snapshot.min_running_slice_ns)
			tctx->snapshot.min_running_slice_ns = observed_slice;
		if (observed_slice > tctx->snapshot.max_running_slice_ns)
			tctx->snapshot.max_running_slice_ns = observed_slice;
		if (observed_slice == tctx->selected_slice_ns) {
			tctx->snapshot.matching_dispatch_slices++;
			stat_add(SPROUT_STAT_MATCHING_DISPATCH_SLICES, 1);
		}
		if (tctx->slice_was_adaptive)
			tctx->snapshot.adaptive_runs++;
	}
	publish(tctx);
}

void BPF_STRUCT_OPS(sprout_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 runtime, delta;
	u32 weight = p->scx.weight;

	if (!tctx || !tctx->running)
		return;
	tctx->running = false;
	runtime = p->se.sum_exec_runtime;
	if (runtime < tctx->runtime_start_ns) {
		tctx->episode_valid = false;
		tctx->snapshot.last_censor_reason = SPROUT_CENSOR_ACCOUNTING;
		stat_add(SPROUT_STAT_COUNTER_ERRORS, 1);
		return;
	}
	delta = runtime - tctx->runtime_start_ns;
	/* No bounded slice is treated as executed service for accounting. */
	if (!weight)
		weight = 1;
	p->scx.dsq_vtime += delta * 100 / weight;
	tctx->episode_runtime_ns += delta;
	stat_add(SPROUT_STAT_RUNTIME_NS, delta);
	if (tctx->tracked) {
		tctx->snapshot.total_runtime_ns += delta;
		tctx->snapshot.last_runtime_delta_ns = delta;
	}
	publish(tctx);
}

void BPF_STRUCT_OPS(sprout_quiescent, struct task_struct *p, u64 deq_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 burst;

	if (!tctx || !(deq_flags & SCX_DEQ_SLEEP))
		return;
	/*
	 * Audited Linux v7.0 kernel/sched/ext.c::dequeue_task_scx(): a running
	 * task receives update_curr_scx() and stopping(p, false) BEFORE this
	 * callback. put_prev_task_scx() then suppresses stopping for !QUEUED.
	 * The final service segment is therefore already included exactly once.
	 */
	/* Non-sleep quiescence preserves the episode across migration/preemption. */
	burst = tctx->episode_runtime_ns;
	if (tctx->episode_active && tctx->tracked) {
		if (p->flags & PF_EXITING) {
			censor_episode(tctx, SPROUT_CENSOR_EXIT);
		} else if (!tctx->episode_valid || !burst) {
			censor_episode(tctx, tctx->snapshot.last_censor_reason ==
				       SPROUT_CENSOR_ACCOUNTING ? SPROUT_CENSOR_ACCOUNTING :
				       SPROUT_CENSOR_ATTACH_OR_UNVERIFIED_WAKE);
		} else {
			__s32 target = sprout_scale_ns_q16(burst, SPROUT_REFERENCE_NS);

			tctx->snapshot.completed_bursts++;
			tctx->snapshot.last_burst_ns = burst;
			tctx->snapshot.last_target_q16 = target;
			tctx->snapshot.last_censor_reason = SPROUT_CENSOR_NONE;
			if (burst > tctx->snapshot.max_burst_ns)
				tctx->snapshot.max_burst_ns = burst;
			stat_add(SPROUT_STAT_COMPLETED_BURSTS, 1);
			if (mode != SPROUT_MODE_FIXED) {
				tctx->snapshot.last_training_error_q16 =
					sprout_model_train(&tctx->model, tctx->features, target);
				stat_add(SPROUT_STAT_TRAINING_UPDATES, 1);
			}
			tctx->last_burst_ns = burst;
			/* Cap history before arithmetic; target itself is capped at 10ms. */
			if (burst > SPROUT_REFERENCE_NS)
				burst = SPROUT_REFERENCE_NS;
			tctx->ewma_burst_ns = tctx->ewma_burst_ns ?
				tctx->ewma_burst_ns * 3 / 4 + burst / 4 : burst;
		}
	}
	tctx->last_sleep_ns = bpf_ktime_get_ns();
	tctx->has_slept = true;
	tctx->episode_active = false;
	tctx->episode_valid = false;
	publish(tctx);
}

s32 BPF_STRUCT_OPS(sprout_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
						BPF_LOCAL_STORAGE_GET_F_CREATE);

	if (!tctx) {
		stat_add(SPROUT_STAT_ALLOC_FAILURES, 1);
		/* Runtime ownership is essential: reject rather than mischarge vtime. */
		return -ENOMEM;
	}
	sprout_model_init(&tctx->model);
	refresh_tracking(p, tctx);
	return 0;
}

void BPF_STRUCT_OPS(sprout_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = vtime_now;
}

void BPF_STRUCT_OPS(sprout_disable, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;
	censor_episode(tctx, p->flags & PF_EXITING ? SPROUT_CENSOR_EXIT :
			SPROUT_CENSOR_POLICY_TRANSITION);
	tctx->episode_active = false;
	tctx->episode_valid = false;
	tctx->has_slept = false;
	publish(tctx);
}

void BPF_STRUCT_OPS(sprout_exit_task, struct task_struct *p,
		    struct scx_exit_task_args *args)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;
	censor_episode(tctx, SPROUT_CENSOR_EXIT);
	tctx->episode_active = false;
	tctx->episode_valid = false;
	tctx->snapshot.exited = 1;
	publish(tctx);
}

s32 BPF_STRUCT_OPS_SLEEPABLE(sprout_init)
{
	return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

void BPF_STRUCT_OPS(sprout_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(sprout_ops,
	.select_cpu = (void *)sprout_select_cpu,
	.enqueue = (void *)sprout_enqueue,
	.dispatch = (void *)sprout_dispatch,
	.runnable = (void *)sprout_runnable,
	.running = (void *)sprout_running,
	.stopping = (void *)sprout_stopping,
	.quiescent = (void *)sprout_quiescent,
	.init_task = (void *)sprout_init_task,
	.exit_task = (void *)sprout_exit_task,
	.enable = (void *)sprout_enable,
	.disable = (void *)sprout_disable,
	.init = (void *)sprout_init,
	.exit = (void *)sprout_exit,
	.timeout_ms = 1000U,
	.name = "scx_sprout");
