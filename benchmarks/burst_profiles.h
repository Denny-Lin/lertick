/* SPDX-License-Identifier: MIT */
#ifndef LERTICK_BURST_PROFILES_H
#define LERTICK_BURST_PROFILES_H

#include <stdint.h>

enum lertick_burst_profile {
	LERTICK_BURST_FIXED,
	LERTICK_BURST_STEP,
	LERTICK_BURST_ALTERNATING,
	LERTICK_BURST_RAMP,
	LERTICK_BURST_RANDOM_STEP,
};

/* Workload targets only: these values are never supplied to the scheduler.
 * Random groups depend on seed/worker/job, not on policy-dependent compute
 * checksums. All products are bounded or deliberately unsigned modulo 2^64.
 */
static inline uint64_t lertick_burst_target_ns(enum lertick_burst_profile profile,
		uint64_t fixed_ns, uint64_t seed, unsigned worker,
		uint64_t iteration, uint64_t elapsed_ns)
{
	uint64_t phase, value;

	switch (profile) {
	case LERTICK_BURST_STEP:
		return ((elapsed_ns / UINT64_C(7000000000) + worker) & 1) ? 2500000 : 75000;
	case LERTICK_BURST_ALTERNATING:
		return ((iteration + worker) & 1) ? 1750000 : 75000;
	case LERTICK_BURST_RAMP:
		phase = (elapsed_ns % UINT64_C(4000000000) +
			 (uint64_t)worker * UINT64_C(490000000)) % UINT64_C(4000000000);
		return 75000 + phase * UINT64_C(2925000) / UINT64_C(4000000000);
	case LERTICK_BURST_RANDOM_STEP:
		value = seed ^ ((iteration / 17 + 1) * UINT64_C(0x9e3779b97f4a7c15) + worker);
		value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
		value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
		value ^= value >> 31;
		return 75000 + value % UINT64_C(2425001);
	default:
		return fixed_ns;
	}
}

#endif
