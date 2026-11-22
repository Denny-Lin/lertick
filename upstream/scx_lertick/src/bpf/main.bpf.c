/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Hungyu Lin <dennylin0707@gmail.com>
 *
 * scx_lertick: weighted virtual-time scheduling with learned time slices.
 *
 * Tasks wait in one DSQ per LLC, ordered by a virtual deadline: the weighted
 * virtual time plus half the slice the task is given, scaled by its weight
 * like the virtual time. An idle CPU steals
 * from another LLC's DSQ. The virtual clock advances with the CPU
 * time actually delivered over the total runnable weight, so tasks that keep
 * waking cannot hold back tasks that have already run. A woken task is placed
 * at most LAG_NS behind the clock.
 *
 * In online mode each task trains a small linear model inside BPF on its own
 * completed CPU bursts (wake to sleep). At the next wakeup the prediction sets
 * the task's time slice, bounded by slice_min and slice_max. A new process
 * starts from the model earlier tasks with the same name trained, and a
 * thread whose bursts depend on who wakes it is told apart by its waker.
 * Fixed mode uses slice_fixed for every task. Fairness always charges the CPU
 * time actually used, never the prediction.
 *
 * Each CPU also has a DSQ of its own. Whether a woken task preempts the task
 * running on its CPU, and whether a woken or preempted task waits on its
 * CPU's DSQ or its LLC's, is learned for each program (see choose()).
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

/* Wakers remembered per task. */
#define NR_WAKERS 4

/*
 * Share of a task's slice, in percent, added to its vtime to order the DSQ.
 * A task predicted to run briefly gets a short slice and so an earlier
 * deadline, and runs before longer ones that are about as far behind in
 * vtime, without preempting anyone. Without the offset, the schbench -m 4
 * -t 2 wakeup p99 on 4 CPUs moved between 3 and 5 ms; with 100 or 200
 * percent, perf messaging with processes on 8 CPUs was about 5% slower.
 */
#define DEADLINE_PCT 50

/*
 * Update count a task keeps when it takes over its program's model, so that
 * it still learns with a large step (see train_model()).
 */
#define PROG_INHERIT_UPDATES 2ULL

/*
 * Scheduling mode and slice bounds, set by user space before load.
 */
const volatile u32 mode	       = LERTICK_MODE_ONLINE;
const volatile u64 slice_fixed = 2ULL * NSEC_PER_MSEC;
const volatile u64 slice_min   = 500ULL * NSEC_PER_USEC;
const volatile u64 slice_max   = 3ULL * NSEC_PER_MSEC;
/*
 * Bursts a task's model must have learned from before its prediction sets
 * its slice. A task that takes over its program's model counts as having
 * learned PROG_INHERIT_UPDATES of them.
 */
const volatile u32 min_train_samples = 2;

/*
 * Statistics, read by user space. Each CPU counts in its own slot, so the
 * counters add no cache line that all CPUs write.
 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, u32);
	__type(value, u64);
	__uint(max_entries, LERTICK_NR_STATS);
} stats SEC(".maps");

static __always_inline void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);

	if (cnt)
		(*cnt)++;
}

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
	u64 episode_work_ns; /* the same, scaled by CPU capacity */
	u32 run_cap; /* capacity of the CPU it last ran on */
	s32 rotate_to; /* big CPU + 1 that dispatch() hands it to */
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
	char comm[16]; /* program whose model this task last took over */
	u32  waker; /* pid that woke the current episode */
	u32  waker_pid[NR_WAKERS]; /* the last wakers of this task ... */
	u64  waker_burst[NR_WAKERS]; /* ... and the last burst each one started */
	u32  waker_next;
	u64  waker_err, avg_err; /* recent error of the two burst guesses */
	u64  dl_off; /* deadline offset in dsq_vtime while queued */
	bool wake_sync; /* the last wakeup was a sync one */
	u64  queued_ts; /* queued instead of preempting */
	u64  preempted_ts;
	u32  queued_key, preempted_key;
	u64  qrun_wait, qrun_ts; /* wait after not preempting, run start */
	u32  qrun_key;
	u64  prun_ts; /* run start of a task that preempted */
	u32  prun_key;
	bool qrun_on, prun_on, prun_pend;
	u64  place_ts; /* when the queue choice queued it */
	u32  place_key;
	bool place_cpu; /* on its CPU's queue, not the LLC's */
	u64  cpuq_ts, cpuq_wait; /* run start and wait, after a CPU queue */
	u32  cpuq_key;
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

/*
 * A model per task name (comm), trained with every completed burst of the
 * tasks with that name. Short-lived processes, such as a compiler's, then
 * predict from their first burst with what earlier processes of the program
 * learned. A task that renames itself only changes which model it starts
 * from: the prediction sets the slice, never the vtime charge. Updates take
 * no lock, so concurrent ones can be lost; that only slows the learning.
 */
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

/* keep_prev() compares the running task with the deadline of a fixed slice. */
#define LERTICK_PREV_DEADLINE(p, tctx)                     \
	(slice_fixed * DEADLINE_PCT /                      \
	 ((tctx) && (tctx)->eff_weight ? (tctx)->eff_weight : 100))
#define LERTICK_MOVE_TO_LOCAL(dsq) scx_bpf_dsq_move_to_local(dsq, 0)
#define LERTICK_TASK_CGROUP(p) scx_bpf_task_cgroup(p)
/* take_next() follows the CPU queue rules below (see own_first()). */
static __always_inline bool own_first(s32 cpu);
static __always_inline void took_shared(u32 home, u64 t0);
#define LERTICK_OWN_FIRST(cpu) own_first(cpu)
#define LERTICK_NOW() bpf_ktime_get_ns()
#define LERTICK_TOOK_SHARED(home, t0) took_shared(home, t0)
#include "policy.h"

/*
 * A new model predicts its burst guess (feature 1) as it is, so it is no
 * worse than that guess before it has learned anything.
 */
static void start_model(struct lertick_model *model)
{
	lertick_model_init(model);
	model->weight_q16[0] = 0;
	model->weight_q16[1] = LERTICK_Q;
}

/*
 * One least-mean-squares step. The step starts large and shrinks (1/8 for
 * the first 8 updates, 1/16 up to 32, then 1/64), so a model adapts within a
 * few bursts and is stable afterwards.
 */
static void train_model(struct lertick_model *model,
			const __s32 features[LERTICK_FEATURES], __s32 target_q16)
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

static bool same_comm(const char *a, const char *b)
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
	struct prog_key	 key = {};
	struct prog_val *v;

	if (mode != LERTICK_MODE_ONLINE || same_comm(tctx->comm, p->comm))
		return;
	__builtin_memcpy(tctx->comm, p->comm, sizeof(tctx->comm));
	__builtin_memcpy(key.comm, p->comm, sizeof(key.comm));
	v = bpf_map_lookup_elem(&prog_stor, &key);
	if (!v)
		return;
	tctx->model	    = v->model;
	tctx->ewma_burst_ns = v->ewma_burst_ns;
	if (tctx->model.updates > PROG_INHERIT_UPDATES)
		tctx->model.updates = PROG_INHERIT_UPDATES;
}

/* Train @p's program model with the burst that just completed. */
static void prog_learn(struct task_struct *p, struct task_ctx *tctx,
		       __s32 target, u64 burst)
{
	struct prog_key	 key = {};
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
	v->ewma_burst_ns = v->ewma_burst_ns ?
				   v->ewma_burst_ns * 3 / 4 + burst / 4 :
				   burst;
}

/* Slot of the current episode's waker among the task's last wakers, or -1. */
static __always_inline int waker_slot(struct task_ctx *tctx)
{
	int i, slot = -1;

	bpf_for(i, 0, NR_WAKERS)
	{
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
static u64 burst_guess(struct task_ctx *tctx)
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
	u64 g	 = slot >= 0 ? tctx->waker_burst[slot % NR_WAKERS] :
			       MIN(tctx->last_burst_ns, REFERENCE_NS);
	u64 a	 = tctx->ewma_burst_ns;

	tctx->waker_err = tctx->waker_err * 7 / 8 +
			  (g > burst ? g - burst : burst - g) / 8;
	tctx->avg_err	= tctx->avg_err * 7 / 8 +
			  (a > burst ? a - burst : burst - a) / 8;
	if (slot < 0) {
		slot = tctx->waker_next % NR_WAKERS;
		tctx->waker_next++;
		tctx->waker_pid[slot % NR_WAKERS] = tctx->waker;
	}
	tctx->waker_burst[slot % NR_WAKERS] = burst;
}

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
	tctx->episode_work_ns	 = 0;
	tctx->prediction_ns	 = 0;
	/* On a wakeup the running task is the waker. */
	tctx->waker		 = (u32)bpf_get_current_pid_tgid();

	tctx->features[0]	 = LERTICK_Q;
	tctx->features[1] =
		lertick_scale_ns_q16(burst_guess(tctx), REFERENCE_NS);
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

/*
 * CPU capacities, from the loader, the largest 1024. cpus_asym is set when
 * some CPU is more than 5% below the largest in the kernel's capacities, or
 * when scx_utils finds little cores, as on a machine with big and little
 * cores. Without it, nothing below changes the scheduling: every
 * capacity is 1024.
 *
 * With it, bursts are learned as work, their run time scaled by the
 * capacity of the CPU they ran on, so a burst learned on a little CPU
 * predicts the right time on a big one; a woken task whose bursts run long
 * takes an idle CPU of the largest capacity; and a task that keeps running
 * on a little CPU is handed to an idle big one (asym_rotate).
 */
const volatile u16  cpu_cap[LERTICK_MAX_CPUS];
const volatile bool cpus_asym;
const volatile bool asym_rotate = true;

#define BIG_CAP 973 /* within 5% of the largest */
/* Tasks handed to a big CPU wait here, taken by that CPU only. */
#define ROT_DSQ(cpu) (0x20000ULL + (u64)(cpu))

static __always_inline u32 cap_of(s32 cpu)
{
	u16 c = cpu_cap[cpu & (LERTICK_MAX_CPUS - 1)];

	return cpus_asym && c ? c : 1024;
}

/*
 * SMT siblings, from the loader: cpu_sibling[cpu] is the other hardware
 * thread of the CPU's core plus one, 0 for none. smt_on is set when some
 * core has two threads. A task that keeps running on a core whose other
 * thread is busy too moves to a whole idle core (smt_balance), as the
 * kernel's load balancing spreads busy threads over cores.
 */
const volatile u16  cpu_sibling[LERTICK_MAX_CPUS];
const volatile bool smt_on;
const volatile bool smt_balance = true;

static __always_inline s32 sibling_of(s32 cpu)
{
	return smt_on ? (s32)cpu_sibling[cpu & (LERTICK_MAX_CPUS - 1)] - 1 : -1;
}

/* Is the other thread of @cpu's core busy? */
static __always_inline bool sibling_busy(s32 cpu)
{
	const struct cpumask *idle;
	s32		      sib = sibling_of(cpu);
	bool		      busy;

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
	bpf_for(i, 0, n)
	{
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
 * is worth moving while at least slice_min of that is left, the time below
 * which the kernel also treats a task's cache as still hot, or once it has
 * run twice as long, so that the prediction has plainly missed. A task about
 * to finish its burst then stays rather than finishing on a cold cache.
 * Without a trusted prediction, it is worth moving once it has run past the
 * fixed slice.
 */
const volatile bool pred_move = true;

static __always_inline bool worth_moving(struct task_ctx *tctx, u64 run)
{
	u32 cap = tctx->run_cap ? tctx->run_cap : 1024;
	u64 expect;

	if (!pred_move || mode != LERTICK_MODE_ONLINE || !tctx->episode_valid ||
	    tctx->model.updates < min_train_samples)
		return run >= slice_fixed;
	expect = MAX(tctx->prediction_ns, tctx->ewma_burst_ns) * 1024 / cap;
	return run >= 2 * expect || run + slice_min <= expect;
}

/* Do tasks handed over in dispatch() wait in ROT_DSQ()? */
static __always_inline bool handover_on(void)
{
	return (cpus_asym && asym_rotate) || (smt_on && smt_balance);
}

/*
 * Remaining slice: the predicted burst less what already ran, within bounds.
 * A burst predicted past slice_max is cut whatever its slice, so it gets
 * slice_fixed: a longer slice would only make waiting tasks wait longer.
 */
static u64 task_slice(struct task_ctx *tctx, u64 consumed_ns)
{
	u64 slice = slice_fixed, pred = tctx ? tctx->prediction_ns : 0;

	/* Work to time on the CPU the task last ran on (see cpus_asym). */
	if (cpus_asym && tctx) {
		u32 cap	    = tctx->run_cap ? tctx->run_cap : 1024;
		u64 pending = consumed_ns > tctx->episode_runtime_ns ?
				      consumed_ns - tctx->episode_runtime_ns :
				      0;

		pred	    = pred * 1024 / cap;
		consumed_ns = tctx->episode_work_ns * 1024 / cap + pending;
	}
	if (tctx && mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	    tctx->model.updates >= min_train_samples && pred <= slice_max &&
	    pred > consumed_ns) {
		slice = pred - consumed_ns;
		stat_inc(LERTICK_STAT_ADAPTIVE_SLICES);
	}

	return CLAMP(slice, slice_min, slice_max);
}

/*
 * Learned choices, one cell per program and decision, each with the average
 * cost of its two options and the average deviation of that cost. The
 * decisions are whether a woken task preempts the task running on its CPU,
 * and where a woken task that does not preempt, or a preempted task, waits:
 * on its CPU's queue or on its LLC's queue. Each CPU keeps its own table,
 * so that CPUs do not write each other's cache lines.
 */
#define CHOICE_CELLS 4096
#define CHOICE_SHIFT 3
#define CHOICE_MIN 4
#define CHOICE_COST_MAX (50ULL * NSEC_PER_MSEC)
#define CHOICE_RUN_MAX 0xffffffffULL
#define CHOICE_N_MAX 0xffff

/*
 * Costs are capped at CHOICE_COST_MAX and runs at CHOICE_RUN_MAX, so a cell
 * fits in 28 bytes, stored in 32, and the table takes 128 KiB per possible
 * CPU.
 */
struct choice {
	u32 cost[2], dev[2];
	u32 run; /* run of a woken task after it preempted, average */
	u16 n[2];
	u16 nrun;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, CHOICE_CELLS);
	__type(key, u32);
	__type(value, struct choice);
} choices SEC(".maps");

/* Average time a take from an LLC's queue costs on this CPU. */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, u64);
} take_cost SEC(".maps");

/* The decisions, mixed into the hash of the program's name. */
#define CHOICE_PREEMPT 0
#define CHOICE_WOKEN 0x6a09e667u
#define CHOICE_PREEMPTED 0x27d4eb2fu

/* The cell of @p's program for @decision. */
static __always_inline u32 choice_key(struct task_struct *p, u32 decision)
{
	u32 h = 2166136261u;

#pragma unroll
	for (int i = 0; i < 16; i++) {
		if (!p->comm[i])
			break;
		h = (h ^ (u8)p->comm[i]) * 16777619u;
	}
	return (h ^ decision) % CHOICE_CELLS;
}

/*
 * Record @cost for option @opt of cell @key. A cost is capped at
 * CHOICE_COST_MAX, so that one long stall does not outweigh many runs.
 */
static __always_inline void choice_cost(u32 key, u32 opt, u64 cost)
{
	struct choice *c = bpf_map_lookup_elem(&choices, &key);
	u64	       dev;

	if (!c)
		return;
	opt &= 1;
	cost = MIN(cost, CHOICE_COST_MAX);
	dev  = !c->n[opt]	      ? 0 :
	       cost > c->cost[opt] ? cost - c->cost[opt] :
				       c->cost[opt] - cost;
	c->dev[opt] = c->dev[opt] - (c->dev[opt] >> CHOICE_SHIFT) +
		      (dev >> CHOICE_SHIFT);
	if (c->n[opt])
		c->cost[opt] = c->cost[opt] - (c->cost[opt] >> CHOICE_SHIFT) +
			       (cost >> CHOICE_SHIFT);
	else
		c->cost[opt] = cost;
	if (c->n[opt] < CHOICE_N_MAX)
		c->n[opt]++;
}

/*
 * Pick option 1 of cell @key? Each option's cost is drawn from its average
 * plus or minus its deviation, and the lower draw wins, so an option whose
 * cost is close or uncertain is still tried now and then. Each option is
 * tried CHOICE_MIN times first.
 */
static __always_inline bool choose(u32 key)
{
	struct choice *c = bpf_map_lookup_elem(&choices, &key);
	s64	       r0, r1;

	if (!c)
		return true;
	if (c->n[0] < CHOICE_MIN || c->n[1] < CHOICE_MIN)
		return c->n[1] <= c->n[0];
	r0 = (s64)(bpf_get_prandom_u32() % 2001) - 1000;
	r1 = (s64)(bpf_get_prandom_u32() % 2001) - 1000;
	return (s64)c->cost[1] + r1 * (s64)c->dev[1] / 1000 <
	       (s64)c->cost[0] + r0 * (s64)c->dev[0] / 1000;
}

/*
 * The head deadline of each LLC's queue, lowered on insertion and cleared on
 * a take, each on its own cache line, so that a CPU can compare it with its
 * own queue without taking the LLC queue's lock.
 */
struct llc_head {
	u64 v;
	u64 pad[7];
};
struct llc_head llc_heads[LERTICK_MAX_DOMAINS];

static __always_inline struct llc_head *llc_head_of(u32 d)
{
	return &llc_heads[d & (LERTICK_MAX_DOMAINS - 1)];
}

/* The vtime of the head of @dsq, if it has one. */
static __always_inline bool dsq_head_vtime(u64 dsq, u64 *vt)
{
	struct task_struct *q;

	bpf_for_each(scx_dsq, q, dsq, 0) {
		*vt = q->scx.dsq_vtime;
		return true;
	}
	return false;
}

/*
 * A CPU tries its own queue before its LLC queue while its own head deadline
 * is at most CPUQ_WINDOW_NS after the LLC queue's head deadline, read from a
 * cached copy. Otherwise it tries the LLC queue first and its own queue
 * after it.
 */
#define CPUQ_WINDOW_NS (2ULL * NSEC_PER_MSEC)

/* LERTICK_OWN_FIRST() for take_next(), see CPUQ_WINDOW_NS. */
static __always_inline bool own_first(s32 cpu)
{
	u32  home = domain_of(cpu);
	u64  own = 0, sh;

	if (!dsq_head_vtime(CPU_DSQ(cpu), &own))
		return false;
	if (!scx_bpf_dsq_nr_queued(home))
		return true;
	sh = llc_head_of(home)->v;
	if (!sh && dsq_head_vtime(home, &sh))
		llc_head_of(home)->v = sh;
	return !sh || !time_before(sh + CPUQ_WINDOW_NS, own);
}

/* A take from LLC queue @home started at @t0: time it, and drop the head. */
static __always_inline void took_shared(u32 home, u64 t0)
{
	u64 d = bpf_ktime_get_ns() - t0, *tc;
	u32 z = 0;

	tc = bpf_map_lookup_elem(&take_cost, &z);
	if (tc)
		*tc = *tc ? *tc - *tc / 8 + d / 8 : d;
	if (llc_head_of(home)->v)
		llc_head_of(home)->v = 0;
}

/*
 * Does the head of @cpu's own queue come before @prev, as keep_prev() asks
 * of the LLC queue?
 */
static __always_inline bool cpu_head_before(struct task_struct *prev, s32 cpu)
{
	struct task_ctx *pt = task_ctx(prev);
	u64		 pv = prev->scx.dsq_vtime, ran = 0, h = 0;
	u32		 pw = pt && pt->eff_weight ? pt->eff_weight :
			      prev->scx.weight	    ? prev->scx.weight :
							1;

	if (pt && pt->running &&
	    prev->se.sum_exec_runtime > pt->runtime_start_ns)
		ran = prev->se.sum_exec_runtime - pt->runtime_start_ns;
	pv += ran * 100 / pw + LERTICK_PREV_DEADLINE(prev, pt);
	return dsq_head_vtime(CPU_DSQ(cpu), &h) && time_before(h, pv);
}

/*
 * Does woken task @p, at @vtime, preempt the task running on its CPU? It may
 * when its vtime is before the running task's vtime plus the run not yet
 * charged and a fixed slice, both scaled by the running task's weight, much
 * as EEVDF checks a wakeup. Then it does as learned for its program. Not
 * preempting costs the wait of @p over the run that follows it. Preempting
 * costs the wait of the preempted task over the average run of @p after a
 * preemption. A wakee that runs a few microseconds and sleeps again is then
 * not worth the preemption that a long run would be.
 */
static __always_inline bool wake_preempt(struct task_struct *p,
					 struct task_ctx *tctx, u64 vtime,
					 u64 slice, u64 enq_flags)
{
	s32		    c	= scx_bpf_task_cpu(p);
	struct task_struct *cur = __COMPAT_scx_bpf_cpu_curr(c);
	struct task_ctx	   *ct	= cur && cur != p ? task_ctx(cur) : NULL;
	u64		    ran = 0, dl;
	u32		    cw, key;

	if (!ct || !ct->running)
		return false;
	cw = ct->eff_weight ? ct->eff_weight : 100;
	if (cur->se.sum_exec_runtime > ct->runtime_start_ns)
		ran = cur->se.sum_exec_runtime - ct->runtime_start_ns;
	dl = cur->scx.dsq_vtime + (ran + slice_fixed) * 100 / cw;
	if (!time_before(vtime, dl))
		return false;

	key = choice_key(p, CHOICE_PREEMPT);
	if (!choose(key)) {
		tctx->queued_ts	 = bpf_ktime_get_ns();
		tctx->queued_key = key;
		return false;
	}
	ct->preempted_ts  = bpf_ktime_get_ns();
	ct->preempted_key = key;
	tctx->prun_key	  = key;
	tctx->prun_pend	  = true;
	p->scx.dsq_vtime  = vtime;
	scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | c, slice,
			   enq_flags | SCX_ENQ_PREEMPT);
	stat_inc(LERTICK_STAT_PREEMPTS);
	return true;
}

/*
 * Pick a CPU for a waking task. An idle CPU gets the task on its local DSQ
 * right away, and the waker's CPU on a sync wakeup that sync_local() accepts
 * gets it on its CPU queue. Otherwise ops.enqueue() queues it.
 */
s32 BPF_STRUCT_OPS(lertick_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	struct task_ctx *tctx = task_ctx(p);
	bool		 idle = false;
	s32		 cpu  = -1;

	/* A task whose bursts run long takes an idle big CPU if one is free. */
	if (cpus_asym && tctx && tctx->ewma_burst_ns >= slice_fixed) {
		cpu  = pick_big_idle(p, prev_cpu);
		idle = cpu >= 0;
	}
	if (cpu < 0)
		cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &idle);

	/* select_cpu() precedes runnable() on a wakeup and can insert directly. */
	if (tctx && !tctx->episode_active) {
		prog_sync(p, tctx);
		begin_episode(tctx, SCX_ENQ_WAKEUP);
		tctx->started_in_select = true;
	}
	if (tctx)
		tctx->wake_sync = wake_flags & SCX_WAKE_SYNC;
	if (idle) {
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL,
				   task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0),
				   0);
		stat_inc(LERTICK_STAT_LOCAL_DISPATCHES);
	} else if ((wake_flags & SCX_WAKE_SYNC) && sync_local(p, &cpu)) {
		queue_on_cpu(p, cpu,
			     task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0));
		stat_inc(LERTICK_STAT_LOCAL_DISPATCHES);
	} else if (tctx) {
		/*
		 * No CPU is idle: queue it on the waker's CPU, where enqueue()
		 * may let it preempt the waker, as the kernel's wake_affine
		 * does.
		 */
		s32		 this = bpf_get_smp_processor_id();
		struct task_ctx *wt   = task_ctx(bpf_get_current_task_btf());

		if (wt && wt->running &&
		    bpf_cpumask_test_cpu(this, p->cpus_ptr))
			cpu = this;
	}

	return cpu;
}

/*
 * Queue @p in deadline order, with its vtime held within the lag bounds: on
 * its CPU's queue or its LLC's queue, unless it preempts the running task.
 * The deadline offset is kept in dsq_vtime only while @p is queued:
 * running() takes it out, so the charge and the clock use the vtime.
 */
void BPF_STRUCT_OPS(lertick_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx  = task_ctx(p);
	u64		 vtime = p->scx.dsq_vtime;
	u64 slice = task_slice(tctx, tctx ? tctx->episode_runtime_ns : 0);
	s32  cpu   = scx_bpf_task_cpu(p);
	bool ahead;

	/* A task dispatch() handed to a big CPU waits for that CPU only. */
	if (tctx && tctx->rotate_to) {
		s32 dst = tctx->rotate_to - 1;

		tctx->rotate_to = 0;
		if (!(enq_flags & SCX_ENQ_WAKEUP) &&
		    bpf_cpumask_test_cpu(dst, p->cpus_ptr)) {
			scx_bpf_dsq_insert_vtime(p, ROT_DSQ(dst), slice_fixed,
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

	/* A task dequeued without running still carries its offset. */
	if (tctx && tctx->dl_off) {
		vtime -= tctx->dl_off;
		tctx->dl_off = 0;
	}

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

	if (tctx) {
		bool sync = tctx->wake_sync;

		tctx->wake_sync = false;
		/* a sync wakeup seen in select_cpu() does not preempt */
		if ((enq_flags & SCX_ENQ_WAKEUP) && !sync &&
		    wake_preempt(p, tctx, vtime, slice, enq_flags))
			return;
	}

	/*
	 * A task whose vtime is past the clock has had more than its share and
	 * waits on the LLC queue. Any other task waits on its CPU's queue or
	 * on the LLC queue, as learned for its program, separately for woken
	 * and preempted tasks. The CPU queue avoids the LLC queue's lock,
	 * which more CPUs contend for. So the LLC queue is charged the time a
	 * take from it costs, and the CPU queue the wait it adds to the LLC
	 * queue's head.
	 */
	ahead = time_before(vtime_now, vtime);
	if (tctx) {
		u32 w = tctx->eff_weight ? tctx->eff_weight :
			p->scx.weight	 ? p->scx.weight :
					   1;

		tctx->dl_off = slice * DEADLINE_PCT / w;
		vtime += tctx->dl_off;
	}
	if (tctx && !ahead) {
		u32 key = choice_key(p, (enq_flags & SCX_ENQ_WAKEUP) ?
						CHOICE_WOKEN :
						CHOICE_PREEMPTED);

		tctx->place_cpu = choose(key);
		tctx->place_ts	= bpf_ktime_get_ns();
		tctx->place_key = key;
		if (tctx->place_cpu) {
			scx_bpf_dsq_insert_vtime(p, CPU_DSQ(cpu), slice, vtime,
						 enq_flags);
			/*
			 * No kick: its CPU is the one it ran on or the one
			 * select_cpu() picked, and a kick on every such insert
			 * cut futex throughput by more than half.
			 */
			stat_inc(LERTICK_STAT_CPU_QUEUED);
			return;
		}
	}
	scx_bpf_dsq_insert_vtime(p, domain_of(cpu), slice, vtime, enq_flags);
	if (llc_head_of(domain_of(cpu))->v &&
	    time_before(vtime, llc_head_of(domain_of(cpu))->v))
		llc_head_of(domain_of(cpu))->v = vtime;
	stat_inc(LERTICK_STAT_SHARED_DISPATCHES);
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

	if (handover_on()) {
		u64 run = 0;
		s32 dst = -1;

		/* A task handed to this CPU first. */
		if (LERTICK_MOVE_TO_LOCAL(ROT_DSQ(cpu)))
			return;
		/*
		 * A task that has run past the fixed slice in its current burst
		 * on a little CPU goes to an idle big CPU, as the kernel moves a
		 * misfit task. It is not kept here: the kernel queues it again
		 * (SCX_OPS_ENQ_LAST, which the loader sets on such machines)
		 * and enqueue() hands it over.
		 */
		tctx = continuing ? task_ctx(prev) : NULL;
		if (tctx) {
			run = tctx->episode_runtime_ns;
			if (tctx->running &&
			    prev->se.sum_exec_runtime > tctx->runtime_start_ns)
				run += prev->se.sum_exec_runtime -
				       tctx->runtime_start_ns;
		}
		if (tctx && worth_moving(tctx, run) && cpus_asym && asym_rotate &&
		    cap_of(cpu) < BIG_CAP)
			dst = pick_big_idle(prev, -1);
		/*
		 * Likewise, a task that keeps running while the other thread of
		 * its core is busy goes to a whole idle core of the same or a
		 * larger capacity.
		 */
		if (dst < 0 && tctx && worth_moving(tctx, run) && smt_on &&
		    smt_balance && sibling_busy(cpu)) {
			dst = pick_idle(prev, -1, cap_of(cpu) >= BIG_CAP, true);
			if (dst >= 0 && cap_of(dst) < cap_of(cpu)) {
				/* Never to a slower CPU: give the claim back. */
				scx_bpf_kick_cpu(dst, 0);
				dst = -1;
			}
		}
		if (dst >= 0 && tctx) {
			stat_inc(LERTICK_STAT_HANDOVERS);
			tctx->rotate_to = dst + 1;
			take_next(cpu);
			return;
		}
	}

	if (!((continuing && keep_prev(prev, task_ctx(prev), cpu) &&
	       !cpu_head_before(prev, cpu)) ||
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
	stat_inc(LERTICK_STAT_CONTINUATIONS);
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
 * @p starts running after waiting: record the costs of the choices that made
 * it wait. A cost that depends on the run that follows is recorded when it
 * stops.
 */
static __always_inline void learn_on_run(struct task_struct *p,
					 struct task_ctx *tctx)
{
	u64 now = bpf_ktime_get_ns(), r, *tc;
	u32 z	= 0;

	if (tctx->queued_ts && now > tctx->queued_ts) {
		tctx->qrun_wait = now - tctx->queued_ts;
		tctx->qrun_key	= tctx->queued_key;
		tctx->qrun_ts	= now;
		tctx->qrun_on	= true;
	}
	if (tctx->preempted_ts && now > tctx->preempted_ts) {
		u32	       k = tctx->preempted_key;
		struct choice *c = bpf_map_lookup_elem(&choices, &k);

		r = c && c->nrun ? c->run : now - tctx->preempted_ts;
		choice_cost(k, 1, (now - tctx->preempted_ts) * 1000 /
					  MAX(r, NSEC_PER_USEC));
	}
	tctx->queued_ts	   = 0;
	tctx->preempted_ts = 0;

	if (tctx->place_ts && now > tctx->place_ts) {
		if (!tctx->place_cpu) {
			tc = bpf_map_lookup_elem(&take_cost, &z);
			choice_cost(tctx->place_key, 0,
				    now - tctx->place_ts + (tc ? *tc : 0));
		} else if (scx_bpf_dsq_nr_queued(
				   domain_of(scx_bpf_task_cpu(p)))) {
			tctx->cpuq_ts	= now;
			tctx->cpuq_wait = now - tctx->place_ts;
			tctx->cpuq_key	= tctx->place_key;
		} else {
			choice_cost(tctx->place_key, 1, now - tctx->place_ts);
		}
	}
	tctx->place_ts = 0;

	if (tctx->prun_pend) {
		tctx->prun_ts	= now;
		tctx->prun_on	= true;
		tctx->prun_pend = false;
	}
}

/* @p stops: record the costs that depend on the run that just ended. */
static __always_inline void learn_on_stop(struct task_ctx *tctx)
{
	u64 now = bpf_ktime_get_ns();

	if (tctx->qrun_on && now > tctx->qrun_ts)
		choice_cost(tctx->qrun_key, 0,
			    tctx->qrun_wait * 1000 /
				    MAX(now - tctx->qrun_ts, NSEC_PER_USEC));
	if (tctx->prun_on && now > tctx->prun_ts) {
		u32	       k = tctx->prun_key;
		struct choice *c = bpf_map_lookup_elem(&choices, &k);
		u64	       r = MIN(now - tctx->prun_ts, CHOICE_RUN_MAX);

		if (c) {
			c->run = c->nrun ? c->run - c->run / 8 + r / 8 : r;
			if (c->nrun < CHOICE_N_MAX)
				c->nrun++;
		}
	}
	/* the LLC queue's head waited for this run too */
	if (tctx->cpuq_ts && now > tctx->cpuq_ts)
		choice_cost(tctx->cpuq_key, 1,
			    tctx->cpuq_wait + now - tctx->cpuq_ts);
	tctx->qrun_on = false;
	tctx->prun_on = false;
	tctx->cpuq_ts = 0;
}

/*
 * @p starts running: take its deadline offset out of its vtime, raise the
 * clock to that vtime if the clock is behind, and note where this run starts.
 */
void BPF_STRUCT_OPS(lertick_running, struct task_struct *p)
{
	struct task_ctx *tctx = task_ctx(p);

	if (tctx && tctx->dl_off) {
		p->scx.dsq_vtime -= tctx->dl_off;
		tctx->dl_off = 0;
	}
	clock_raise(p->scx.dsq_vtime);

	if (!tctx)
		return;
	learn_on_run(p, tctx);
	tctx->runtime_start_ns = p->se.sum_exec_runtime;
	tctx->running	       = true;
	tctx->run_cap	       = cap_of(scx_bpf_task_cpu(p));
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

	if (tctx)
		learn_on_stop(tctx);
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
	tctx->episode_work_ns += delta * cap_of(scx_bpf_task_cpu(p)) / 1024;
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
	 * Bursts are learned as work (see cpus_asym), which is their run time
	 * when the CPUs are alike.
	 */
	burst = tctx->episode_work_ns;
	if (tctx->episode_active && tctx->episode_valid && burst &&
	    !(p->flags & PF_EXITING)) {
		__s32 target = lertick_scale_ns_q16(burst, REFERENCE_NS);

		if (mode == LERTICK_MODE_ONLINE) {
			train_model(&tctx->model, tctx->features, target);
			stat_inc(LERTICK_STAT_TRAINING_UPDATES);
			prog_learn(p, tctx, target, MIN(burst, REFERENCE_NS));
			learn_waker(tctx, MIN(burst, REFERENCE_NS));
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
 * Allocate and initialize the context of a new task. Sleepable, so that the
 * context is allocated with GFP_KERNEL: an atomic allocation can fail, which
 * keeps the scheduler from starting during attach and fails a fork after.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(lertick_init_task, struct task_struct *p,
			    struct scx_init_task_args *args)
{
	struct task_ctx *tctx;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;
	start_model(&tctx->model);

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
	/* enable() restarts the vtime at the clock, without any offset. */
	tctx->dl_off	     = 0;
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

	ret = create_cpu_dsqs();
	if (ret || !handover_on())
		return ret;
	bpf_for(d, 0, scx_bpf_nr_cpu_ids())
	{
		ret = scx_bpf_create_dsq(ROT_DSQ(d), -1);
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
