/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "burst_profiles.h"

int main(void)
{
	assert(lertick_burst_target_ns(LERTICK_BURST_FIXED, 123, 0, 0, 0, 0) == 123);
	assert(lertick_burst_target_ns(LERTICK_BURST_STEP, 0, 0, 0, 0, 0) == 75000);
	assert(lertick_burst_target_ns(LERTICK_BURST_STEP, 0, 0, 0, 0, 7000000000ull) == 2500000);
	assert(lertick_burst_target_ns(LERTICK_BURST_STEP, 0, 0, 0, 0, 14000000000ull) == 75000);
	assert(lertick_burst_target_ns(LERTICK_BURST_STEP, 0, 0, 1, 0, 0) == 2500000);
	assert(lertick_burst_target_ns(LERTICK_BURST_ALTERNATING, 0, 0, 0, 0, 0) == 75000);
	assert(lertick_burst_target_ns(LERTICK_BURST_ALTERNATING, 0, 0, 0, 1, 0) == 1750000);
	assert(lertick_burst_target_ns(LERTICK_BURST_RAMP, 0, 0, 0, 0, 0) == 75000);
	assert(lertick_burst_target_ns(LERTICK_BURST_RAMP, 0, 0, 0, 0, 3999999999ull) == 2999999);
	assert(lertick_burst_target_ns(LERTICK_BURST_RAMP, 0, 0, 0, 0, 4000000000ull) == 75000);
	assert(lertick_burst_target_ns(LERTICK_BURST_RANDOM_STEP, 0, 20261102, 0, 0, 0) == 1663493);
	assert(lertick_burst_target_ns(LERTICK_BURST_RANDOM_STEP, 0, 20261102, 0, 16, 0) == 1663493);
	assert(lertick_burst_target_ns(LERTICK_BURST_RANDOM_STEP, 0, 20261102, 0, 17, 0) == 636163);
	assert(lertick_burst_target_ns(LERTICK_BURST_RANDOM_STEP, 0, 20261102, 3, 0, 0) == 2186278);
	assert(lertick_burst_target_ns(LERTICK_BURST_RANDOM_STEP, 0, UINT64_MAX, 31,
				      UINT64_MAX, UINT64_MAX) == 1389320);
	for (int profile = LERTICK_BURST_STEP; profile <= LERTICK_BURST_RANDOM_STEP; profile++)
		for (unsigned worker = 0; worker < 32; worker++)
			for (uint64_t i = 0; i < 100; i++) {
				uint64_t target = lertick_burst_target_ns(profile, 0, UINT64_MAX - i,
					worker, i * (UINT64_MAX / 100), UINT64_MAX - i);
				assert(target >= 75000 && target <= 3000000);
			}
	puts("burst profiles: phase boundaries, deterministic seeds, groups and u64 bounds passed");
	return 0;
}
