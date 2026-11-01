/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LERTICK_CPUPERF_H
#define LERTICK_CPUPERF_H

#include "model.h"

/*
 * CPU performance targets for schedutil, and the per-CPU signals that the
 * optional Lerwatt plug-in publishes.
 *
 * Linux v7.3 sets every CPU's sched_ext performance target to SCX_CPUPERF_ONE
 * when a BPF scheduler attaches (kernel/sched/ext/ext.c). While sched_ext runs
 * every task, schedutil's sugov_get_util() uses that target instead of CFS
 * utilization, so a scheduler that never calls scx_bpf_cpuperf_set() keeps each
 * cpufreq policy at its highest operating point. schedutil treats the target as
 * utilization on the capacity scale: effective_cpu_util() clamps it to
 * arch_scale_cpu_capacity() and sugov_effective_cpu_perf() adds 25% headroom.
 * The target is therefore wall-clock busy fraction x current/maximum frequency
 * x CPU capacity, the quantity PELT provides for CFS; a CPU that is 80% busy
 * keeps its frequency.
 *
 * All arithmetic is unsigned: the BPF build targets ISA v3.
 */

#define LERTICK_CPUPERF_ONE 1024U
/* Time constant of the smoothed busy fraction. */
#define LERTICK_UTIL_TAU_NS 16000000ULL
/* A utilization window closes no sooner than this. */
#define LERTICK_UTIL_MIN_WINDOW_NS 1000000ULL
/* Period of the timer that lowers the targets of idle CPUs. */
#define LERTICK_IDLE_DECAY_PERIOD_NS 8000000ULL
/* An episode is latency-critical when it follows a sleep no longer than this... */
#define LERTICK_LATCRIT_SLEEP_NS 32000000ULL
/* ...and the task's previous CPU burst was no longer than this. */
#define LERTICK_LATCRIT_BURST_NS 2000000ULL
#define LERTICK_WAKE_HIST_BUCKETS 32
/* Lerwatt prices remove at most this share of a target (1000 = all of it). */
#define LERTICK_PRICE_TRIM_MILLI 200U

enum lertick_cpuperf_mode {
	/* Never call scx_bpf_cpuperf_set(): Linux keeps SCX_CPUPERF_ONE. */
	LERTICK_CPUPERF_MAX = 0,
	/* Targets follow per-CPU utilization. */
	LERTICK_CPUPERF_UTIL = 1,
};

/* Busy share of an interval on the 0..1024 scale. */
LERTICK_INLINE __u32 lertick_busy_frac(__u64 busy_ns, __u64 elapsed_ns)
{
	if (!elapsed_ns)
		return 0;
	if (busy_ns >= elapsed_ns)
		return LERTICK_CPUPERF_ONE;
	/* Keep busy_ns * 1024 below 2^64 for intervals longer than ~18 minutes. */
	if (elapsed_ns > (1ULL << 40)) {
		busy_ns >>= 10;
		elapsed_ns >>= 10;
	}
	return (__u32)(busy_ns * LERTICK_CPUPERF_ONE / elapsed_ns);
}

/*
 * Time-weighted moving average: move avg toward sample by min(elapsed, tau)/tau,
 * rounding to nearest. After tau without a busy nanosecond the average is zero.
 */
LERTICK_INLINE __u32 lertick_util_blend(__u32 avg, __u32 sample, __u64 elapsed_ns,
					__u64 tau_ns)
{
	__u64 weight, delta;

	if (avg > LERTICK_CPUPERF_ONE)
		avg = LERTICK_CPUPERF_ONE;
	if (sample > LERTICK_CPUPERF_ONE)
		sample = LERTICK_CPUPERF_ONE;
	if (!tau_ns || elapsed_ns >= tau_ns)
		return sample;
	weight = elapsed_ns * LERTICK_CPUPERF_ONE / tau_ns;
	if (sample >= avg) {
		delta = ((__u64)(sample - avg) * weight + LERTICK_CPUPERF_ONE / 2) /
			LERTICK_CPUPERF_ONE;
		return avg + (__u32)delta;
	}
	delta = ((__u64)(avg - sample) * weight + LERTICK_CPUPERF_ONE / 2) /
		LERTICK_CPUPERF_ONE;
	return avg - (__u32)delta;
}

/*
 * Utilization on the capacity scale. util is the smoothed wall-clock busy
 * share (0..1024); cur is scx_bpf_cpuperf_cur(), the current/maximum frequency
 * of this CPU; cap is scx_bpf_cpuperf_cap(), its capacity relative to the
 * fastest CPU. Zero cur or cap means "unknown" and counts as 1024.
 */
LERTICK_INLINE __u32 lertick_util_capacity(__u32 util, __u32 cur, __u32 cap)
{
	if (util > LERTICK_CPUPERF_ONE)
		util = LERTICK_CPUPERF_ONE;
	if (!cur || cur > LERTICK_CPUPERF_ONE)
		cur = LERTICK_CPUPERF_ONE;
	if (!cap || cap > LERTICK_CPUPERF_ONE)
		cap = LERTICK_CPUPERF_ONE;
	return (__u32)(((__u64)util * cur * cap) >> 20);
}

/*
 * Final target. Standalone Lertick passes price = floor = ceiling = 0 and gets
 * lertick_util_capacity(). With a fresh Lerwatt contract, the energy price
 * (0..1000) trims up to 20% of the target, which removes schedutil's 25%
 * headroom at the highest price; the floor applies while latency-critical work
 * runs; a nonzero ceiling bounds the request. The result never exceeds cap.
 */
LERTICK_INLINE __u32 lertick_cpuperf_target(__u32 util, __u32 cur, __u32 cap,
					    __u32 price_milli, __u32 floor_cap,
					    __u32 ceiling_cap)
{
	__u64 target = lertick_util_capacity(util, cur, cap);

	if (!cap || cap > LERTICK_CPUPERF_ONE)
		cap = LERTICK_CPUPERF_ONE;
	if (price_milli > 1000)
		price_milli = 1000;
	target = target * (1000000ULL - (__u64)price_milli * LERTICK_PRICE_TRIM_MILLI) /
		 1000000ULL;
	if (target < floor_cap)
		target = floor_cap;
	if (ceiling_cap && target > ceiling_cap)
		target = ceiling_cap;
	if (target > cap)
		target = cap;
	return (__u32)target;
}

/*
 * Latency-critical episode: a verified wakeup after a short sleep, from a task
 * whose previous CPU burst was short (UI, audio and request/response loops). A
 * task that sleeps for long intervals, such as a periodic daemon, never
 * qualifies, so an idle system reports no latency-critical demand.
 */
LERTICK_INLINE int lertick_latency_critical(int verified_wake, __u64 sleep_ns,
					    __u64 last_burst_ns)
{
	return verified_wake && sleep_ns && sleep_ns <= LERTICK_LATCRIT_SLEEP_NS &&
	       last_burst_ns && last_burst_ns <= LERTICK_LATCRIT_BURST_NS;
}

/* Histogram bucket b holds [2^b, 2^(b+1)) ns; bucket 0 also holds 0. */
LERTICK_INLINE __u32 lertick_log2_bucket(__u64 value)
{
	__u32 bucket = 0;

	if (value >> 32) {
		value >>= 32;
		bucket += 32;
	}
	if (value >> 16) {
		value >>= 16;
		bucket += 16;
	}
	if (value >> 8) {
		value >>= 8;
		bucket += 8;
	}
	if (value >> 4) {
		value >>= 4;
		bucket += 4;
	}
	if (value >> 2) {
		value >>= 2;
		bucket += 2;
	}
	if (value >> 1)
		bucket += 1;
	return bucket < LERTICK_WAKE_HIST_BUCKETS ? bucket : LERTICK_WAKE_HIST_BUCKETS - 1;
}

#endif /* LERTICK_CPUPERF_H */
