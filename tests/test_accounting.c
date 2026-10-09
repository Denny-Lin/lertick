/* SPDX-License-Identifier: GPL-2.0-only */
/* Independent 128-bit arithmetic oracle and audited callback-order traces.
 * These are portable service-accounting tests, not kernel lifecycle tests.
 * The Linux runner must independently exercise real callbacks and recovery.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "accounting.h"

static uint64_t rng_state = UINT64_C(0xabc6819462ff1237);

static uint64_t random_u64(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static void check_oracle(uint64_t committed, uint64_t start,
			 uint64_t runtime, int running)
{
	struct lertick_service_sample actual =
		lertick_service_sample(committed, start, runtime, running);
	__uint128_t pending = running && runtime >= start ?
		(__uint128_t)runtime - start : 0;
	__uint128_t consumed = (__uint128_t)committed + pending;
	int valid = !running || (runtime >= start && consumed <= UINT64_MAX);

	assert(actual.valid == valid);
	assert(actual.pending_ns == pending);
	assert(actual.consumed_ns == (valid ? (uint64_t)consumed : UINT64_MAX));
}

static void test_boundary_and_random_oracle(void)
{
	const uint64_t values[] = {0, 1, 499999, 500000, 2000000, 5000000,
		10000000, UINT64_MAX / 2, UINT64_MAX - 1, UINT64_MAX};
	unsigned int a, b, c, i;

	for (a = 0; a < sizeof(values) / sizeof(values[0]); a++)
		for (b = 0; b < sizeof(values) / sizeof(values[0]); b++)
			for (c = 0; c < sizeof(values) / sizeof(values[0]); c++) {
				check_oracle(values[a], values[b], values[c], 0);
				check_oracle(values[a], values[b], values[c], 1);
			}
	for (i = 0; i < 200000; i++)
		check_oracle(random_u64(), random_u64(), random_u64(), i % 2);
}

static void test_continuation_does_not_commit(void)
{
	/* Already committed 1ms; the currently running segment reaches 2ms.
	 * A 4ms prediction must have 1ms left, not the stale view's 3ms.
	 * balance_one() may read the same service more than once without a stop.
	 */
	const uint64_t committed = 1000000, start = 7000000, prediction = 4000000;
	struct lertick_service_sample first, repeated, later, stopped;

	first = lertick_service_sample(committed, start, 9000000, 1);
	repeated = lertick_service_sample(committed, start, 9000000, 1);
	assert(first.valid && first.pending_ns == 2000000);
	assert(first.consumed_ns == 3000000);
	assert(prediction - first.consumed_ns == 1000000);
	assert(repeated.pending_ns == first.pending_ns);
	assert(repeated.consumed_ns == first.consumed_ns);
	later = lertick_service_sample(committed, start, 11500000, 1);
	assert(later.valid && later.pending_ns == 4500000);
	assert(later.consumed_ns == 5500000 && later.consumed_ns > prediction);
	/* stopping commits the whole 4.5ms segment once. A stopped task has
	 * no pending segment even if its old start watermark is still present.
	 */
	stopped = lertick_service_sample(later.consumed_ns, start, 11500000, 0);
	assert(stopped.pending_ns == 0 && stopped.consumed_ns == 5500000);
}

static void test_preemption_migration_and_final_sleep_trace(void)
{
	/* Audited Linux ordering: running -> stopping(runnable), then another
	 * running -> zero or more continuing dispatches -> stopping(false) ->
	 * quiescent(SLEEP). Migration/non-sleep quiescence does not reset the
	 * episode. The independent oracle is final CPU clock - wake CPU clock.
	 */
	const uint64_t wake_clock = 30000000, first_stop_clock = 31700000;
	const uint64_t final_sleep_clock = 35600000;
	uint64_t committed = 0, charge = 0;
	struct lertick_service_sample first, continuing, final, duplicate;

	first = lertick_service_sample(committed, wake_clock, first_stop_clock, 1);
	charge += first.pending_ns;
	committed = first.consumed_ns;
	/* Off-CPU wall time does not advance sum_exec_runtime; resumption
	 * starts at the same CPU clock even on a different CPU.
	 */
	continuing = lertick_service_sample(committed, first_stop_clock, 34000000, 1);
	assert(continuing.consumed_ns == 4000000);
	assert(charge == 1700000 && committed == 1700000);
	final = lertick_service_sample(committed, first_stop_clock, final_sleep_clock, 1);
	charge += final.pending_ns;
	committed = final.consumed_ns;
	assert(committed == final_sleep_clock - wake_clock);
	assert(charge == final_sleep_clock - wake_clock);
	/* dequeue_task_scx() already stopped it before quiescent; there is no
	 * additional final segment to invent or charge after that callback.
	 */
	duplicate = lertick_service_sample(committed, first_stop_clock, final_sleep_clock, 0);
	assert(duplicate.pending_ns == 0 && duplicate.consumed_ns == committed);
}

static void test_attach_exit_and_new_episode_boundaries(void)
{
	/* Attaching to an existing task must not include its lifetime service.
	 * Attach/exit episodes are censored by the BPF lifecycle callbacks; this
	 * arithmetic test checks service ownership across their reset boundary.
	 */
	const uint64_t attach_clock = 900000000000ULL;
	struct lertick_service_sample partial, next, nonrunning;

	partial = lertick_service_sample(0, attach_clock, attach_clock + 1300000, 1);
	assert(partial.pending_ns == 1300000 && partial.consumed_ns == 1300000);
	/* Verified subsequent wake begins an episode with zero committed CPU
	 * service and captures the actual current CPU clock at running().
	 */
	next = lertick_service_sample(0, attach_clock + 1300000,
				      attach_clock + 4000000, 1);
	assert(next.pending_ns == 2700000 && next.consumed_ns == 2700000);
	/* A non-running task has no charge during disable/exit or late opt-in.
	 * Its stale clock cannot create an extra pending execution segment.
	 */
	nonrunning = lertick_service_sample(2700000, attach_clock,
					      attach_clock + 4000000, 0);
	assert(nonrunning.pending_ns == 0 && nonrunning.consumed_ns == 2700000);
}

int main(void)
{
	test_boundary_and_random_oracle();
	test_continuation_does_not_commit();
	test_preemption_migration_and_final_sleep_trace();
	test_attach_exit_and_new_episode_boundaries();
	puts("accounting: 202000 oracle cases and callback-order traces passed");
	return 0;
}
