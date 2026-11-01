/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Portable tests for the CPU performance-target arithmetic in cpuperf.h, plus a
 * closed-loop check against a model of schedutil (25% headroom, frequency
 * invariance). The kernel path itself is exercised by the Linux runner.
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "cpuperf.h"

static uint64_t rng_state = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t random_u64(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static void test_busy_frac(void)
{
	assert(lertick_busy_frac(0, 0) == 0);
	assert(lertick_busy_frac(5, 0) == 0);
	assert(lertick_busy_frac(0, 1000) == 0);
	assert(lertick_busy_frac(500, 1000) == 512);
	assert(lertick_busy_frac(1000, 1000) == 1024);
	assert(lertick_busy_frac(2000, 1000) == 1024);
	assert(lertick_busy_frac(UINT64_MAX - 1, UINT64_MAX) >= 1023);
	for (int i = 0; i < 200000; i++) {
		uint64_t elapsed = random_u64() >> (random_u64() % 64);
		uint64_t busy = elapsed ? random_u64() % elapsed : 0;
		uint32_t got = lertick_busy_frac(busy, elapsed);
		double want = elapsed ? (double)busy * 1024.0 / (double)elapsed : 0;

		assert(got <= 1024);
		/* Exact below 2^40 ns; at most one step of 1/1024 coarser above. */
		assert(fabs((double)got - want) <= (elapsed > (1ULL << 40) ? 2.0 : 1.0));
	}
}

static void test_blend(void)
{
	const uint64_t tau = LERTICK_UTIL_TAU_NS;
	uint32_t avg = 0;

	assert(lertick_util_blend(300, 900, tau, tau) == 900);
	assert(lertick_util_blend(300, 900, 10 * tau, tau) == 900);
	assert(lertick_util_blend(300, 900, 0, tau) == 300);
	assert(lertick_util_blend(900, 100, tau / 2, tau) == 500);
	assert(lertick_util_blend(100, 900, tau / 4, tau) == 300);
	assert(lertick_util_blend(5000, 5000, 1, tau) == 1024);
	assert(lertick_util_blend(1, 0, tau - 1, tau) == 0);
	/* A saturated CPU sampled every 4 ms approaches 1024 without overshoot. */
	for (int k = 0; k < 64; k++) {
		uint32_t next = lertick_util_blend(avg, 1024, 4000000, tau);

		assert(next >= avg && next <= 1024);
		avg = next;
	}
	assert(avg >= 1023);
	/* The same CPU, idle for tau, decays to zero. */
	assert(lertick_util_blend(avg, 0, tau, tau) == 0);
}

static void test_capacity_and_target(void)
{
	/* RK3588 LITTLE: capacity 397, half frequency, fully busy. */
	assert(lertick_util_capacity(1024, 512, 397) == 198);
	assert(lertick_util_capacity(1024, 1024, 1024) == 1024);
	assert(lertick_util_capacity(512, 0, 0) == 512);
	assert(lertick_util_capacity(4096, 4096, 4096) == 1024);
	for (int i = 0; i < 100000; i++) {
		uint32_t util = random_u64() % 1025, cur = 1 + random_u64() % 1024;
		uint32_t cap = 1 + random_u64() % 1024;
		uint32_t base = lertick_util_capacity(util, cur, cap);
		uint32_t floor = random_u64() % 1100, ceiling = random_u64() % 1100;
		uint32_t price = random_u64() % 1200;
		uint32_t standalone = lertick_cpuperf_target(util, cur, cap, 0, 0, 0);
		uint32_t target = lertick_cpuperf_target(util, cur, cap, price, floor, ceiling);
		double trimmed = base * (1.0 - 0.2 * (price > 1000 ? 1000 : price) / 1000.0);

		assert(base == (uint32_t)(((uint64_t)util * cur * cap) >> 20));
		/* Standalone Lertick requests exactly its capacity-scale utilization. */
		assert(standalone == base && standalone <= cap);
		assert(target <= cap);
		if (floor <= cap && (!ceiling || floor <= ceiling))
			assert(target >= floor);
		if (ceiling)
			assert(target <= ceiling);
		if (!floor && !ceiling)
			assert(fabs((double)target - trimmed) <= 1.0);
	}
	/* The highest price removes schedutil's headroom: 1.25 * 0.8 = 1. */
	assert(lertick_cpuperf_target(1000, 1024, 1024, 1000, 0, 0) == 800);
}

/*
 * Closed loop: a CPU runs a fixed amount of work per second; schedutil picks
 * f = 1.25 * target / cap * fmax (clamped); the CPU is busy work / f of the
 * time. Utilization is sampled every 4 ms tick. The loop must settle where
 * schedutil intends, busy = 0.8, from both low and high frequency, without a
 * sustained oscillation.
 */
static double settle(double work, uint32_t price, double f_start, double *swing)
{
	const uint32_t cap = 1024;
	double f = f_start, lo = 1e9, hi = -1e9, busy = 0;
	uint32_t util = 0;

	for (int k = 0; k < 2000; k++) {
		uint32_t cur, target;

		busy = work / f;
		if (busy > 1)
			busy = 1;
		util = lertick_util_blend(util, (uint32_t)(busy * 1024), 4000000,
					  LERTICK_UTIL_TAU_NS);
		cur = (uint32_t)(f * 1024);
		target = lertick_cpuperf_target(util, cur, cap, price, 0, 0);
		f = 1.25 * target / cap;
		if (f > 1)
			f = 1;
		if (f < 0.1)
			f = 0.1;
		if (k >= 1800) {
			lo = busy < lo ? busy : lo;
			hi = busy > hi ? busy : hi;
		}
	}
	*swing = hi - lo;
	return busy;
}

static void test_closed_loop(void)
{
	double swing, busy;

	for (double work = 0.15; work <= 0.75; work += 0.15) {
		busy = settle(work, 0, 1.0, &swing);
		assert(busy > 0.75 && busy < 0.86 && swing < 0.03);
		busy = settle(work, 0, 0.1, &swing);
		assert(busy > 0.75 && busy < 0.86 && swing < 0.03);
		/* The highest Lerwatt price runs the same work at a lower frequency. */
		busy = settle(work, 1000, 1.0, &swing);
		assert(busy > 0.90 && swing < 0.03);
	}
	/* Work beyond the CPU's capacity saturates it at the highest frequency. */
	busy = settle(1.5, 0, 0.3, &swing);
	assert(busy == 1.0);
}

static void test_latency_and_buckets(void)
{
	assert(lertick_latency_critical(1, 16000000, 1500000));
	assert(!lertick_latency_critical(0, 16000000, 1500000));
	assert(!lertick_latency_critical(1, 0, 1500000));
	assert(!lertick_latency_critical(1, 1000000000, 1500000));
	assert(!lertick_latency_critical(1, 16000000, 0));
	assert(!lertick_latency_critical(1, 16000000, 3000000));
	assert(lertick_log2_bucket(0) == 0);
	assert(lertick_log2_bucket(1) == 0);
	assert(lertick_log2_bucket(2) == 1);
	assert(lertick_log2_bucket(3) == 1);
	assert(lertick_log2_bucket(1023) == 9);
	assert(lertick_log2_bucket(1024) == 10);
	assert(lertick_log2_bucket(1ULL << 31) == 31);
	assert(lertick_log2_bucket(1ULL << 40) == 31);
	assert(lertick_log2_bucket(UINT64_MAX) == 31);
	for (int b = 1; b < 31; b++) {
		assert(lertick_log2_bucket(1ULL << b) == (uint32_t)b);
		assert(lertick_log2_bucket((1ULL << (b + 1)) - 1) == (uint32_t)b);
	}
}

int main(void)
{
	test_busy_frac();
	test_blend();
	test_capacity_and_target();
	test_closed_loop();
	test_latency_and_buckets();
	printf("{\"test\":\"cpuperf\",\"status\":\"passed\"}\n");
	return 0;
}
