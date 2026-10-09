/* SPDX-License-Identifier: MIT */
/* Host-only independent 128-bit oracle and behavioral model tests. */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "model.h"

static uint64_t rng_state = UINT64_C(0x51ec2a1846403137);

static uint64_t random_u64(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static __int128 clamp128(__int128 value, __int128 lower, __int128 upper)
{
	return value < lower ? lower : value > upper ? upper : value;
}

static __s32 reference_raw(const struct sprout_model *model, const __s32 *x)
{
	__int128 dot = 0;
	int i;

	for (i = 0; i < SPROUT_FEATURES; i++)
		dot += clamp128(model->weight_q16[i], -SPROUT_WEIGHT_LIMIT,
				SPROUT_WEIGHT_LIMIT) * clamp128(x[i], 0, SPROUT_Q);
	return (__s32)(dot / SPROUT_Q);
}

static __s32 reference_train(struct sprout_model *model, const __s32 *x,
			   __s32 target)
{
	__int128 error = clamp128(clamp128(target, 0, SPROUT_Q) -
				 reference_raw(model, x), -SPROUT_Q, SPROUT_Q);
	int i;

	for (i = 0; i < SPROUT_FEATURES; i++) {
		__int128 delta = (__int128)SPROUT_ALPHA_Q16 * error *
				clamp128(x[i], 0, SPROUT_Q) /
				((__int128)SPROUT_Q * SPROUT_Q);

		model->weight_q16[i] = (__s32)clamp128(
			clamp128(model->weight_q16[i], -SPROUT_WEIGHT_LIMIT,
				 SPROUT_WEIGHT_LIMIT) + delta,
			-SPROUT_WEIGHT_LIMIT, SPROUT_WEIGHT_LIMIT);
	}
	if (model->updates < UINT64_MAX)
		model->updates++;
	return (__s32)error;
}

static void assert_models_equal(const struct sprout_model *a,
				const struct sprout_model *b)
{
	int i;

	assert(a->updates == b->updates);
	for (i = 0; i < SPROUT_FEATURES; i++)
		assert(a->weight_q16[i] == b->weight_q16[i]);
}

static void test_boundary_oracle(void)
{
	const __s32 values[] = {INT32_MIN, -SPROUT_WEIGHT_LIMIT, -SPROUT_Q,
		-1, 0, 1, SPROUT_Q / 2, SPROUT_Q, SPROUT_WEIGHT_LIMIT,
		INT32_MAX};
	unsigned int trial;

	for (trial = 0; trial < 100000; trial++) {
		struct sprout_model actual = {0}, expected;
		__s32 x[SPROUT_FEATURES], target;
		int i;

		for (i = 0; i < SPROUT_FEATURES; i++) {
			actual.weight_q16[i] = values[random_u64() % 10];
			x[i] = values[random_u64() % 10];
		}
		actual.updates = trial % 2 ? UINT64_MAX : trial;
		expected = actual;
		target = values[random_u64() % 10];
		assert(sprout_model_raw_q16(&actual, x) == reference_raw(&expected, x));
		assert(sprout_model_predict_q16(&actual, x) ==
		       clamp128(reference_raw(&expected, x), 0, SPROUT_Q));
		assert(sprout_model_train(&actual, x, target) ==
		       reference_train(&expected, x, target));
		assert_models_equal(&actual, &expected);
	}
}

static void test_scale_full_u64(void)
{
	const uint64_t values[] = {0, 1, 2, 65535, 65536, 20000000,
		UINT64_MAX / SPROUT_Q, UINT64_MAX / SPROUT_Q + 1,
		UINT64_MAX / 2, UINT64_MAX - 1, UINT64_MAX};
	unsigned int i, j;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		for (j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
			uint64_t ns = values[i], cap = values[j];
			__s32 expected = cap == 0 ? 0 : ns >= cap ? SPROUT_Q :
				(__s32)(((__uint128_t)ns * SPROUT_Q) / cap);

			assert(sprout_scale_ns_q16(ns, cap) == expected);
		}
	}
	for (i = 0; i < 100000; i++) {
		uint64_t ns = random_u64(), cap = random_u64();
		__s32 expected = cap == 0 ? 0 : ns >= cap ? SPROUT_Q :
			(__s32)(((__uint128_t)ns * SPROUT_Q) / cap);

		assert(sprout_scale_ns_q16(ns, cap) == expected);
	}
}

static void test_learning_and_saturation(void)
{
	struct sprout_model model;
	__s32 x[SPROUT_FEATURES] = {SPROUT_Q, 0, 0, 0, 0};
	int i;

	sprout_model_init(&model);
	assert(sprout_model_predict_q16(&model, x) == SPROUT_Q / 4);
	assert(sprout_model_train(&model, x, 3 * SPROUT_Q / 4) == SPROUT_Q / 2);
	assert(model.weight_q16[0] == SPROUT_Q / 4 + 512);
	for (i = 0; i < 600; i++)
		sprout_model_train(&model, x, 3 * SPROUT_Q / 4);
	assert(3 * SPROUT_Q / 4 - sprout_model_predict_q16(&model, x) < 100);
	for (i = 0; i < 600; i++)
		sprout_model_train(&model, x, SPROUT_Q / 8);
	assert(sprout_model_predict_q16(&model, x) - SPROUT_Q / 8 < 100);
	assert(model.updates == 1201);

	/* A clipped-output residual would be zero here and never recover. */
	for (i = 0; i < SPROUT_FEATURES; i++) {
		model.weight_q16[i] = SPROUT_WEIGHT_LIMIT;
		x[i] = SPROUT_Q;
	}
	assert(sprout_model_raw_q16(&model, x) == 20 * SPROUT_Q);
	assert(sprout_model_train(&model, x, SPROUT_Q) == -SPROUT_Q);
	for (i = 0; i < SPROUT_FEATURES; i++)
		assert(model.weight_q16[i] == SPROUT_WEIGHT_LIMIT - SPROUT_ALPHA_Q16);
}

static void test_negative_rounding_and_zero_updates(void)
{
	const int64_t cases[] = {INT64_MIN, INT64_MIN + 1, -4294967297LL,
		-4294967296LL, -4294967295LL, -65537, -65536, -65535, -1,
		0, 1, 65535, 65536, 65537, INT64_MAX};
	struct sprout_model model;
	__s32 x[SPROUT_FEATURES] = {SPROUT_Q / 2, 0, 0, 0, 0};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		assert(sprout_trunc_shift(cases[i], 16) ==
		       (__int128)cases[i] / 65536);
		assert(sprout_trunc_shift(cases[i], 32) ==
		       (__int128)cases[i] / 4294967296LL);
	}
	sprout_model_init(&model);
	model.weight_q16[0] = -1;
	assert(sprout_model_raw_q16(&model, x) == 0);
	model.weight_q16[0] = -3;
	assert(sprout_model_raw_q16(&model, x) == -1);

	/* A small negative residual gives a zero update, not an arithmetic
	 * right-shift's -1. Zero-valued features cannot change coefficients.
	 */
	x[0] = SPROUT_Q;
	model.weight_q16[0] = 1;
	assert(sprout_model_train(&model, x, 0) == -1);
	assert(model.weight_q16[0] == 1);
	for (i = 1; i < SPROUT_FEATURES; i++)
		assert(model.weight_q16[i] == 0);
	x[0] = 0;
	assert(sprout_model_train(&model, x, SPROUT_Q) == SPROUT_Q);
	assert(model.weight_q16[0] == 1);
}

static __s32 known_target(const __s32 *x)
{
	/* A causal signal with mixed positive and negative coefficients. */
	return (__s32)((__s64)SPROUT_Q / 10 +
		((__s64)35 * x[1] + 20 * x[2] + 10 * x[3] - 5 * x[4]) / 100);
}

static void random_features(__s32 *x)
{
	int i;

	x[0] = SPROUT_Q;
	for (i = 1; i < SPROUT_FEATURES; i++)
		x[i] = (__s32)(random_u64() % (SPROUT_Q + 1));
}

static void test_holdout_and_causal_snapshot(void)
{
	struct sprout_model model;
	__s32 x[SPROUT_FEATURES], snapshot[SPROUT_FEATURES];
	uint64_t model_squared_error = 0, fixed_squared_error = 0;
	int i;

	sprout_model_init(&model);
	for (i = 0; i < 20000; i++) {
		random_features(x);
		memcpy(snapshot, x, sizeof(snapshot));
		sprout_model_train(&model, snapshot, known_target(snapshot));
		assert(memcmp(x, snapshot, sizeof(snapshot)) == 0);
	}
	/* Held-out inputs never update the model. */
	for (i = 0; i < 1000; i++) {
		__s64 error, fixed_error;

		random_features(x);
		error = (__s64)sprout_model_predict_q16(&model, x) - known_target(x);
		fixed_error = (__s64)SPROUT_Q / 4 - known_target(x);
		model_squared_error += (uint64_t)(error * error);
		fixed_squared_error += (uint64_t)(fixed_error * fixed_error);
	}
	assert(model.updates == 20000);
	assert(model_squared_error < fixed_squared_error / 100);
	printf("{\"test\":\"synthetic_holdout\",\"samples\":1000,"
	       "\"model_squared_error\":%llu,\"fixed_squared_error\":%llu}\n",
	       (unsigned long long)model_squared_error,
	       (unsigned long long)fixed_squared_error);
}

int main(void)
{
	_Static_assert(sizeof(__s32) == 4 && sizeof(__s64) == 8, "integer widths");
	_Static_assert((-1LL >> 1) == -1LL, "arithmetic signed right shifts required");
	test_boundary_oracle();
	test_scale_full_u64();
	test_learning_and_saturation();
	test_negative_rounding_and_zero_updates();
	test_holdout_and_causal_snapshot();
	puts("{\"test\":\"model\",\"status\":\"passed\",\"oracle_trials\":200121}");
	return 0;
}
