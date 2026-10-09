/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LERTICK_ACCOUNTING_H
#define LERTICK_ACCOUNTING_H

#include "model.h"

struct lertick_service_sample {
	__u64 pending_ns;
	__u64 consumed_ns;
	int valid;
};

/*
 * A read-only view of actual CPU service. Linux may update sum_exec_runtime
 * and call dispatch(prev) while keeping prev running: no stopping() callback
 * commits that segment yet. Include it when choosing a continuing slice, but
 * do not advance the watermark or charge vtime here. stopping() remains the
 * sole owner of committing service. A duplicate stopping notification has
 * running == 0 and therefore cannot charge that segment a second time.
 */
LERTICK_INLINE struct lertick_service_sample
lertick_service_sample(__u64 committed_ns, __u64 runtime_start_ns,
		       __u64 runtime_ns, int running)
{
	struct lertick_service_sample result = {
		.pending_ns = 0,
		.consumed_ns = committed_ns,
		.valid = 1,
	};

	if (!running)
		return result;
	if (runtime_ns < runtime_start_ns) {
		result.valid = 0;
		result.consumed_ns = ~(__u64)0;
		return result;
	}
	result.pending_ns = runtime_ns - runtime_start_ns;
	if (result.pending_ns > ~(__u64)0 - committed_ns) {
		result.valid = 0;
		result.consumed_ns = ~(__u64)0;
		return result;
	}
	result.consumed_ns = committed_ns + result.pending_ns;
	return result;
}

#endif /* LERTICK_ACCOUNTING_H */
