/* SPDX-License-Identifier: MIT */
/* Calibrated CPU work plus real sleeping, for deployment evidence and small
 * policy comparisons. JSONL samples measure CPU service, wake lateness and
 * wake-to-completion latency separately. Run CPU controls as separate processes.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#define NS_PER_SEC UINT64_C(1000000000)
#define SCHED_EXT_POLICY 7

static volatile uint64_t observed_work;

static void fail(const char *operation)
{
	perror(operation);
	exit(EXIT_FAILURE);
}

static uint64_t read_ns(clockid_t clock)
{
	struct timespec time;

	if (clock_gettime(clock, &time))
		fail("clock_gettime");
	return (uint64_t)time.tv_sec * NS_PER_SEC + (uint64_t)time.tv_nsec;
}

static uint64_t tid(void)
{
#ifdef __linux__
	return (uint64_t)syscall(SYS_gettid);
#else
	return (uint64_t)(uintptr_t)pthread_self();
#endif
}

static uint64_t work_batch(uint64_t state, uint64_t count)
{
	uint64_t i;

	for (i = 0; i < count; i++) {
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
	}
	observed_work = state;
	return state;
}

static uint64_t calibrate_batch(uint64_t *state, uint64_t *units,
				uint64_t *cpu_ns)
{
	uint64_t start = read_ns(CLOCK_THREAD_CPUTIME_ID), end, count = 0, batch;

	do {
		*state = work_batch(*state, 1024);
		count += 1024;
		end = read_ns(CLOCK_THREAD_CPUTIME_ID);
	} while (end - start < UINT64_C(2000000));
	*units = count;
	*cpu_ns = end - start;
	/* Aim for a CPU-clock check every 25us. Bounds keep work finite even
	 * under emulation or coarse clocks. Calibration is measured, not assumed.
	 */
	batch = count * UINT64_C(25000) / *cpu_ns;
	if (batch < 32)
		batch = 32;
	if (batch > 65536)
		batch = 65536;
	return batch;
}

static uint64_t burn_cpu(uint64_t target_ns, uint64_t batch,
			uint64_t *state, uint64_t *measured_ns)
{
	uint64_t start = read_ns(CLOCK_THREAD_CPUTIME_ID), end, units = 0;

	do {
		*state = work_batch(*state, batch);
		units += batch;
		end = read_ns(CLOCK_THREAD_CPUTIME_ID);
	} while (end - start < target_ns);
	*measured_ns = end - start;
	return units;
}

static void sleep_until(uint64_t deadline)
{
#ifdef __linux__
	struct timespec time = {
		.tv_sec = (time_t)(deadline / NS_PER_SEC),
		.tv_nsec = (long)(deadline % NS_PER_SEC),
	};
	int result;

	do {
		result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL);
	} while (result == EINTR);
	if (result) {
		errno = result;
		fail("clock_nanosleep");
	}
#else
	uint64_t now;

	while ((now = read_ns(CLOCK_MONOTONIC)) < deadline) {
		uint64_t remaining = deadline - now;
		struct timespec time = {
			.tv_sec = (time_t)(remaining / NS_PER_SEC),
			.tv_nsec = (long)(remaining % NS_PER_SEC),
		};

		if (nanosleep(&time, NULL) && errno != EINTR)
			fail("nanosleep");
	}
#endif
}

static uint64_t parse_us(const char *value, int allow_zero)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || *value == '-' || !*value || *end ||
	    (!allow_zero && !parsed) || parsed > 1000000) {
		fprintf(stderr, "invalid microseconds: %s (maximum 1000000)\n", value);
		exit(EXIT_FAILURE);
	}
	return (uint64_t)parsed * 1000;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [--duration SEC] [--burst-us US] [--sleep-us US]\n"
		"       [--phase-burst-us US] [--role burst|cpu] [--cpu CPU]\n"
		"       [--policy-ext]\n"
		"Default: duration 3, burst 2000us, sleep 8000us, role burst.\n"
		"phase-burst-us switches the CPU target halfway through wall time.\n"
		"role cpu stays runnable and emits only calibration/final records.\n"
		"policy-ext explicitly requests Linux SCHED_EXT (policy 7).\n"
		"JSONL stdout; diagnostics on stderr. No performance claims.\n", program);
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{"duration", required_argument, NULL, 'd'},
		{"burst-us", required_argument, NULL, 'b'},
		{"sleep-us", required_argument, NULL, 's'},
		{"phase-burst-us", required_argument, NULL, 'p'},
		{"role", required_argument, NULL, 'r'},
		{"cpu", required_argument, NULL, 'c'},
		{"policy-ext", no_argument, NULL, 'e'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	uint64_t burst_ns = 2000000, sleep_ns = 8000000, phase_ns = 0;
	uint64_t state = UINT64_C(0x91b8c542410d87bd), batch, calibration_units;
	uint64_t calibration_ns, start, end, deadline, expected_wake;
	uint64_t total_units = 0, total_cpu = 0, samples = 0, process_tid;
	double duration = 3.0;
	const char *role = "burst";
	int option, cpu = -1, policy_ext = 0;

	while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
		char *tail;

		switch (option) {
		case 'd':
			errno = 0;
			duration = strtod(optarg, &tail);
			if (errno || !*optarg || *tail || !isfinite(duration) ||
			    duration < 0.01 || duration > 86400.0) {
				fprintf(stderr, "duration must be in [0.01,86400] seconds\n");
				return EXIT_FAILURE;
			}
			break;
		case 'b': burst_ns = parse_us(optarg, 0); break;
		case 's': sleep_ns = parse_us(optarg, 1); break;
		case 'p': phase_ns = parse_us(optarg, 0); break;
		case 'r': role = optarg; break;
		case 'c': {
			long parsed;

			errno = 0;
			parsed = strtol(optarg, &tail, 10);
			if (errno || !*optarg || *tail || parsed < 0 || parsed > 1023) {
				fprintf(stderr, "CPU must be in [0,1023]\n");
				return EXIT_FAILURE;
			}
			cpu = (int)parsed;
			break;
		}
		case 'e': policy_ext = 1; break;
		case 'h': usage(stdout, argv[0]); return 0;
		default: usage(stderr, argv[0]); return EXIT_FAILURE;
		}
	}
	if (optind != argc || (strcmp(role, "burst") && strcmp(role, "cpu"))) {
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}
#ifdef __linux__
	if (prctl(PR_SET_NAME, strcmp(role, "cpu") ? "sprout_burst" : "sprout_cpu",
		  0, 0, 0))
		fail("prctl(PR_SET_NAME)");
	if (cpu >= 0) {
		cpu_set_t cpus;

		if (cpu >= CPU_SETSIZE) {
			fprintf(stderr, "CPU exceeds libc CPU_SETSIZE\n");
			return EXIT_FAILURE;
		}
		CPU_ZERO(&cpus);
		CPU_SET(cpu, &cpus);
		if (sched_setaffinity(0, sizeof(cpus), &cpus))
			fail("sched_setaffinity");
	}
	if (policy_ext) {
		struct sched_param parameter = {.sched_priority = 0};

		if (sched_setscheduler(0, SCHED_EXT_POLICY, &parameter))
			fail("sched_setscheduler(SCHED_EXT)");
	}
#else
	if (cpu >= 0 || policy_ext) {
		fprintf(stderr, "--cpu and --policy-ext require Linux\n");
		return EXIT_FAILURE;
	}
#endif
	process_tid = tid();
	setvbuf(stdout, NULL, _IOLBF, 0);
	batch = calibrate_batch(&state, &calibration_units, &calibration_ns);
	printf("{\"event\":\"calibration\",\"pid\":%ld,\"tid\":%" PRIu64
	       ",\"role\":\"%s\",\"units\":%" PRIu64 ",\"cpu_ns\":%" PRIu64
	       ",\"batch_units\":%" PRIu64 ",\"policy_ext\":%s}\n",
	       (long)getpid(), process_tid, role, calibration_units, calibration_ns,
	       batch, policy_ext ? "true" : "false");
	expected_wake = read_ns(CLOCK_MONOTONIC);
	/* Give the scheduler a verified sleep/wakeup boundary after calibration.
	 * The CPU control deliberately has no such boundary until exit.
	 */
	if (strcmp(role, "cpu")) {
		expected_wake += 1000000;
		sleep_until(expected_wake);
	}
	start = read_ns(CLOCK_MONOTONIC);
	deadline = start + (uint64_t)(duration * (double)NS_PER_SEC);
	while (read_ns(CLOCK_MONOTONIC) < deadline) {
		uint64_t cycle_start = read_ns(CLOCK_MONOTONIC), cpu_ns, units;
		uint64_t target = !strcmp(role, "cpu") ? 10000000 :
			phase_ns && cycle_start - start >= (deadline - start) / 2 ?
			phase_ns : burst_ns;

		units = burn_cpu(target, batch, &state, &cpu_ns);
		end = read_ns(CLOCK_MONOTONIC);
		total_units += units;
		total_cpu += cpu_ns;
		samples++;
		if (strcmp(role, "cpu")) {
			uint64_t response_ns = cycle_start > expected_wake ?
				cycle_start - expected_wake : 0;

			printf("{\"event\":\"sample\",\"pid\":%ld,\"tid\":%" PRIu64
			       ",\"role\":\"burst\",\"sequence\":%" PRIu64
			       ",\"target_cpu_ns\":%" PRIu64 ",\"cpu_ns\":%" PRIu64
			       ",\"wall_ns\":%" PRIu64 ",\"response_ns\":%" PRIu64
			       ",\"completion_ns\":%" PRIu64 ",\"units\":%" PRIu64 "}\n",
			       (long)getpid(), process_tid, samples, target, cpu_ns,
			       end - cycle_start, response_ns, end - expected_wake, units);
			expected_wake = read_ns(CLOCK_MONOTONIC) + sleep_ns;
			if (expected_wake >= deadline)
				break;
			if (sleep_ns)
				sleep_until(expected_wake);
		}
	}
	end = read_ns(CLOCK_MONOTONIC);
	printf("{\"event\":\"summary\",\"pid\":%ld,\"tid\":%" PRIu64
	       ",\"role\":\"%s\",\"samples\":%" PRIu64 ",\"units\":%" PRIu64
	       ",\"cpu_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64
	       ",\"throughput_units_per_s\":%.3f,\"checksum\":%" PRIu64 "}\n",
	       (long)getpid(), process_tid, role, samples, total_units, total_cpu,
	       end - start, (double)total_units * (double)NS_PER_SEC / (double)(end - start),
	       observed_work);
	return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}
