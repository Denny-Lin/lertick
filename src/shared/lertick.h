/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LERTICK_SHARED_H
#define LERTICK_SHARED_H

#include "model.h"

#define LERTICK_COMM_LEN 16
#define LERTICK_REFERENCE_NS 10000000ULL
#define LERTICK_DEFAULT_SLICE_NS 2000000ULL
#define LERTICK_MIN_SLICE_NS 500000ULL
#define LERTICK_MAX_SLICE_NS 5000000ULL
#define LERTICK_MIN_TRAIN_SAMPLES 8U
#define LERTICK_MAX_TASK_SNAPSHOTS 4096U

enum lertick_mode {
	LERTICK_MODE_FIXED = 0,
	LERTICK_MODE_SHADOW = 1,
	LERTICK_MODE_ONLINE = 2,
};

enum lertick_stat {
	LERTICK_STAT_LOCAL_ENQUEUE,
	LERTICK_STAT_GLOBAL_ENQUEUE,
	LERTICK_STAT_RUNS,
	LERTICK_STAT_RUNTIME_NS,
	LERTICK_STAT_COMPLETED_BURSTS,
	LERTICK_STAT_CENSORED_BURSTS,
	LERTICK_STAT_TRAINING_UPDATES,
	LERTICK_STAT_INFERENCES,
	LERTICK_STAT_ADAPTIVE_SLICES,
	LERTICK_STAT_SLICE_CHANGES,
	LERTICK_STAT_FAULT_STALLS,
	LERTICK_STAT_COUNTER_ERRORS,
	LERTICK_STAT_ALLOC_FAILURES,
	LERTICK_STAT_SNAPSHOT_FAILURES,
	LERTICK_STAT_PREDICTION_NS_SUM,
	LERTICK_STAT_SELECTED_SLICE_NS_SUM,
	LERTICK_STAT_MATCHING_DISPATCH_SLICES,
	LERTICK_STAT_COUNT,
};

enum lertick_censor_reason {
	LERTICK_CENSOR_NONE,
	LERTICK_CENSOR_ATTACH_OR_UNVERIFIED_WAKE,
	LERTICK_CENSOR_EXIT,
	LERTICK_CENSOR_ACCOUNTING,
	LERTICK_CENSOR_POLICY_TRANSITION,
};

/*
 * Retained after task exit for a finite experiment. Key is Linux TID, not TGID.
 * Live reads are approximate snapshots; readers should also collect a final
 * snapshot after their workload has exited. Scheduling state lives separately
 * in task storage and is never reconstructed from this userspace-visible map.
 */
struct lertick_task_snapshot {
	__u32 pid;
	__u32 tgid;
	char comm[LERTICK_COMM_LEN];
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
	__s32 model_weight_q16[LERTICK_FEATURES];
	__s32 features_q16[LERTICK_FEATURES];
	__s32 last_target_q16;
	__s32 last_prediction_q16;
	__s32 last_training_error_q16;
	__u32 last_censor_reason;
	__u8 episode_active;
	__u8 episode_valid;
	__u8 exited;
	__u8 reserved;
};

#endif /* LERTICK_SHARED_H */
