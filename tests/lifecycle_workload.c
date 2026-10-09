/* SPDX-License-Identifier: MIT */
/* Linux-only lifecycle and runnable-service workload. Emits measured JSONL;
 * it contains no scheduler-specific accounting oracle or performance claim.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define NS_SECOND UINT64_C(1000000000)
#define MAX_WORKERS 32

struct options {
	const char *scenario;
	const char *cpu_text;
	const char *self;
	cpu_set_t allowed;
	unsigned cpus[CPU_SETSIZE];
	unsigned ncpus;
	unsigned workers;
	unsigned iterations;
	unsigned index;
	double duration;
};

struct measurement {
	uint64_t tid, pid, cpu_ns, schedstat_ns, schedstat_wait_ns;
	uint64_t wall_ns, max_run_gap_ns, units, checksum;
	uint64_t loops, yields, sleeps, migrations, affinity_changes;
	uint64_t affinity_violations;
	unsigned index;
	const char *role;
};

struct thread_arg {
	struct options *options;
	pthread_barrier_t *barrier;
	struct measurement result;
	unsigned index;
};

static void fail(const char *operation)
{
	perror(operation);
	exit(EXIT_FAILURE);
}

static uint64_t now_ns(clockid_t clock)
{
	struct timespec value;

	if (clock_gettime(clock, &value))
		fail("clock_gettime");
	return (uint64_t)value.tv_sec * NS_SECOND + (uint64_t)value.tv_nsec;
}

static void sleep_ns(uint64_t amount)
{
	struct timespec delay = {
		.tv_sec = (time_t)(amount / NS_SECOND),
		.tv_nsec = (long)(amount % NS_SECOND),
	};

	while (nanosleep(&delay, &delay) && errno == EINTR)
		;
}

static void schedstat(int fd, uint64_t *runtime, uint64_t *wait)
{
	char text[128];
	ssize_t count = pread(fd, text, sizeof(text) - 1, 0);

	if (count <= 0)
		fail("read /proc/thread-self/schedstat");
	text[count] = '\0';
	if (sscanf(text, "%" SCNu64 " %" SCNu64, runtime, wait) != 2) {
		fprintf(stderr, "invalid schedstat record\n");
		exit(EXIT_FAILURE);
	}
}

static void set_affinity(const cpu_set_t *cpus)
{
	if (sched_setaffinity(0, sizeof(*cpus), cpus))
		fail("sched_setaffinity");
}

static struct measurement run_worker(struct options *options, const char *role,
				     unsigned index, pthread_barrier_t *barrier)
{
	struct measurement result = {.role = role, .index = index};
	uint64_t sched_start, wait_start, sched_end, wait_end, cpu_start;
	uint64_t start, deadline, last, next_affinity, state = UINT64_C(0xa61449bd);
	uint64_t burst_start = 0;
	int previous_cpu = -1, fd;
	unsigned selected = 0;
	char name[16];
	cpu_set_t selected_mask = options->allowed;

	set_affinity(&options->allowed);
	fd = open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		fail("open schedstat");
	snprintf(name, sizeof(name), "lertick_%.7s", role);
	if (prctl(PR_SET_NAME, name, 0, 0, 0))
		fail("prctl");
	result.tid = (uint64_t)syscall(SYS_gettid);
	result.pid = (uint64_t)getpid();
	if (barrier) {
		int error = pthread_barrier_wait(barrier);

		if (error && error != PTHREAD_BARRIER_SERIAL_THREAD) {
			errno = error;
			fail("pthread_barrier_wait");
		}
	}
	/* The name is set while this process may already be running, before BPF
	 * has observed the opt-in. A setup sleep establishes a verified tracked
	 * wake before both independent oracles start. CPU/yield then remain
	 * continuously runnable; their wake-to-exit episode must be censored.
	 * lateoptin intentionally omits that setup boundary to exercise opt-in
	 * recognition during continuation/stopping callbacks.
	 */
	if (strcmp(role, "lateoptin"))
		sleep_ns(1000000);
	schedstat(fd, &sched_start, &wait_start);
	cpu_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
	start = now_ns(CLOCK_MONOTONIC);
	deadline = start + (uint64_t)(options->duration * NS_SECOND);
	last = start;
	next_affinity = start;
	burst_start = cpu_start;
	while (last < deadline) {
		uint64_t current;
		int cpu;

		for (unsigned unit = 0; unit < 512; unit++) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
		}
		result.units += 512;
		result.loops++;
		current = now_ns(CLOCK_MONOTONIC);
		if (current - last > result.max_run_gap_ns)
			result.max_run_gap_ns = current - last;
		last = current;
		if (!strcmp(role, "affinity") && current >= next_affinity) {
			CPU_ZERO(&selected_mask);
			CPU_SET(options->cpus[selected], &selected_mask);
			set_affinity(&selected_mask);
			selected = (selected + 1) % options->ncpus;
			next_affinity = current + 20000000;
			result.affinity_changes++;
		}
		cpu = sched_getcpu();
		if (cpu < 0)
			fail("sched_getcpu");
		if (!CPU_ISSET(cpu, &selected_mask))
			result.affinity_violations++;
		if (previous_cpu >= 0 && cpu != previous_cpu)
			result.migrations++;
		previous_cpu = cpu;
		if (!strcmp(role, "yield") && result.loops % 32 == 0) {
			if (sched_yield())
				fail("sched_yield");
			result.yields++;
		} else if (!strcmp(role, "burst") &&
			   now_ns(CLOCK_THREAD_CPUTIME_ID) - burst_start >= 1000000) {
			sleep_ns(2000000);
			result.sleeps++;
			burst_start = now_ns(CLOCK_THREAD_CPUTIME_ID);
			/* Voluntary sleep is excluded from the runnable-gap metric. */
			last = now_ns(CLOCK_MONOTONIC);
		}
	}
	result.cpu_ns = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
	result.wall_ns = now_ns(CLOCK_MONOTONIC) - start;
	schedstat(fd, &sched_end, &wait_end);
	result.schedstat_ns = sched_end - sched_start;
	result.schedstat_wait_ns = wait_end - wait_start;
	result.checksum = state;
	if (close(fd))
		fail("close schedstat");
	return result;
}

static void emit(const struct measurement *value)
{
	/* Each child emits one record; one write prevents forked records interleaving. */
	char line[1024];
	int size = snprintf(line, sizeof(line),
		"{\"event\":\"worker\",\"role\":\"%s\",\"pid\":%" PRIu64
		",\"tid\":%" PRIu64 ",\"index\":%u,\"cpu_ns\":%" PRIu64
		",\"schedstat_ns\":%" PRIu64 ",\"schedstat_wait_ns\":%" PRIu64
		",\"wall_ns\":%" PRIu64 ",\"max_run_gap_ns\":%" PRIu64
		",\"units\":%" PRIu64 ",\"checksum\":%" PRIu64
		",\"loops\":%" PRIu64 ",\"yields\":%" PRIu64
		",\"sleeps\":%" PRIu64 ",\"setup_sleep_ns\":%u,\"migrations\":%" PRIu64
		",\"affinity_changes\":%" PRIu64 ",\"affinity_violations\":%" PRIu64 "}\n",
		value->role, value->pid, value->tid, value->index, value->cpu_ns,
		value->schedstat_ns, value->schedstat_wait_ns, value->wall_ns,
		value->max_run_gap_ns, value->units, value->checksum, value->loops,
		value->yields, value->sleeps, !strcmp(value->role, "lateoptin") ? 0U : 1000000U,
		value->migrations, value->affinity_changes,
		value->affinity_violations);

	if (size < 0 || (size_t)size >= sizeof(line) || write(STDOUT_FILENO, line, size) != size)
		fail("write worker record");
}

static void *thread_worker(void *opaque)
{
	struct thread_arg *argument = opaque;
	const char *role = argument->index % 2 ? "burst" : "cpu";

	argument->result = run_worker(argument->options, role, argument->index,
				      argument->barrier);
	return NULL;
}

static void threaded(struct options *options)
{
	pthread_t threads[MAX_WORKERS];
	struct thread_arg arguments[MAX_WORKERS];
	pthread_barrier_t barrier;
	int error = pthread_barrier_init(&barrier, NULL, options->workers + 1);

	if (error) {
		errno = error;
		fail("pthread_barrier_init");
	}
	for (unsigned index = 0; index < options->workers; index++) {
		arguments[index] = (struct thread_arg){.options = options,
			.barrier = &barrier, .index = index};
		error = pthread_create(&threads[index], NULL, thread_worker, &arguments[index]);
		if (error) {
			errno = error;
			fail("pthread_create");
		}
	}
	error = pthread_barrier_wait(&barrier);
	if (error && error != PTHREAD_BARRIER_SERIAL_THREAD) {
		errno = error;
		fail("parent barrier");
	}
	for (unsigned index = 0; index < options->workers; index++) {
		error = pthread_join(threads[index], NULL);
		if (error) {
			errno = error;
			fail("pthread_join");
		}
		emit(&arguments[index].result);
	}
	error = pthread_barrier_destroy(&barrier);
	if (error) {
		errno = error;
		fail("pthread_barrier_destroy");
	}
}

static void fork_exec(struct options *options)
{
	unsigned completed = 0;

	/* Children inherit an opted-in name before exec, so even very short-lived
	 * tasks are represented in the scheduler's lifecycle snapshots.
	 */
	if (prctl(PR_SET_NAME, "lertick_parent", 0, 0, 0))
		fail("prctl parent");
	for (unsigned base = 0; base < options->iterations; base += options->workers) {
		pid_t children[MAX_WORKERS];
		unsigned count = options->iterations - base;

		if (count > options->workers)
			count = options->workers;
		for (unsigned index = 0; index < count; index++) {
			children[index] = fork();
			if (children[index] < 0)
				fail("fork");
			if (!children[index]) {
				char duration[32], identifier[32];

				snprintf(duration, sizeof(duration), "%.6f", options->duration);
				snprintf(identifier, sizeof(identifier), "%u", base + index);
				execl(options->self, options->self, "--scenario", "cpu",
				      "--duration", duration, "--cpus", options->cpu_text,
				      "--index", identifier, (char *)NULL);
				fail("exec workload");
			}
		}
		for (unsigned index = 0; index < count; index++) {
			int status;

			if (waitpid(children[index], &status, 0) != children[index])
				fail("waitpid");
			if (!WIFEXITED(status) || WEXITSTATUS(status)) {
				fprintf(stderr, "child failed: pid=%ld status=%d\n",
					(long)children[index], status);
				exit(EXIT_FAILURE);
			}
			completed++;
		}
	}
	dprintf(STDOUT_FILENO,
		"{\"event\":\"lifecycle\",\"created\":%u,\"reaped\":%u,\"exec\":true}\n",
		options->iterations, completed);
}

static unsigned integer(const char *text, unsigned minimum, unsigned maximum)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || !*text || *text == '-' || *end || value < minimum || value > maximum) {
		fprintf(stderr, "integer must be in %u..%u: %s\n", minimum, maximum, text);
		exit(EXIT_FAILURE);
	}
	return (unsigned)value;
}

static void parse_cpus(struct options *options)
{
	char *copy = strdup(options->cpu_text), *cursor, *token;
	cpu_set_t inherited;

	if (!copy)
		fail("strdup");
	if (sched_getaffinity(0, sizeof(inherited), &inherited))
		fail("sched_getaffinity");
	CPU_ZERO(&options->allowed);
	cursor = copy;
	while ((token = strsep(&cursor, ","))) {
		unsigned cpu = integer(token, 0, CPU_SETSIZE - 1);

		if (!CPU_ISSET(cpu, &inherited)) {
			fprintf(stderr, "CPU %u is not in inherited affinity\n", cpu);
			exit(EXIT_FAILURE);
		}
		if (!CPU_ISSET(cpu, &options->allowed)) {
			CPU_SET(cpu, &options->allowed);
			options->cpus[options->ncpus++] = cpu;
		}
	}
	free(copy);
}

int main(int argc, char **argv)
{
	static const struct option arguments[] = {
		{"scenario", required_argument, NULL, 's'},
		{"duration", required_argument, NULL, 'd'},
		{"workers", required_argument, NULL, 'w'},
		{"iterations", required_argument, NULL, 'i'},
		{"index", required_argument, NULL, 'n'},
		{"cpus", required_argument, NULL, 'c'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	struct options options = {.scenario = "cpu", .cpu_text = "0", .self = argv[0],
		.workers = 8, .iterations = 32, .duration = 1.0};
	int option;

	while ((option = getopt_long(argc, argv, "", arguments, NULL)) != -1) {
		char *end;

		switch (option) {
		case 's': options.scenario = optarg; break;
		case 'd':
			errno = 0;
			options.duration = strtod(optarg, &end);
			if (errno || !*optarg || *end || !isfinite(options.duration) ||
			    options.duration < 0.0001 || options.duration > 120) {
				fprintf(stderr, "duration must be 0.0001..120 seconds\n");
				return EXIT_FAILURE;
			}
			break;
		case 'w': options.workers = integer(optarg, 1, MAX_WORKERS); break;
		case 'i': options.iterations = integer(optarg, 1, 8192); break;
		case 'n': options.index = integer(optarg, 0, 8192); break;
		case 'c': options.cpu_text = optarg; break;
		case 'h':
			printf("Usage: %s --scenario cpu|yield|burst|affinity|lateoptin|threads|forkexec|churn\n"
			       "  [--duration SEC] [--workers N] [--iterations N] [--cpus 0,1,2,3]\n"
			       "Linux-only; JSONL CPU-clock/schedstat and lifecycle measurements.\n", argv[0]);
			return 0;
		default: return EXIT_FAILURE;
		}
	}
	if (optind != argc)
		return EXIT_FAILURE;
	parse_cpus(&options);
	if (!strcmp(options.scenario, "threads")) {
		threaded(&options);
	} else if (!strcmp(options.scenario, "forkexec") || !strcmp(options.scenario, "churn")) {
		fork_exec(&options);
	} else if (!strcmp(options.scenario, "cpu") || !strcmp(options.scenario, "yield") ||
		   !strcmp(options.scenario, "burst") || !strcmp(options.scenario, "affinity") ||
		   !strcmp(options.scenario, "lateoptin")) {
		struct measurement result = run_worker(&options, options.scenario, options.index, NULL);

		emit(&result);
	} else {
		fprintf(stderr, "unknown scenario: %s\n", options.scenario);
		return EXIT_FAILURE;
	}
	return 0;
}
