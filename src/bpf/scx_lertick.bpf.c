/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Lertick: global weighted-vtime scheduling with bounded slices.
 *
 * DSQ policy and CPU selection derive from the official scx_simple example;
 * task-storage allocation follows scx_qmap. Reference revision:
 * scx-c-examples at 370b1cea95c6bb668d5b9261f069d433142b9986.
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 * Copyright (c) 2026 Lertick contributors.
 *
 * Model parameters never determine dispatch order or fairness charge. Runtime
 * labels use sum_exec_runtime, including every preempted execution segment.
 * Only a verified wake-to-sleep episode is a training example. This deliberately
 * excludes continuously runnable work from training rather than mistaking a
 * slice expiration for a completed CPU burst.
 */
#include <scx/common.bpf.h>
#include "shared/lertick.h"
#include "shared/accounting.h"

char _license[] SEC("license") = "GPL";

const volatile u32 mode = LERTICK_MODE_FIXED;
const volatile u64 fixed_slice_ns = LERTICK_DEFAULT_SLICE_NS;
const volatile u64 min_slice_ns = LERTICK_MIN_SLICE_NS;
const volatile u64 max_slice_ns = LERTICK_MAX_SLICE_NS;
const volatile u32 min_train_samples = LERTICK_MIN_TRAIN_SAMPLES;
/* Zero selects comm names beginning with "lertick"; otherwise select a TGID. */
const volatile u32 tracked_tgid;
const volatile bool track_all;
/* Disposable-VM watchdog test only; userspace gates this behind partial mode. */
const volatile bool fault_stall;
const volatile u32 stall_tgid;
/* Optional diagnostic clocks; primary benchmarks leave this disabled. */
const volatile bool profile_model;

#define SHARED_DSQ 0
#define LERTICK_QUEUE_REFERENCE 16U

static u64 vtime_now;
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
	u64 ewma_prediction_ns;
	u64 wake_ns;
	u64 selected_slice_ns;
	bool episode_active;
	bool episode_valid;
	bool has_slept;
	bool running;
	bool tracked;
	bool slice_was_adaptive;
	bool started_in_select;
	struct lertick_task_snapshot snapshot;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, LERTICK_MAX_TASK_SNAPSHOTS);
	__type(key, u32);
	__type(value, struct lertick_task_snapshot);
} task_snapshots SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, LERTICK_STAT_COUNT);
	__type(key, u32);
	__type(value, u64);
} stats SEC(".maps");

static __always_inline void stat_add(u32 idx, u64 amount)
{
	u64 *value = bpf_map_lookup_elem(&stats, &idx);

	if (value)
		*value += amount;
}

static __always_inline void stat_max(u32 idx, u64 amount)
{
	u64 *value = bpf_map_lookup_elem(&stats, &idx);

	if (value && amount > *value)
		*value = amount;
}

static __always_inline bool is_tracked(struct task_struct *p)
{
	if (track_all)
		return true;
	if (tracked_tgid)
		return p->tgid == tracked_tgid;
	return p->comm[0] == 'l' && p->comm[1] == 'e' &&
	       p->comm[2] == 'r' && p->comm[3] == 't' &&
	       p->comm[4] == 'i' && p->comm[5] == 'c' &&
	       p->comm[6] == 'k';
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
		__builtin_memcpy(tctx->snapshot.comm, p->comm, LERTICK_COMM_LEN);
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
	for (int j = 0; j < LERTICK_FEATURES; j++) {
		tctx->snapshot.model_weight_q16[j] = tctx->model.weight_q16[j];
		tctx->snapshot.features_q16[j] = tctx->features[j];
	}
	pid = tctx->snapshot.pid;
	if (bpf_map_update_elem(&task_snapshots, &pid, &tctx->snapshot, BPF_ANY))
		stat_add(LERTICK_STAT_SNAPSHOT_FAILURES, 1);
}

static __always_inline void censor_episode(struct task_ctx *tctx, u32 reason)
{
	if (!tctx->tracked || !tctx->episode_active)
		return;
	tctx->snapshot.censored_bursts++;
	tctx->snapshot.last_censor_reason = reason;
	stat_add(LERTICK_STAT_CENSORED_BURSTS, 1);
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
	tctx->ewma_prediction_ns = tctx->ewma_burst_ns;
	tctx->wake_ns = tctx->episode_valid ? now : 0;
	if (!tctx->tracked)
		return;
	tctx->snapshot.last_censor_reason = tctx->episode_valid ? LERTICK_CENSOR_NONE :
		LERTICK_CENSOR_ATTACH_OR_UNVERIFIED_WAKE;
	tctx->features[0] = LERTICK_Q;
	tctx->features[1] = lertick_scale_ns_q16(tctx->last_burst_ns,
					      LERTICK_REFERENCE_NS);
	tctx->features[2] = lertick_scale_ns_q16(tctx->ewma_burst_ns,
					      LERTICK_REFERENCE_NS);
	tctx->features[3] = lertick_scale_ns_q16(sleep_ns, LERTICK_REFERENCE_NS);
	tctx->features[4] = queued >= LERTICK_QUEUE_REFERENCE ? LERTICK_Q :
		queued * LERTICK_Q / LERTICK_QUEUE_REFERENCE;
	tctx->snapshot.last_sleep_interval_ns = sleep_ns;
	tctx->snapshot.last_queue_depth = queued;
	if (mode != LERTICK_MODE_FIXED && tctx->episode_valid) {
		u64 started = profile_model ? bpf_ktime_get_ns() : 0;
		__s32 pred = mode == LERTICK_MODE_EWMA ? tctx->features[2] :
			lertick_model_predict_q16(&tctx->model, tctx->features);

		if (profile_model)
			stat_add(LERTICK_STAT_INFERENCE_TIME_NS, bpf_ktime_get_ns() - started);

		tctx->prediction_ns = (__u64)pred * LERTICK_REFERENCE_NS / LERTICK_Q;
		tctx->snapshot.last_prediction_q16 = pred;
		tctx->snapshot.inferences++;
		stat_add(LERTICK_STAT_INFERENCES, 1);
		stat_add(LERTICK_STAT_PREDICTION_NS_SUM, tctx->prediction_ns);
	}
}

static __always_inline u64 choose_slice(struct task_ctx *tctx, u64 consumed_ns)
{
	u64 slice = fixed_slice_ns;
	bool adaptive = false;

	if (tctx && (mode == LERTICK_MODE_ONLINE || mode == LERTICK_MODE_EWMA) &&
	    tctx->tracked && tctx->episode_valid &&
	    (mode == LERTICK_MODE_EWMA ? tctx->snapshot.completed_bursts :
	     tctx->model.updates) >= min_train_samples &&
	    tctx->prediction_ns > consumed_ns) {
		slice = tctx->prediction_ns - consumed_ns;
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
		stat_add(LERTICK_STAT_SLICE_CHANGES, 1);
	}
	tctx->snapshot.selected_slice_ns = slice;
	if (!tctx->snapshot.min_selected_slice_ns ||
	    slice < tctx->snapshot.min_selected_slice_ns)
		tctx->snapshot.min_selected_slice_ns = slice;
	if (slice > tctx->snapshot.max_selected_slice_ns)
		tctx->snapshot.max_selected_slice_ns = slice;
	tctx->snapshot.enqueue_count++;
	tctx->snapshot.slice_sum_ns += slice;
	stat_add(LERTICK_STAT_SELECTED_SLICE_NS_SUM, slice);
	if (adaptive) {
		tctx->snapshot.adaptive_slices++;
		stat_add(LERTICK_STAT_ADAPTIVE_SLICES, 1);
		if (!tctx->snapshot.min_adaptive_slice_ns ||
		    slice < tctx->snapshot.min_adaptive_slice_ns)
			tctx->snapshot.min_adaptive_slice_ns = slice;
		if (slice > tctx->snapshot.max_adaptive_slice_ns)
			tctx->snapshot.max_adaptive_slice_ns = slice;
	}
	return slice;
}

s32 BPF_STRUCT_OPS(lertick_select_cpu, struct task_struct *p, s32 prev_cpu,
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
		u64 slice = choose_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

		stat_add(LERTICK_STAT_LOCAL_ENQUEUE, 1);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		if (tctx)
			publish(tctx);
	}
	return cpu;
}

void BPF_STRUCT_OPS(lertick_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 vtime = p->scx.dsq_vtime;
	u64 slice;

	if (is_fault_target(p)) {
		stat_add(LERTICK_STAT_FAULT_STALLS, 1);
		return;
	}
	if (tctx)
		refresh_tracking(p, tctx);
	slice = choose_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);
	/* Sleeper credit is bounded independently of model output. */
	if (time_before(vtime, vtime_now - max_slice_ns))
		vtime = vtime_now - max_slice_ns;
	stat_add(LERTICK_STAT_GLOBAL_ENQUEUE, 1);
	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, slice, vtime, enq_flags);
	if (tctx)
		publish(tctx);
}

void BPF_STRUCT_OPS(lertick_dispatch, s32 cpu, struct task_struct *prev)
{
	/* Explicitly reinsert a continuing task to avoid the kernel's DFL refill. */
	if (!scx_bpf_dsq_move_to_local(SHARED_DSQ) && prev &&
	    (prev->scx.flags & SCX_TASK_QUEUED) && !is_fault_target(prev)) {
		struct task_ctx *tctx = task_ctx(prev);
		struct lertick_service_sample service = {0};
		u64 slice;

		/*
		 * Linux v7.0 balance_one() updates sum_exec_runtime before
		 * dispatch(), but may keep prev without stopping()/running().
		 * Read the pending segment for the remaining-prediction decision;
		 * committing it here would double-charge it in stopping().
		 */
		if (tctx) {
			refresh_tracking(prev, tctx);
			service = lertick_service_sample(tctx->episode_runtime_ns,
				tctx->runtime_start_ns, prev->se.sum_exec_runtime,
				tctx->running);
			if (!service.valid) {
				tctx->episode_valid = false;
				tctx->snapshot.last_censor_reason = LERTICK_CENSOR_ACCOUNTING;
				stat_add(LERTICK_STAT_COUNTER_ERRORS, 1);
			}
		}
		slice = choose_slice(tctx, service.consumed_ns);
		if (tctx && tctx->tracked) {
			tctx->snapshot.continuation_slices++;
			tctx->snapshot.continuing_pending_ns = service.pending_ns;
			tctx->snapshot.continuing_consumed_ns = service.consumed_ns;
			tctx->snapshot.continuing_prediction_ns = tctx->prediction_ns;
			tctx->snapshot.continuing_selected_slice_ns = slice;
			tctx->snapshot.continuing_model_updates = tctx->model.updates;
			tctx->snapshot.continuing_episode_valid = tctx->episode_valid;
			stat_add(LERTICK_STAT_CONTINUATION_SLICES, 1);
			stat_add(LERTICK_STAT_CONTINUATION_PENDING_NS, service.pending_ns);
		}

		stat_add(LERTICK_STAT_LOCAL_ENQUEUE, 1);
		scx_bpf_dsq_insert(prev, SCX_DSQ_LOCAL, slice, 0);
		if (tctx)
			publish(tctx);
	}
}

void BPF_STRUCT_OPS(lertick_runnable, struct task_struct *p, u64 enq_flags)
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

void BPF_STRUCT_OPS(lertick_running, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 observed_slice = p->scx.slice;

	/* As in scx_simple, this cross-CPU monotonic update tolerates small races. */
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
	stat_add(LERTICK_STAT_RUNS, 1);
	if (!tctx)
		return;
	refresh_tracking(p, tctx);
	if (tctx->wake_ns && tctx->episode_valid && tctx->tracked) {
		u64 latency = bpf_ktime_get_ns() - tctx->wake_ns;

		tctx->snapshot.wake_samples++;
		tctx->snapshot.wake_to_run_ns += latency;
		if (latency > tctx->snapshot.wake_to_run_max_ns)
			tctx->snapshot.wake_to_run_max_ns = latency;
		stat_add(LERTICK_STAT_WAKE_SAMPLES, 1);
		stat_add(LERTICK_STAT_WAKE_TO_RUN_NS, latency);
		stat_max(LERTICK_STAT_WAKE_TO_RUN_MAX_NS, latency);
	}
	tctx->wake_ns = 0;
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
			stat_add(LERTICK_STAT_MATCHING_DISPATCH_SLICES, 1);
		}
		if (tctx->slice_was_adaptive)
			tctx->snapshot.adaptive_runs++;
	}
	publish(tctx);
}

void BPF_STRUCT_OPS(lertick_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx = task_ctx(p);
	struct lertick_service_sample service;
	u64 delta;
	u32 weight = p->scx.weight;

	if (!tctx || !tctx->running)
		return;
	/* comm can change while a task continues without a running callback. */
	refresh_tracking(p, tctx);
	service = lertick_service_sample(tctx->episode_runtime_ns,
			tctx->runtime_start_ns, p->se.sum_exec_runtime, true);
	tctx->running = false;
	if (!service.valid) {
		tctx->episode_valid = false;
		tctx->snapshot.last_censor_reason = LERTICK_CENSOR_ACCOUNTING;
		stat_add(LERTICK_STAT_COUNTER_ERRORS, 1);
		return;
	}
	delta = service.pending_ns;
	/* No bounded slice is treated as executed service for accounting. */
	if (!weight)
		weight = 1;
	p->scx.dsq_vtime += delta * 100 / weight;
	tctx->episode_runtime_ns = service.consumed_ns;
	stat_add(LERTICK_STAT_RUNTIME_NS, delta);
	if (tctx->tracked) {
		tctx->snapshot.total_runtime_ns += delta;
		tctx->snapshot.last_runtime_delta_ns = delta;
	}
	publish(tctx);
}

void BPF_STRUCT_OPS(lertick_quiescent, struct task_struct *p, u64 deq_flags)
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
			censor_episode(tctx, LERTICK_CENSOR_EXIT);
		} else if (!tctx->episode_valid || !burst) {
			censor_episode(tctx, tctx->snapshot.last_censor_reason ==
				       LERTICK_CENSOR_ACCOUNTING ? LERTICK_CENSOR_ACCOUNTING :
				       LERTICK_CENSOR_ATTACH_OR_UNVERIFIED_WAKE);
		} else {
			__s32 target = lertick_scale_ns_q16(burst, LERTICK_REFERENCE_NS);
			u64 bounded = burst > LERTICK_REFERENCE_NS ? LERTICK_REFERENCE_NS : burst;

			/* Prequential errors: wake-time predictions, before this label updates history. */
			if (mode != LERTICK_MODE_FIXED &&
			    tctx->snapshot.completed_bursts >= min_train_samples) {
				u64 err = tctx->prediction_ns > bounded ?
					tctx->prediction_ns - bounded : bounded - tctx->prediction_ns;
				u64 ewma_err = tctx->ewma_prediction_ns > bounded ?
					tctx->ewma_prediction_ns - bounded : bounded - tctx->ewma_prediction_ns;

				tctx->snapshot.prediction_samples++;
				tctx->snapshot.linear_abs_error_ns += err;
				tctx->snapshot.ewma_abs_error_ns += ewma_err;
				tctx->snapshot.linear_squared_error_us2 += (err / 1000) * (err / 1000);
				tctx->snapshot.ewma_squared_error_us2 += (ewma_err / 1000) * (ewma_err / 1000);
				tctx->snapshot.raw_burst_ns_sum += burst;
				tctx->snapshot.clipped_targets += burst > LERTICK_REFERENCE_NS;
				stat_add(LERTICK_STAT_PREDICTION_SAMPLES, 1);
				stat_add(LERTICK_STAT_LINEAR_ABS_ERROR_NS, err);
				stat_add(LERTICK_STAT_EWMA_ABS_ERROR_NS, ewma_err);
			}

			tctx->snapshot.completed_bursts++;
			tctx->snapshot.last_burst_ns = burst;
			tctx->snapshot.last_target_q16 = target;
			tctx->snapshot.last_censor_reason = LERTICK_CENSOR_NONE;
			if (burst > tctx->snapshot.max_burst_ns)
				tctx->snapshot.max_burst_ns = burst;
			stat_add(LERTICK_STAT_COMPLETED_BURSTS, 1);
			if (mode == LERTICK_MODE_SHADOW || mode == LERTICK_MODE_ONLINE) {
				u64 started = profile_model ? bpf_ktime_get_ns() : 0;

				tctx->snapshot.last_training_error_q16 =
					lertick_model_train(&tctx->model, tctx->features, target);
				if (profile_model)
					stat_add(LERTICK_STAT_TRAINING_TIME_NS, bpf_ktime_get_ns() - started);
				stat_add(LERTICK_STAT_TRAINING_UPDATES, 1);
			}
			tctx->last_burst_ns = burst;
			/* Cap history before arithmetic; target itself is capped at 10ms. */
			if (burst > LERTICK_REFERENCE_NS)
				burst = LERTICK_REFERENCE_NS;
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

s32 BPF_STRUCT_OPS(lertick_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
						BPF_LOCAL_STORAGE_GET_F_CREATE);

	if (!tctx) {
		stat_add(LERTICK_STAT_ALLOC_FAILURES, 1);
		/* Runtime ownership is essential: reject rather than mischarge vtime. */
		return -ENOMEM;
	}
	lertick_model_init(&tctx->model);
	refresh_tracking(p, tctx);
	stat_add(LERTICK_STAT_INIT_TASKS, 1);
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
	censor_episode(tctx, p->flags & PF_EXITING ? LERTICK_CENSOR_EXIT :
			LERTICK_CENSOR_POLICY_TRANSITION);
	tctx->episode_active = false;
	tctx->episode_valid = false;
	tctx->has_slept = false;
	publish(tctx);
}

void BPF_STRUCT_OPS(lertick_exit_task, struct task_struct *p,
		    struct scx_exit_task_args *args)
{
	struct task_ctx *tctx = task_ctx(p);

	if (!tctx)
		return;
	stat_add(LERTICK_STAT_EXIT_TASKS, 1);
	censor_episode(tctx, LERTICK_CENSOR_EXIT);
	tctx->episode_active = false;
	tctx->episode_valid = false;
	tctx->snapshot.exited = 1;
	publish(tctx);
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
	.select_cpu = (void *)lertick_select_cpu,
	.enqueue = (void *)lertick_enqueue,
	.dispatch = (void *)lertick_dispatch,
	.runnable = (void *)lertick_runnable,
	.running = (void *)lertick_running,
	.stopping = (void *)lertick_stopping,
	.quiescent = (void *)lertick_quiescent,
	.init_task = (void *)lertick_init_task,
	.exit_task = (void *)lertick_exit_task,
	.enable = (void *)lertick_enable,
	.disable = (void *)lertick_disable,
	.init = (void *)lertick_init,
	.exit = (void *)lertick_exit,
	.timeout_ms = 1000U,
	.name = "scx_lertick");
