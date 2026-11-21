// SPDX-License-Identifier: GPL-2.0
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
/*
 * Copy each learned task's snapshot to task_snapshots for user space. Off
 * with --summary-only, which never reads them: with every task learned, the
 * copies on each callback slowed hackbench -g 64 by about 70%.
 */
const volatile bool publish_snapshots = true;
/* Disposable-VM watchdog test only; userspace gates this behind partial mode. */
const volatile bool fault_stall;
const volatile u32 stall_tgid;
/* Optional diagnostic clocks; primary benchmarks leave this disabled. */
const volatile bool profile_model;
const volatile u32 model_init = LERTICK_MODEL_INIT_GUESS;
const volatile u32 model_update = LERTICK_MODEL_UPDATE_DECAY;
/* LERTICK_CPUPERF_MAX never calls scx_bpf_cpuperf_set(), as before. */
const volatile u32 cpuperf_mode = LERTICK_CPUPERF_MAX;
/* Optional Lerwatt plug-in: read lw_contract, keep demand counters. */
const volatile bool lerwatt_enabled;
/*
 * Loader can disable advice after a best-effort publication failure, before
 * attach. Keep this writable: rodata is frozen when BPF is loaded.
 */
bool lerwatt_runtime_disabled;
const volatile u64 lerwatt_session_id;
const volatile u64 cluster_topology_id[LW_CONTRACT_MAX_CLUSTERS];
const volatile u32 nr_cpus = 1;
/* Lerwatt cluster of each CPU; LERTICK_NO_CLUSTER for none. */
const volatile u8 cpu_cluster[LERTICK_MAX_CPUS];
/* Nonzero replaces scx_bpf_cpuperf_cap(), e.g. a simulated board's capacity. */
const volatile u16 cpu_cap_override[LERTICK_MAX_CPUS];

#define LERTICK_QUEUE_REFERENCE 16U

/* Wakers remembered per task. */
#define NR_WAKERS 4

/*
 * Share of a task's slice, in percent, added to its vtime to order the DSQ.
 * A task predicted to run briefly gets a short slice and so an earlier
 * deadline, and runs before longer ones that are about as far behind in
 * vtime, without preempting anyone.
 */
#define DEADLINE_PCT 50

/*
 * Update count a task keeps when it takes over its program's model, so that
 * it still learns with a large step (see train_model()).
 */
#define PROG_INHERIT_UPDATES 2ULL

/*
 * CPU capacities for scheduling, from the loader, the largest 1024. Unlike
 * cpu_cap_override, which only shapes frequency requests, these change
 * where tasks run. cpus_asym is set when some CPU is more than 5% below the
 * largest in the kernel's capacities, or when the loader finds little cores.
 * Without it every capacity is 1024 and nothing below changes scheduling.
 *
 * With it, bursts are learned as work, their run time scaled by the
 * capacity of the CPU they ran on; a woken task whose bursts run long takes
 * an idle CPU of the largest capacity; and a task that keeps running on a
 * little CPU is handed to an idle big one (asym_rotate).
 */
const volatile u16 cpu_cap[LERTICK_MAX_CPUS];
const volatile bool cpus_asym;
const volatile bool asym_rotate = true;

/*
 * SMT siblings, from the loader: cpu_sibling[cpu] is the other hardware
 * thread of the CPU's core plus one, 0 for none. A task that keeps running
 * on a core whose other thread is busy too moves to a whole idle core
 * (smt_balance).
 */
const volatile u16 cpu_sibling[LERTICK_MAX_CPUS];
const volatile bool smt_on;
const volatile bool smt_balance = true;
/* Decide moves of running tasks by their predicted burst (worth_moving()). */
const volatile bool pred_move = true;

#define BIG_CAP 973 /* within 5% of the largest */
/* Tasks handed to a CPU wait here, taken by that CPU only. */
#define ROT_DSQ(cpu) (0x20000ULL + (u64)(cpu))

UEI_DEFINE(uei);

struct task_ctx {
	struct lertick_model model;
	__s32 features[LERTICK_FEATURES];
	u64 runtime_start_ns;
	u64 episode_runtime_ns;
	u64 episode_work_ns;	/* the same, scaled by CPU capacity */
	u32 run_cap;		/* capacity of the CPU it last ran on */
	s32 rotate_to;		/* CPU + 1 that dispatch() hands it to */
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
	u32 rq_weight;		/* weight counted in runnable_weight, 0 when not counted */
	bool cg_counted;	/* rq_weight is also counted in its cgroup's sums */
	u32 eff_weight;		/* weight of the last charge; dispatch() cannot look up cgroups */
	char prog_comm[LERTICK_COMM_LEN]; /* program whose model it last took over */
	u32 waker;		/* pid that woke the current episode */
	u32 waker_pid[NR_WAKERS]; /* the last wakers of this task ... */
	u64 waker_burst[NR_WAKERS]; /* ... and the last burst each one started */
	u32 waker_next;
	u64 waker_err, avg_err;	/* recent error of the two burst guesses */
	u64 dl_off;		/* deadline offset in dsq_vtime while queued */
	struct lertick_task_snapshot snapshot;
};

/*
 * A model per task name (comm), trained with every completed burst of the
 * learned tasks with that name. Short-lived processes, such as a compiler's,
 * then predict from their first burst with what earlier processes of the
 * program learned. A task that renames itself only changes which model it
 * starts from: the prediction sets the slice, never the vtime charge.
 * Updates take no lock, so concurrent ones can be lost; that only slows the
 * learning.
 */
struct prog_key {
	char comm[LERTICK_COMM_LEN];
};

struct prog_val {
	struct lertick_model model;
	u64 ewma_burst_ns;
};

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 4096);
	__type(key, struct prog_key);
	__type(value, struct prog_val);
} prog_stor SEC(".maps");

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
	if (!lerwatt_enabled || READ_ONCE(lerwatt_runtime_disabled) || idx >= LERTICK_MAX_CPUS)
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

#define NSEC_PER_USEC 1000ULL
#define NSEC_PER_MSEC (1000ULL * NSEC_PER_USEC)
#define LERTICK_MOVE_TO_LOCAL(dsq) scx_bpf_dsq_move_to_local(dsq)
#define LERTICK_TASK_CGROUP(p) __COMPAT_scx_bpf_task_cgroup(p)
/* keep_prev() compares the running task with the deadline of a fixed slice. */
#define LERTICK_PREV_DEADLINE(p, tctx)                     \
	(fixed_slice_ns * DEADLINE_PCT /                   \
	 ((tctx) && (tctx)->eff_weight ? (tctx)->eff_weight : 100))
#include "policy.h"

/* A new model, with the starting weights --model-init chose. */
static __always_inline void start_model(struct lertick_model *model)
{
	if (model_init == LERTICK_MODEL_INIT_EWMA) {
		lertick_model_init_ewma(model);
		return;
	}
	lertick_model_init(model);
	if (model_init == LERTICK_MODEL_INIT_GUESS) {
		/* No worse than the burst guess before it has learned anything. */
		model->weight_q16[0] = 0;
		model->weight_q16[1] = LERTICK_Q;
	}
}

/*
 * One least-mean-squares step whose size starts large and shrinks (1/8 for
 * the first 8 updates, 1/16 up to 32, then 1/64), so a model adapts within a
 * few bursts and is stable afterwards. Returns the clipped residual.
 */
static __s32 train_decay(struct lertick_model *model,
			 const __s32 features[LERTICK_FEATURES], __s32 target_q16)
{
	__s64 error = (__s64)lertick_clip_q16(target_q16) -
		      lertick_model_raw_q16(model, features);
	__s64 alpha = model->updates < 8 ? LERTICK_Q / 8 :
		      model->updates < 32 ? LERTICK_Q / 16 : LERTICK_ALPHA_Q16;
	int i;

	if (error > LERTICK_Q)
		error = LERTICK_Q;
	if (error < -LERTICK_Q)
		error = -LERTICK_Q;
	bpf_for(i, 0, LERTICK_FEATURES) {
		__s64 update;

		if (i >= LERTICK_FEATURES)
			break;
		update = lertick_trunc_shift(alpha * error * lertick_clip_q16(features[i]), 32);
		model->weight_q16[i] = lertick_clip_weight(
			(__s64)lertick_clip_weight(model->weight_q16[i]) + update);
	}
	if (model->updates != ~(__u64)0)
		model->updates++;
	return (__s32)error;
}

/* Train @model with the update rule --model-update chose. */
static __s32 train_model(struct lertick_model *model,
			 const __s32 features[LERTICK_FEATURES], __s32 target_q16)
{
	if (model_update == LERTICK_MODEL_UPDATE_CARRY)
		return lertick_model_train_carry(model, features, target_q16);
	if (model_update == LERTICK_MODEL_UPDATE_TRUNC)
		return lertick_model_train(model, features, target_q16);
	return train_decay(model, features, target_q16);
}

static __always_inline bool learning(void)
{
	return mode == LERTICK_MODE_ONLINE || mode == LERTICK_MODE_SHADOW;
}

static __always_inline bool same_comm(const char *a, const char *b)
{
	u64 x[2], y[2];

	__builtin_memcpy(x, a, sizeof(x));
	__builtin_memcpy(y, b, sizeof(y));
	return x[0] == y[0] && x[1] == y[1];
}

/*
 * When @p's name differs from the one its model was taken for (first wakeup,
 * exec, rename), take over the model of its new name. Its update count is
 * capped, since threads of one program can behave differently and each must
 * still adapt to its own bursts.
 */
static void prog_sync(struct task_struct *p, struct task_ctx *tctx)
{
	struct prog_key key = {};
	struct prog_val *v;

	if (!learning() || !tctx->tracked || same_comm(tctx->prog_comm, p->comm))
		return;
	__builtin_memcpy(tctx->prog_comm, p->comm, sizeof(tctx->prog_comm));
	__builtin_memcpy(key.comm, p->comm, sizeof(key.comm));
	v = bpf_map_lookup_elem(&prog_stor, &key);
	if (!v)
		return;
	tctx->model = v->model;
	tctx->ewma_burst_ns = v->ewma_burst_ns;
	if (tctx->model.updates > PROG_INHERIT_UPDATES)
		tctx->model.updates = PROG_INHERIT_UPDATES;
}

/* Train @p's program model with the burst that just completed. */
static void prog_learn(struct task_struct *p, struct task_ctx *tctx,
		       __s32 target, u64 burst)
{
	struct prog_key key = {};
	struct prog_val *v;

	__builtin_memcpy(key.comm, p->comm, sizeof(key.comm));
	v = bpf_map_lookup_elem(&prog_stor, &key);
	if (!v) {
		struct prog_val init = {};

		start_model(&init.model);
		bpf_map_update_elem(&prog_stor, &key, &init, BPF_NOEXIST);
		v = bpf_map_lookup_elem(&prog_stor, &key);
		if (!v)
			return;
	}
	train_model(&v->model, tctx->features, target);
	v->ewma_burst_ns = v->ewma_burst_ns ? v->ewma_burst_ns * 3 / 4 + burst / 4 : burst;
}

/* Slot of the current episode's waker among the task's last wakers, or -1. */
static __always_inline int waker_slot(struct task_ctx *tctx)
{
	int i, slot = -1;

	bpf_for(i, 0, NR_WAKERS) {
		if (tctx->waker_pid[i % NR_WAKERS] == tctx->waker)
			slot = i % NR_WAKERS;
	}
	return slot;
}

/*
 * The burst guess fed to the model: the last burst this waker started, while
 * that has recently been closer than the burst average, else the average.
 * A thread that alternates between two kinds of work, each started by a
 * different waker, then gets the right length for each; a task whose bursts
 * vary at random keeps the steadier average.
 */
static __always_inline u64 burst_guess(struct task_ctx *tctx)
{
	int slot = waker_slot(tctx);

	if (slot >= 0 && tctx->waker_err < tctx->avg_err)
		return tctx->waker_burst[slot % NR_WAKERS];
	return tctx->ewma_burst_ns;
}

/*
 * Score both burst guesses against the burst that just completed, and
 * remember it as this waker's last.
 */
static void learn_waker(struct task_ctx *tctx, u64 burst)
{
	int slot = waker_slot(tctx);
	u64 g = slot >= 0 ? tctx->waker_burst[slot % NR_WAKERS] :
		(tctx->last_burst_ns < LERTICK_REFERENCE_NS ? tctx->last_burst_ns :
		 LERTICK_REFERENCE_NS);
	u64 a = tctx->ewma_burst_ns;

	tctx->waker_err = tctx->waker_err * 7 / 8 + (g > burst ? g - burst : burst - g) / 8;
	tctx->avg_err = tctx->avg_err * 7 / 8 + (a > burst ? a - burst : burst - a) / 8;
	if (slot < 0) {
		slot = tctx->waker_next % NR_WAKERS;
		tctx->waker_next++;
		tctx->waker_pid[slot % NR_WAKERS] = tctx->waker;
	}
	tctx->waker_burst[slot % NR_WAKERS] = burst;
}

static __always_inline u32 cap_of(s32 cpu)
{
	u16 c = cpu_cap[cpu & (LERTICK_MAX_CPUS - 1)];

	return cpus_asym && c ? c : 1024;
}

static __always_inline s32 sibling_of(s32 cpu)
{
	return smt_on ? (s32)cpu_sibling[cpu & (LERTICK_MAX_CPUS - 1)] - 1 : -1;
}

/* Is the other thread of @cpu's core busy? */
static __always_inline bool sibling_busy(s32 cpu)
{
	const struct cpumask *idle;
	s32 sib = sibling_of(cpu);
	bool busy;

	if (sib < 0)
		return false;
	idle = scx_bpf_get_idle_cpumask();
	busy = !bpf_cpumask_test_cpu(sib, idle);
	scx_bpf_put_idle_cpumask(idle);
	return busy;
}

/* Is @cpu idle in @idle, and its sibling, if any, too? */
static __always_inline bool core_idle_in(s32 cpu, const struct cpumask *idle)
{
	s32 sib = sibling_of(cpu);

	return bpf_cpumask_test_cpu(cpu, idle) &&
	       (sib < 0 || bpf_cpumask_test_cpu(sib, idle));
}

/*
 * Claim an idle CPU for @p: of the largest capacity if @big, of a whole idle
 * core if @whole, @prev first. Returns -1 if there is none.
 */
static __always_inline s32 pick_idle(struct task_struct *p, s32 prev, bool big,
				     bool whole)
{
	const struct cpumask *idle = scx_bpf_get_idle_cpumask();
	u32 n = scx_bpf_nr_cpu_ids(), i;
	s32 cpu = -1;

	if (prev >= 0 && (!big || cap_of(prev) >= BIG_CAP) &&
	    bpf_cpumask_test_cpu(prev, p->cpus_ptr) &&
	    (!whole || core_idle_in(prev, idle)) &&
	    scx_bpf_test_and_clear_cpu_idle(prev)) {
		cpu = prev;
		goto out;
	}
	bpf_for(i, 0, n) {
		if (i >= LERTICK_MAX_CPUS)
			break;
		if ((big && cap_of(i) < BIG_CAP) ||
		    !bpf_cpumask_test_cpu(i, p->cpus_ptr) ||
		    (whole && !core_idle_in(i, idle)))
			continue;
		if (scx_bpf_test_and_clear_cpu_idle(i)) {
			cpu = i;
			break;
		}
	}
out:
	scx_bpf_put_idle_cpumask(idle);
	return cpu;
}

/* An idle CPU of the largest capacity, of a whole idle core if possible. */
static __always_inline s32 pick_big_idle(struct task_struct *p, s32 prev)
{
	s32 cpu = smt_on ? pick_idle(p, prev, true, true) : -1;

	return cpu >= 0 ? cpu : pick_idle(p, prev, true, false);
}

/*
 * Is a task that has run @run of its current burst worth moving to a better
 * CPU? With a trusted prediction, the burst is expected to last the larger of
 * the prediction and the average burst (as time on the CPU it runs on): it
 * is worth moving while at least min_slice_ns of that is left, or once it
 * has run twice as long, so that the prediction has plainly missed. Without
 * a trusted prediction, it is worth moving once it has run past the fixed
 * slice.
 */
static __always_inline bool worth_moving(struct task_ctx *tctx, u64 run)
{
	u32 cap = tctx->run_cap ? tctx->run_cap : 1024;
	u64 expect;

	if (!pred_move || mode != LERTICK_MODE_ONLINE || !tctx->episode_valid ||
	    tctx->model.updates < min_train_samples)
		return run >= fixed_slice_ns;
	expect = tctx->prediction_ns > tctx->ewma_burst_ns ? tctx->prediction_ns :
		 tctx->ewma_burst_ns;
	expect = expect * 1024 / cap;
	return run >= 2 * expect || run + min_slice_ns <= expect;
}

/* Do tasks handed over in dispatch() wait in ROT_DSQ()? */
static __always_inline bool handover_on(void)
{
	return (cpus_asym && asym_rotate) || (smt_on && smt_balance);
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

	if (!publish_snapshots || !tctx->tracked)
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
	u64 queued = nr_queued_all();

	tctx->episode_active = true;
	tctx->episode_valid = tctx->has_slept && (enq_flags & SCX_ENQ_WAKEUP);
	tctx->episode_runtime_ns = 0;
	tctx->episode_work_ns = 0;
	tctx->prediction_ns = 0;
	tctx->ewma_prediction_ns = tctx->ewma_burst_ns;
	/* On a wakeup the running task is the waker. */
	tctx->waker = (u32)bpf_get_current_pid_tgid();
	tctx->wake_ns = tctx->episode_valid ? now : 0;
	if (lerwatt_enabled)
		tctx->latcrit = lertick_latency_critical(tctx->episode_valid, sleep_ns,
							 tctx->lc_last_burst_ns);
	if (!tctx->tracked)
		return;
	tctx->snapshot.last_censor_reason = tctx->episode_valid ? LERTICK_CENSOR_NONE :
		LERTICK_CENSOR_ATTACH_OR_UNVERIFIED_WAKE;
	tctx->features[0] = LERTICK_Q;
	tctx->features[1] = lertick_scale_ns_q16(burst_guess(tctx),
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
	u64 slice = fixed_slice_ns, pred = tctx ? tctx->prediction_ns : 0;
	bool adaptive = false;

	/* Work to time on the CPU the task last ran on (see cpus_asym). */
	if (cpus_asym && tctx) {
		u32 cap = tctx->run_cap ? tctx->run_cap : 1024;
		u64 pending = consumed_ns > tctx->episode_runtime_ns ?
			consumed_ns - tctx->episode_runtime_ns : 0;

		pred = pred * 1024 / cap;
		consumed_ns = tctx->episode_work_ns * 1024 / cap + pending;
	}
	/*
	 * A burst predicted past max_slice_ns is cut whatever its slice, so it
	 * gets fixed_slice_ns: a longer slice would only make others wait.
	 */
	if (tctx && (mode == LERTICK_MODE_ONLINE || mode == LERTICK_MODE_EWMA) &&
	    tctx->tracked && tctx->episode_valid &&
	    (mode == LERTICK_MODE_EWMA ? tctx->snapshot.completed_bursts :
	     tctx->model.updates) >= min_train_samples &&
	    pred <= max_slice_ns && pred > consumed_ns) {
		slice = pred - consumed_ns;
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
	s32 cpu = -1;

	/* A task whose bursts run long takes an idle big CPU if one is free. */
	if (cpus_asym && tctx && tctx->ewma_burst_ns >= fixed_slice_ns &&
	    !is_fault_target(p)) {
		cpu = pick_big_idle(p, prev_cpu);
		idle = cpu >= 0;
	}
	if (cpu < 0)
		cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	if (tctx) {
		refresh_tracking(p, tctx);
		/* select_cpu precedes runnable on a wakeup and can insert directly. */
		if (!tctx->episode_active) {
			prog_sync(p, tctx);
			begin_episode(p, tctx, SCX_ENQ_WAKEUP);
			tctx->started_in_select = true;
		}
	}
	if (is_fault_target(p))
		return cpu;
	if (idle || ((wake_flags & SCX_WAKE_SYNC) && sync_local(p, &cpu))) {
		u64 slice = choose_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);

		stat_add(LERTICK_STAT_LOCAL_ENQUEUE, 1);
		if (idle)
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, slice, 0);
		else
			queue_on_cpu(p, cpu, slice);
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
	/* A task dispatch() handed to another CPU waits for that CPU only. */
	if (tctx && tctx->rotate_to) {
		s32 dst = tctx->rotate_to - 1;

		tctx->rotate_to = 0;
		if (!(enq_flags & SCX_ENQ_WAKEUP) &&
		    bpf_cpumask_test_cpu(dst, p->cpus_ptr)) {
			scx_bpf_dsq_insert_vtime(p, ROT_DSQ(dst), fixed_slice_ns,
						 p->scx.dsq_vtime, enq_flags);
			scx_bpf_kick_cpu(dst, 0);
			return;
		}
		/*
		 * Not handed over after all. dst was claimed as idle: kick it
		 * so that it goes idle again and can be picked.
		 */
		scx_bpf_kick_cpu(dst, 0);
	}
	if (tctx)
		refresh_tracking(p, tctx);
	slice = choose_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);
	/* A task dequeued without running still carries its offset. */
	if (tctx && tctx->dl_off) {
		vtime -= tctx->dl_off;
		tctx->dl_off = 0;
	}
	/*
	 * Sleeper credit is bounded independently of model output. Only a
	 * wakeup is a sleeper: a task requeued after its slice keeps its vtime,
	 * or tasks with larger weights would be pulled back to the clock and
	 * lose their share.
	 */
	if ((enq_flags & SCX_ENQ_WAKEUP) && time_before(vtime, vtime_now - LAG_NS))
		vtime = vtime_now - LAG_NS;
	vtime = clamp_ahead(p, vtime);
	/*
	 * Order the queue by deadline: vtime plus a share of the slice. The
	 * offset stays in dsq_vtime only while queued; running() removes it,
	 * so the charge and the clock use the vtime.
	 */
	if (tctx) {
		u32 w = tctx->eff_weight ? tctx->eff_weight :
			p->scx.weight ? p->scx.weight : 1;

		tctx->dl_off = slice * DEADLINE_PCT / w;
		vtime += tctx->dl_off;
	}
	stat_add(LERTICK_STAT_GLOBAL_ENQUEUE, 1);
	scx_bpf_dsq_insert_vtime(p, domain_of(scx_bpf_task_cpu(p)), slice, vtime, enq_flags);
	if (tctx)
		publish(tctx);
}

void BPF_STRUCT_OPS(lertick_dispatch, s32 cpu, struct task_struct *prev)
{
	bool continuing = prev && (prev->scx.flags & SCX_TASK_QUEUED) && !is_fault_target(prev);

	if (handover_on()) {
		struct task_ctx *hctx = continuing ? task_ctx(prev) : NULL;
		u64 run = 0;
		s32 dst = -1;

		/* A task handed to this CPU first. */
		if (LERTICK_MOVE_TO_LOCAL(ROT_DSQ(cpu)))
			return;
		if (hctx) {
			run = hctx->episode_runtime_ns;
			if (hctx->running && prev->se.sum_exec_runtime > hctx->runtime_start_ns)
				run += prev->se.sum_exec_runtime - hctx->runtime_start_ns;
		}
		/*
		 * A task worth moving on a little CPU goes to an idle big CPU, as
		 * the kernel moves a misfit task. It is not kept here: the kernel
		 * queues it again (SCX_OPS_ENQ_LAST, set by the loader) and
		 * enqueue() hands it over.
		 */
		if (hctx && cpus_asym && asym_rotate && cap_of(cpu) < BIG_CAP &&
		    worth_moving(hctx, run))
			dst = pick_big_idle(prev, -1);
		/*
		 * Likewise, a task that keeps running while the other thread of
		 * its core is busy goes to a whole idle core of the same or a
		 * larger capacity.
		 */
		if (dst < 0 && hctx && smt_on && smt_balance && sibling_busy(cpu) &&
		    worth_moving(hctx, run)) {
			dst = pick_idle(prev, -1, cap_of(cpu) >= BIG_CAP, true);
			if (dst >= 0 && cap_of(dst) < cap_of(cpu)) {
				/* Never to a slower CPU: give the claim back. */
				scx_bpf_kick_cpu(dst, 0);
				dst = -1;
			}
		}
		if (dst >= 0 && hctx) {
			stat_add(LERTICK_STAT_HANDOVERS, 1);
			hctx->rotate_to = dst + 1;
			take_next(cpu);
			return;
		}
	}

	/* Explicitly reinsert a continuing task to avoid the kernel's DFL refill. */
	if ((continuing && keep_prev(prev, task_ctx(prev), cpu)) || (!take_next(cpu) && continuing)) {
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
	if (!tctx->rq_weight) {
		tctx->rq_weight = p->scx.weight ? p->scx.weight : 1;
		__sync_fetch_and_add(&runnable_weight, tctx->rq_weight);
		cgrp_task_runnable(p, tctx, true);
	}
	refresh_tracking(p, tctx);
	/* Migration can produce runnable notifications without a new wakeup. */
	if (!tctx->episode_active) {
		prog_sync(p, tctx);
		begin_episode(p, tctx, enq_flags);
	} else if (tctx->started_in_select) {
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

	if (tctx && tctx->dl_off) {
		p->scx.dsq_vtime -= tctx->dl_off;
		tctx->dl_off = 0;
	}
	clock_raise(p->scx.dsq_vtime);
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
	tctx->run_cap = cap_of(scx_bpf_task_cpu(p));
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
	u64 delta, rw;
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
	tctx->episode_runtime_ns = service.consumed_ns;
	tctx->episode_work_ns += delta * cap_of(scx_bpf_task_cpu(p)) / 1024;
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

	if (tctx && tctx->rq_weight) {
		cgrp_task_runnable(p, tctx, false);
		__sync_fetch_and_sub(&runnable_weight, tctx->rq_weight);
		tctx->rq_weight = 0;
	}
	if (!tctx || !(deq_flags & SCX_DEQ_SLEEP))
		return;
	/*
	 * Audited Linux v7.0 kernel/sched/ext.c::dequeue_task_scx(): a running
	 * task receives update_curr_scx() and stopping(p, false) BEFORE this
	 * callback. put_prev_task_scx() then suppresses stopping for !QUEUED.
	 * The final service segment is therefore already included exactly once.
	 */
	/*
	 * Non-sleep quiescence preserves the episode across migration/preemption.
	 * Bursts are learned as work (see cpus_asym), which equals their run
	 * time when the CPUs are alike.
	 */
	burst = tctx->episode_work_ns;
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
					train_model(&tctx->model, tctx->features, target);
				prog_learn(p, tctx, target, bounded);
				learn_waker(tctx, bounded);
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

/*
 * Sleepable so the task storage is allocated with GFP_KERNEL. An atomic
 * allocation of the context failed in 2 of 20 quick reattaches, and a failed
 * init_task disables the scheduler.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_init_task, struct task_struct *p,
			    struct scx_init_task_args *args)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
						BPF_LOCAL_STORAGE_GET_F_CREATE);

	if (!tctx) {
		stat_add(LERTICK_STAT_ALLOC_FAILURES, 1);
		/* Runtime ownership is essential: reject rather than mischarge vtime. */
		return -ENOMEM;
	}
	start_model(&tctx->model);
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
	/* enable() restarts the vtime at the clock, without any offset. */
	tctx->dl_off = 0;
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
	u32 zero = 0, d;
	s32 ret = 0;

	bpf_for(d, 0, domain_count()) {
		ret = scx_bpf_create_dsq(d, -1);
		if (ret)
			return ret;
	}
	ret = create_cpu_dsqs();
	if (ret)
		return ret;
	if (handover_on()) {
		bpf_for(d, 0, scx_bpf_nr_cpu_ids()) {
			ret = scx_bpf_create_dsq(ROT_DSQ(d), -1);
			if (ret)
				return ret;
		}
	}
	if (cpuperf_mode != LERTICK_CPUPERF_UTIL)
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

s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_cgroup_init, struct cgroup *cgrp,
			     struct scx_cgroup_init_args *args)
{
	struct cgrp_ctx *cgc = bpf_cgrp_storage_get(&cgrp_ctx_stor, cgrp, 0,
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

SCX_OPS_DEFINE(lertick_ops,
	.select_cpu = (void *)lertick_select_cpu,
	.enqueue = (void *)lertick_enqueue,
	.dispatch = (void *)lertick_dispatch,
	.runnable = (void *)lertick_runnable,
	.running = (void *)lertick_running,
	.stopping = (void *)lertick_stopping,
	.quiescent = (void *)lertick_quiescent,
	.cgroup_init = (void *)lertick_cgroup_init,
	.cgroup_set_weight = (void *)lertick_cgroup_set_weight,
	.cgroup_move = (void *)lertick_cgroup_move,
	.tick = (void *)lertick_tick,
	.init_task = (void *)lertick_init_task,
	.exit_task = (void *)lertick_exit_task,
	.enable = (void *)lertick_enable,
	.disable = (void *)lertick_disable,
	.init = (void *)lertick_init,
	.exit = (void *)lertick_exit,
	/*
	 * Runnable-task watchdog. With hundreds of runnable tasks per CPU, a fair
	 * round alone can take most of a second, so 1 s ejected the scheduler under
	 * heavy hackbench. 5 s matches scx_bpfland and scx_flash and still bounds a
	 * real stall; the kernel default is 30 s.
	 */
	.timeout_ms = 5000U,
	.name = "scx_lertick");
