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
 *
 * CPU performance targets (cpuperf_mode) and the Lerwatt plug-in
 * (lerwatt_enabled) are read-only switches that the loader sets. With both off,
 * the verifier prunes their branches and scheduling is unchanged.
 */
#include <scx/common.bpf.h>
#include "shared/lertick.h"
#include "shared/accounting.h"
#include "lerwatt/contract.h"

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
const volatile bool model_init_ewma;
const volatile bool model_update_carry;
/* LERTICK_CPUPERF_MAX never calls scx_bpf_cpuperf_set(), as before. */
const volatile u32 cpuperf_mode = LERTICK_CPUPERF_MAX;
/* Optional Lerwatt plug-in: read lw_contract, keep demand counters. */
const volatile bool lerwatt_enabled;
const volatile u64 lerwatt_session_id;
const volatile u64 cluster_topology_id[LW_CONTRACT_MAX_CLUSTERS];
const volatile u32 nr_cpus = 1;
/* Lerwatt cluster of each CPU; LERTICK_NO_CLUSTER for none. */
const volatile u8 cpu_cluster[LERTICK_MAX_CPUS];
/* Nonzero replaces scx_bpf_cpuperf_cap(), e.g. a simulated board's capacity. */
const volatile u16 cpu_cap_override[LERTICK_MAX_CPUS];

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
	/* Lerwatt plug-in only: classification of the current episode. */
	bool latcrit;
	u64 lc_last_burst_ns;
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

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct lertick_cpu_state);
} cpu_state SEC(".maps");

struct lertick_idle_timer {
	struct bpf_timer timer;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct lertick_idle_timer);
} idle_timer_map SEC(".maps");

/*
 * Lerwatt contract (vendor/lerwatt/contract.h). The loader pins both maps only
 * with --lerwatt. Lerwatt writes lw_contract; the loader writes lt_demand from
 * cpu_state. This program never writes either map.
 */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, LW_CONTRACT_MAX_CLUSTERS);
	__type(key, u32);
	__type(value, struct lw_contract_rec);
} lw_contract SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(map_flags, BPF_F_MMAPABLE);
	__uint(max_entries, LW_CONTRACT_MAX_CLUSTERS);
	__type(key, u32);
	__type(value, struct lt_demand_rec);
} lt_demand SEC(".maps");

static __always_inline void stat_add(u32 idx, u64 amount)
{
	u64 *value = bpf_map_lookup_elem(&stats, &idx);

	if (!value)
		return;
	/* init_task is unlocked: migration pinning does not exclude preemption. */
	if (idx == LERTICK_STAT_INIT_TASKS || idx == LERTICK_STAT_ALLOC_FAILURES)
		__sync_fetch_and_add(value, amount);
	else
		*value += amount;
}

static __always_inline void stat_max(u32 idx, u64 amount)
{
	u64 *value = bpf_map_lookup_elem(&stats, &idx);

	if (value && amount > *value)
		*value = amount;
}

/*
 * Per-CPU utilization, performance targets and Lerwatt demand counters.
 * Busy time is wall-clock time while a sched_ext task runs on the CPU.
 */
static __always_inline bool cpu_tracking(void)
{
	return cpuperf_mode == LERTICK_CPUPERF_UTIL || lerwatt_enabled;
}

static __always_inline struct lertick_cpu_state *cpu_state_local(void)
{
	u32 zero = 0;

	return bpf_map_lookup_elem(&cpu_state, &zero);
}

static __always_inline u32 cpu_capacity(s32 cpu)
{
	u32 idx = cpu, cap = 0;

	if (idx < LERTICK_MAX_CPUS)
		cap = cpu_cap_override[idx];
	if (!cap)
		cap = scx_bpf_cpuperf_cap(cpu);
	return cap;
}

struct lertick_contract {
	u32 price_milli;
	u32 floor_cap;
	u32 ceiling_cap;
};

/*
 * Copy the Lerwatt record of this CPU's cluster. The seqcount check uses
 * compiler barriers around aligned fields. Identity is fixed for one Lertick
 * attachment; incompatible, out-of-range and stale records are ignored.
 */
static __always_inline bool contract_get(s32 cpu, u64 now, struct lertick_contract *out)
{
	struct lw_contract_rec *rec;
	struct lw_contract_rec copy = {0};
	u32 idx = cpu, cluster, seq;
	u64 expected_topology_id;

	out->price_milli = 0;
	out->floor_cap = 0;
	out->ceiling_cap = 0;
	if (!lerwatt_enabled || idx >= LERTICK_MAX_CPUS)
		return false;
	cluster = cpu_cluster[idx];
	if (cluster >= LW_CONTRACT_MAX_CLUSTERS)
		return false;
	/* Load with the explicit bound before map_lookup_elem() spills the key
	 * and the verifier may discard the scalar range attached to it.
	 */
	expected_topology_id = cluster_topology_id[cluster];
	rec = bpf_map_lookup_elem(&lw_contract, &cluster);
	if (!rec)
		return false;
	seq = READ_ONCE(rec->seq);
	barrier();
	copy.version = READ_ONCE(rec->version);
	copy.energy_price_milli = READ_ONCE(rec->energy_price_milli);
	copy.floor_cap = READ_ONCE(rec->floor_cap);
	copy.ceiling_cap = READ_ONCE(rec->ceiling_cap);
	copy.valid_until_ns = READ_ONCE(rec->valid_until_ns);
	copy.topology_id = READ_ONCE(rec->topology_id);
	copy.session_id = READ_ONCE(rec->session_id);
	barrier();
	if (!seq || (seq & 1) || READ_ONCE(rec->seq) != seq ||
	    !lw_contract_constraint_valid(&copy, expected_topology_id,
					  lerwatt_session_id, now)) {
		stat_add(LERTICK_STAT_CONTRACT_STALE, 1);
		return false;
	}
	out->price_milli = copy.energy_price_milli;
	out->floor_cap = copy.floor_cap;
	out->ceiling_cap = copy.ceiling_cap;
	stat_add(LERTICK_STAT_CONTRACT_APPLIED, 1);
	return true;
}

/* Close the open utilization window once it is long enough. Owning CPU only. */
static __always_inline void cpu_util_update(struct lertick_cpu_state *c, u64 now)
{
	u64 elapsed;

	if (!c->window_start_ns || now < c->window_start_ns) {
		c->window_start_ns = now;
		c->busy_ns = 0;
		return;
	}
	elapsed = now - c->window_start_ns;
	if (elapsed < LERTICK_UTIL_MIN_WINDOW_NS)
		return;
	c->util = lertick_util_blend(c->util, lertick_busy_frac(c->busy_ns, elapsed),
				     elapsed, LERTICK_UTIL_TAU_NS);
	c->busy_ns = 0;
	c->window_start_ns = now;
}

static __always_inline u32 cpu_compute_target(struct lertick_cpu_state *c, s32 cpu,
					      u32 util, u64 now, bool latcrit, bool owner)
{
	struct lertick_contract k;
	u32 cur = scx_bpf_cpuperf_cur(cpu), cap = cpu_capacity(cpu);

	if (owner)
		c->util_cap = lertick_util_capacity(util, cur, cap);
	contract_get(cpu, now, &k);
	return lertick_cpuperf_target(util, cur, cap, k.price_milli,
				      latcrit ? k.floor_cap : 0, k.ceiling_cap);
}

static __always_inline void cpu_send(struct lertick_cpu_state *c, s32 cpu, u32 target,
				     u64 now, bool resend, u32 stat)
{
	if (!resend && c->perf_sent && c->perf_target == target)
		return;
	scx_bpf_cpuperf_set(cpu, target);
	WRITE_ONCE(c->perf_target, target);
	WRITE_ONCE(c->perf_set_ns, now);
	WRITE_ONCE(c->perf_sent, 1);
	stat_add(stat, 1);
}

static __always_inline void cpu_on_stopping(void)
{
	struct lertick_cpu_state *c;
	u64 now;

	if (!cpu_tracking())
		return;
	c = cpu_state_local();
	if (!c)
		return;
	now = bpf_ktime_get_ns();
	if (c->run_mark_ns && now > c->run_mark_ns)
		c->busy_ns += now - c->run_mark_ns;
	c->run_mark_ns = 0;
	c->latcrit_running = 0;
}

static __always_inline void cpu_account_run(u64 delta_ns, bool latcrit)
{
	struct lertick_cpu_state *c;

	if (!lerwatt_enabled)
		return;
	c = cpu_state_local();
	if (!c)
		return;
	c->run_ns += delta_ns;
	if (latcrit)
		c->latcrit_ns += delta_ns;
}

static __always_inline void cpu_wake_sample(u64 latency_ns)
{
	struct lertick_cpu_state *c;
	u32 bucket;

	if (!lerwatt_enabled)
		return;
	c = cpu_state_local();
	if (!c)
		return;
	bucket = lertick_log2_bucket(latency_ns);
	if (bucket < LERTICK_WAKE_HIST_BUCKETS)
		c->wake_hist[bucket]++;
}

/*
 * A CPU that goes idle gets no running() or tick(), and schedutil takes the
 * maximum target over a policy's CPUs, so a stale target would hold the whole
 * cluster at a high frequency. This timer lowers idle CPUs' targets along the
 * same moving average the CPU itself would compute. No rq lock is held here,
 * so scx_bpf_cpuperf_set() takes the target CPU's lock.
 */
static int idle_timer_fn(void *map, int *key, struct bpf_timer *timer)
{
	u64 now = bpf_ktime_get_ns();
	u32 zero = 0, ncpus = nr_cpus;
	s32 cpu;

	if (ncpus > LERTICK_MAX_CPUS)
		ncpus = LERTICK_MAX_CPUS;
	bpf_for(cpu, 0, ncpus) {
		struct lertick_cpu_state *c;
		u64 start, elapsed;
		u32 util, target;

		c = bpf_map_lookup_percpu_elem(&cpu_state, &zero, cpu);
		if (!c || READ_ONCE(c->run_mark_ns))
			continue;
		start = READ_ONCE(c->window_start_ns);
		elapsed = start && now > start ? now - start : 0;
		util = lertick_util_blend(READ_ONCE(c->util),
					  lertick_busy_frac(READ_ONCE(c->busy_ns), elapsed),
					  elapsed, LERTICK_UTIL_TAU_NS);
		target = cpu_compute_target(c, cpu, util, now, false, false);
		if (READ_ONCE(c->perf_sent) && READ_ONCE(c->perf_target) == target)
			continue;
		cpu_send(c, cpu, target, now, true, LERTICK_STAT_CPUPERF_IDLE_SETS);
	}
	bpf_timer_start(timer, LERTICK_IDLE_DECAY_PERIOD_NS, 0);
	return 0;
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
	if (lerwatt_enabled)
		tctx->latcrit = lertick_latency_critical(tctx->episode_valid, sleep_ns,
							 tctx->lc_last_burst_ns);
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
	if (lerwatt_enabled && !tctx->episode_valid)
		tctx->latcrit = false;
	publish(tctx);
}

static __always_inline void cpu_on_running(struct task_struct *p, struct task_ctx *tctx)
{
	struct lertick_cpu_state *c;
	u32 target;
	u64 now;
	s32 cpu;

	if (!cpu_tracking())
		return;
	c = cpu_state_local();
	if (!c)
		return;
	now = bpf_ktime_get_ns();
	cpu = scx_bpf_task_cpu(p);
	c->run_mark_ns = now;
	c->latcrit_running = tctx && tctx->latcrit;
	cpu_util_update(c, now);
	target = cpu_compute_target(c, cpu, c->util, now, c->latcrit_running, true);
	if (cpuperf_mode == LERTICK_CPUPERF_UTIL)
		cpu_send(c, cpu, target, now, false, LERTICK_STAT_CPUPERF_SETS);
}

void BPF_STRUCT_OPS(lertick_running, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);
	u64 observed_slice = p->scx.slice;

	/* As in scx_simple, this cross-CPU monotonic update tolerates small races. */
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
	stat_add(LERTICK_STAT_RUNS, 1);
	cpu_on_running(p, tctx);
	if (!tctx)
		return;
	refresh_tracking(p, tctx);
	if (lerwatt_enabled && tctx->wake_ns && tctx->episode_valid)
		cpu_wake_sample(bpf_ktime_get_ns() - tctx->wake_ns);
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

	cpu_on_stopping();
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
	cpu_account_run(delta, tctx->latcrit);
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
					model_update_carry ? lertick_model_train_carry(&tctx->model, tctx->features, target) :
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
	if (lerwatt_enabled) {
		tctx->lc_last_burst_ns = tctx->episode_runtime_ns;
		tctx->latcrit = false;
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
	if (model_init_ewma)
		lertick_model_init_ewma(&tctx->model);
	else
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

void BPF_STRUCT_OPS(lertick_tick, struct task_struct *p)
{
	struct lertick_cpu_state *c;
	u32 target;
	u64 now;
	s32 cpu;

	if (!cpu_tracking())
		return;
	c = cpu_state_local();
	if (!c)
		return;
	now = bpf_ktime_get_ns();
	if (!c->run_mark_ns) {
		c->run_mark_ns = now;
	} else if (now > c->run_mark_ns) {
		c->busy_ns += now - c->run_mark_ns;
		c->run_mark_ns = now;
	}
	cpu_util_update(c, now);
	cpu = scx_bpf_task_cpu(p);
	target = cpu_compute_target(c, cpu, c->util, now, c->latcrit_running, true);
	/* schedutil drops a request inside its rate limit without a trace: resend. */
	if (cpuperf_mode == LERTICK_CPUPERF_UTIL)
		cpu_send(c, cpu, target, now, true, LERTICK_STAT_CPUPERF_SETS);
}

s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_init)
{
	struct lertick_idle_timer *t;
	u32 zero = 0;
	s32 ret;

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret || cpuperf_mode != LERTICK_CPUPERF_UTIL)
		return ret;
	t = bpf_map_lookup_elem(&idle_timer_map, &zero);
	if (!t)
		return -ESRCH;
	ret = bpf_timer_init(&t->timer, &idle_timer_map, CLOCK_MONOTONIC);
	if (!ret)
		ret = bpf_timer_set_callback(&t->timer, idle_timer_fn);
	if (!ret)
		ret = bpf_timer_start(&t->timer, LERTICK_IDLE_DECAY_PERIOD_NS, 0);
	return ret;
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
	.tick = (void *)lertick_tick,
	.init_task = (void *)lertick_init_task,
	.exit_task = (void *)lertick_exit_task,
	.enable = (void *)lertick_enable,
	.disable = (void *)lertick_disable,
	.init = (void *)lertick_init,
	.exit = (void *)lertick_exit,
	.timeout_ms = 1000U,
	.name = "scx_lertick");
