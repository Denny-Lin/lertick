/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SPROUT_SHARED_H
#define SPROUT_SHARED_H

#include "model.h"

#define SPROUT_COMM_LEN 16
#define SPROUT_REFERENCE_NS 10000000ULL
#define SPROUT_DEFAULT_SLICE_NS 2000000ULL
#define SPROUT_MIN_SLICE_NS 500000ULL
#define SPROUT_MAX_SLICE_NS 5000000ULL
#define SPROUT_MIN_TRAIN_SAMPLES 8U
#define SPROUT_MAX_TASK_SNAPSHOTS 4096U

enum sprout_mode {
	SPROUT_MODE_FIXED = 0,
	SPROUT_MODE_SHADOW = 1,
	SPROUT_MODE_ONLINE = 2,
};

enum sprout_stat {
	SPROUT_STAT_LOCAL_ENQUEUE,
	SPROUT_STAT_GLOBAL_ENQUEUE,
	SPROUT_STAT_RUNS,
	SPROUT_STAT_RUNTIME_NS,
	SPROUT_STAT_COMPLETED_BURSTS,
	SPROUT_STAT_CENSORED_BURSTS,
	SPROUT_STAT_TRAINING_UPDATES,
	SPROUT_STAT_INFERENCES,
	SPROUT_STAT_ADAPTIVE_SLICES,
	SPROUT_STAT_SLICE_CHANGES,
	SPROUT_STAT_FAULT_STALLS,
	SPROUT_STAT_COUNTER_ERRORS,
	SPROUT_STAT_ALLOC_FAILURES,
	SPROUT_STAT_SNAPSHOT_FAILURES,
	SPROUT_STAT_PREDICTION_NS_SUM,
	SPROUT_STAT_SELECTED_SLICE_NS_SUM,
	SPROUT_STAT_MATCHING_DISPATCH_SLICES,
	SPROUT_STAT_COUNT,
};

enum sprout_censor_reason {
	SPROUT_CENSOR_NONE,
	SPROUT_CENSOR_ATTACH_OR_UNVERIFIED_WAKE,
	SPROUT_CENSOR_EXIT,
	SPROUT_CENSOR_ACCOUNTING,
	SPROUT_CENSOR_POLICY_TRANSITION,
};

/*
 * Retained after task exit for a finite experiment. Key is Linux TID, not TGID.
 * Live reads are approximate snapshots; readers should also collect a final
 * snapshot after their workload has exited. Scheduling state lives separately
 * in task storage and is never reconstructed from this userspace-visible map.
 */
struct sprout_task_snapshot {
	__u32 pid;
	__u32 tgid;
	char comm[SPROUT_COMM_LEN];
	__u64 runs;
	__u64 enqueue_count;
	__u64 total_runtime_ns;
	__u64 completed_bursts;
	__u64 censored_bursts;
	__u64 training_updates;
	__u64 inferences;
	__u64 adaptive_slices;
	__u64 slice_changes;
	__u64 last_burst_ns;
	__u64 max_burst_ns;
	__u64 episode_runtime_ns;
	__u64 prediction_ns;
	__u64 selected_slice_ns;
	__u64 min_selected_slice_ns;
	__u64 max_selected_slice_ns;
	__u64 min_adaptive_slice_ns;
	__u64 max_adaptive_slice_ns;
	__u64 observed_running_slice_ns;
	__u64 min_running_slice_ns;
	__u64 max_running_slice_ns;
	__u64 matching_dispatch_slices;
	__u64 adaptive_runs;
	__u64 slice_sum_ns;
	__u64 last_runtime_delta_ns;
	__u64 last_sleep_interval_ns;
	__u64 last_queue_depth;
	__s32 model_weight_q16[SPROUT_FEATURES];
	__s32 features_q16[SPROUT_FEATURES];
	__s32 last_target_q16;
	__s32 last_prediction_q16;
	__s32 last_training_error_q16;
	__u32 last_censor_reason;
	__u8 episode_active;
	__u8 episode_valid;
	__u8 exited;
	__u8 reserved;
};

#endif /* SPROUT_SHARED_H */
