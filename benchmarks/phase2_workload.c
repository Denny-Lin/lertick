/* SPDX-License-Identifier: MIT */
/* Buffered, synchronized CPU scheduling workloads. No I/O in measured loops. */
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
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#define NS_SECOND UINT64_C(1000000000)
#define MAX_WORKERS 32
#define MAX_CPUS 128
#define MAX_SAMPLES 100000

enum role { ROLE_CPU, ROLE_BURST, ROLE_MEMORY };

struct sample {
	uint64_t end_ns, cpu_ns, wall_ns, wake_lateness_ns, completion_ns;
	uint64_t units, progress_gap_ns;
	int cpu;
};

struct worker {
	pthread_t thread;
	unsigned index;
	enum role role;
	uint64_t burst_ns, sleep_ns, batch, tid;
	volatile uint64_t state; /* Observable checksum keeps calibrated work real. */
	struct sample *samples;
	unsigned sample_count, dropped_samples;
	uint64_t units, cpu_service_ns, max_gap_ns;
	unsigned migrations, affinity_changes;
	unsigned initial_allowed_count;
	int initial_allowed_cpus[MAX_CPUS];
	long voluntary_switches, involuntary_switches, minor_faults, major_faults;
	uint64_t process_cpu_ns;
	void *memory;
	size_t memory_bytes, memory_offset;
};

static pthread_mutex_t ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_condition = PTHREAD_COND_INITIALIZER;
static int workers_ready, start_ready;
static struct worker workers[MAX_WORKERS];
static uint64_t start_ns, measured_start_ns, end_ns;
static int cpus[MAX_CPUS], cpu_count, worker_count, migrating, spread;
static size_t memory_bytes = 1024ULL * 1024 * 1024;

static void fail(const char *operation)
{
	perror(operation);
	exit(EXIT_FAILURE);
}

static uint64_t clock_ns(clockid_t clock)
{
	struct timespec ts;
	if (clock_gettime(clock, &ts))
		fail("clock_gettime");
	return (uint64_t)ts.tv_sec * NS_SECOND + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t deadline)
{
	struct timespec ts = { .tv_sec = deadline / NS_SECOND,
		.tv_nsec = deadline % NS_SECOND };
#ifdef __linux__
	int err;
	do { err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL); }
	while (err == EINTR);
	if (err) { errno = err; fail("clock_nanosleep"); }
#else
	uint64_t now;
	while ((now = clock_ns(CLOCK_MONOTONIC)) < deadline) {
		ts.tv_sec = (deadline - now) / NS_SECOND;
		ts.tv_nsec = (deadline - now) % NS_SECOND;
		if (nanosleep(&ts, NULL) && errno != EINTR)
			fail("nanosleep");
	}
#endif
}

static void set_affinity(unsigned index, int one_cpu)
{
#ifdef __linux__
	cpu_set_t mask;
	CPU_ZERO(&mask);
	if (one_cpu)
		CPU_SET(cpus[index % (unsigned)cpu_count], &mask);
	else
		for (int i = 0; i < cpu_count; i++)
			CPU_SET(cpus[i], &mask);
	if (sched_setaffinity(0, sizeof(mask), &mask))
		fail("sched_setaffinity");
#else
	(void)index; (void)one_cpu;
#endif
}

static uint64_t compute(struct worker *worker, uint64_t count)
{
	uint64_t state = worker->state;
	if (worker->role == ROLE_MEMORY) {
		/* Revisit a large anonymous working set. The runner records memory PSI;
		 * this does not assume that the kernel actually enters reclaim. */
		volatile uint64_t *memory = worker->memory;
		size_t words = worker->memory_bytes / sizeof(*memory);
		for (uint64_t i = 0; i < count; i++) {
			worker->memory_offset = (worker->memory_offset + 512) % words;
			state ^= memory[worker->memory_offset] + UINT64_C(0x9e3779b97f4a7c15);
			memory[worker->memory_offset] = state;
		}
	} else {
		for (uint64_t i = 0; i < count; i++) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
		}
	}
	worker->state = state;
	return count;
}

static void calibrate(struct worker *worker)
{
	uint64_t start = clock_ns(CLOCK_THREAD_CPUTIME_ID), elapsed, units = 0;
	do {
		units += compute(worker, 1024);
		elapsed = clock_ns(CLOCK_THREAD_CPUTIME_ID) - start;
	} while (elapsed < 2000000);
	worker->batch = units * 25000 / elapsed;
	if (worker->batch < 16) worker->batch = 16;
	if (worker->batch > 65536) worker->batch = 65536;
}

static uint64_t burn(struct worker *worker, uint64_t *cpu_ns)
{
	uint64_t begin = clock_ns(CLOCK_THREAD_CPUTIME_ID), elapsed, units = 0;
	do {
		units += compute(worker, worker->batch);
		elapsed = clock_ns(CLOCK_THREAD_CPUTIME_ID) - begin;
	} while (elapsed < worker->burst_ns);
	*cpu_ns = elapsed;
	return units;
}

static void *run_worker(void *argument)
{
	struct worker *worker = argument;
	uint64_t expected, previous_end, units, service, beginning, finished;
	uint64_t measured_cpu_start = 0, measured_cpu_end = 0, iteration = 0;
	uint64_t last_measured_progress = 0;
	int previous_cpu = -1, current_cpu = -1, measured = 0;
#ifdef __linux__
	struct rusage usage_start = {0}, usage_end = {0};
	char comm[16];
	snprintf(comm, sizeof(comm), "lertick_%c%02u", worker->role == ROLE_BURST ? 'b' :
		 worker->role == ROLE_MEMORY ? 'm' : 'c', worker->index);
	if (prctl(PR_SET_NAME, comm, 0, 0, 0)) fail("prctl");
	worker->tid = (uint64_t)syscall(SYS_gettid);
#else
	worker->tid = worker->index + 1;
#endif
	set_affinity((spread || migrating) ? worker->index : 0, !spread || migrating);
#ifdef __linux__
	{
		cpu_set_t initial;
		if (sched_getaffinity(0, sizeof(initial), &initial)) fail("sched_getaffinity worker");
		for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
			if (CPU_ISSET(cpu, &initial) && worker->initial_allowed_count < MAX_CPUS)
				worker->initial_allowed_cpus[worker->initial_allowed_count++] = cpu;
	}
#else
	worker->initial_allowed_cpus[worker->initial_allowed_count++] = -1;
#endif
	if (worker->role == ROLE_MEMORY) {
#ifdef __linux__
		worker->memory = mmap(NULL, worker->memory_bytes, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (worker->memory == MAP_FAILED) fail("mmap");
#else
		worker->memory = calloc(1, worker->memory_bytes);
		if (!worker->memory) fail("calloc memory");
#endif
		/* Fault each page before the measurement window. */
		for (size_t offset = 0; offset < worker->memory_bytes; offset += 4096)
			((volatile unsigned char *)worker->memory)[offset] = (unsigned char)offset;
	}
	calibrate(worker);
	pthread_mutex_lock(&ready_mutex);
	workers_ready++;
	pthread_cond_broadcast(&ready_condition);
	while (!start_ready) pthread_cond_wait(&ready_condition, &ready_mutex);
	pthread_mutex_unlock(&ready_mutex);
	sleep_until(start_ns);
	expected = previous_end = start_ns;
	while (clock_ns(CLOCK_MONOTONIC) < end_ns) {
		beginning = clock_ns(CLOCK_MONOTONIC);
		if (!measured && beginning >= measured_start_ns) {
			measured = 1;
			measured_cpu_start = clock_ns(CLOCK_THREAD_CPUTIME_ID);
			previous_end = measured_start_ns;
			last_measured_progress = measured_start_ns;
#ifdef __linux__
			if (getrusage(RUSAGE_THREAD, &usage_start)) fail("getrusage");
#endif
		}
#ifdef __linux__
		current_cpu = sched_getcpu();
		if (current_cpu < 0) fail("sched_getcpu");
#endif
		units = burn(worker, &service);
		finished = clock_ns(CLOCK_MONOTONIC);
		if (measured && finished <= end_ns) {
			uint64_t gap = finished - previous_end;
			worker->units += units;
			worker->cpu_service_ns += service;
			if (gap > worker->max_gap_ns) worker->max_gap_ns = gap;
			last_measured_progress = finished;
			if (previous_cpu >= 0 && current_cpu != previous_cpu) worker->migrations++;
			previous_cpu = current_cpu;
			if (worker->sample_count < MAX_SAMPLES) {
				struct sample *sample = &worker->samples[worker->sample_count++];
				sample->end_ns = finished;
				sample->cpu_ns = service;
				sample->wall_ns = finished - beginning;
				sample->wake_lateness_ns = worker->role == ROLE_BURST && beginning > expected ?
					beginning - expected : 0;
				sample->completion_ns = worker->role == ROLE_BURST ? finished - expected :
					finished - beginning;
				sample->units = units;
				sample->progress_gap_ns = gap;
				sample->cpu = current_cpu;
			} else worker->dropped_samples++;
		}
		previous_end = finished;
		iteration++;
		if (migrating && (iteration + worker->index) % 50 == 0) {
			set_affinity(iteration / 50 + worker->index, 1);
			if (measured) worker->affinity_changes++;
		}
		if (worker->role == ROLE_BURST) {
			expected = finished + worker->sleep_ns;
			/* A final real sleep completes the final burst for BPF labels. */
			sleep_until(expected < end_ns ? expected : end_ns);
		}
	}
	if (measured) {
		/* A task can be delayed past the deadline without another completed
		 * sample. Retain that trailing interval instead of hiding starvation. */
		if (last_measured_progress < end_ns && end_ns - last_measured_progress > worker->max_gap_ns)
			worker->max_gap_ns = end_ns - last_measured_progress;
		measured_cpu_end = clock_ns(CLOCK_THREAD_CPUTIME_ID);
		worker->process_cpu_ns = measured_cpu_end - measured_cpu_start;
#ifdef __linux__
		if (getrusage(RUSAGE_THREAD, &usage_end)) fail("getrusage");
		worker->voluntary_switches = usage_end.ru_nvcsw - usage_start.ru_nvcsw;
		worker->involuntary_switches = usage_end.ru_nivcsw - usage_start.ru_nivcsw;
		worker->minor_faults = usage_end.ru_minflt - usage_start.ru_minflt;
		worker->major_faults = usage_end.ru_majflt - usage_start.ru_majflt;
#endif
	}
	return NULL;
}

static unsigned add_worker(enum role role, uint64_t burst_us, uint64_t sleep_us)
{
	struct worker *worker;
	if (worker_count == MAX_WORKERS) { fprintf(stderr, "too many workers\n"); exit(1); }
	worker = &workers[worker_count];
	worker->index = worker_count++;
	worker->role = role;
	worker->burst_ns = burst_us * 1000;
	worker->sleep_ns = sleep_us * 1000;
	worker->state = UINT64_C(0x91b8c542410d87bd) + worker->index;
	worker->memory_bytes = memory_bytes;
	worker->samples = calloc(MAX_SAMPLES, sizeof(*worker->samples));
	if (!worker->samples) fail("calloc samples");
	return worker->index;
}

static void setup(const char *workload)
{
	if (!strcmp(workload, "cpu")) {
		for (int i = 0; i < 8; i++) add_worker(ROLE_CPU, 1000, 0);
	} else if (!strcmp(workload, "shortburst") || !strcmp(workload, "wakeheavy")) {
		int wakeheavy = !strcmp(workload, "wakeheavy");
		for (int i = 0; i < (wakeheavy ? 6 : 4); i++)
			add_worker(ROLE_BURST, wakeheavy ? 50 : 200, wakeheavy ? 500 : 1800);
		for (int i = 0; i < 2; i++) add_worker(ROLE_CPU, 1000, 0);
	} else if (!strcmp(workload, "mixed") || !strcmp(workload, "multicore")) {
		spread = !strcmp(workload, "multicore");
		for (int i = 0; i < 2; i++) add_worker(ROLE_BURST, 300, 1700);
		for (int i = 0; i < 2; i++) add_worker(ROLE_BURST, 3000, 2000);
		for (int i = 0; i < (spread ? 8 : 4); i++) add_worker(ROLE_CPU, 1000, 0);
	} else if (!strcmp(workload, "affinity")) {
		migrating = spread = 1;
		for (int i = 0; i < 8; i++) add_worker(ROLE_CPU, 1000, 0);
	} else if (!strcmp(workload, "memory")) {
		spread = 1;
		for (int i = 0; i < 4; i++) add_worker(ROLE_CPU, 1000, 0);
		for (int i = 0; i < 2; i++) add_worker(ROLE_BURST, 300, 1700);
		add_worker(ROLE_MEMORY, 1000, 0);
	} else {
		fprintf(stderr, "unknown workload: %s\n", workload);
		exit(1);
	}
}

static void parse_cpus(char *list)
{
	char *save = NULL;
	for (char *token = strtok_r(list, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
		char *tail;
		long cpu = strtol(token, &tail, 10);
		if (!*token || *tail || cpu < 0 || cpu >= 1024 || cpu_count == MAX_CPUS) {
			fprintf(stderr, "invalid --cpus list\n"); exit(1);
		}
		cpus[cpu_count++] = (int)cpu;
	}
	if (!cpu_count) { fprintf(stderr, "empty --cpus list\n"); exit(1); }
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{"workload", required_argument, NULL, 'w'},
		{"duration", required_argument, NULL, 'd'},
		{"warmup", required_argument, NULL, 'u'},
		{"cpus", required_argument, NULL, 'c'},
		{"memory-mib", required_argument, NULL, 'm'},
		{"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
	};
	const char *workload = "cpu";
	double duration = 10, warmup = 2;
	int option;
	while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
		char *tail;
		double value;
		switch (option) {
		case 'w': workload = optarg; break;
		case 'c': parse_cpus(optarg); break;
		case 'm': {
			unsigned long mib = strtoul(optarg, &tail, 10);
			if (!*optarg || *tail || mib < 1 || mib > 4096) {
				fprintf(stderr, "memory-mib must be in [1,4096]\n"); return 1;
			}
			memory_bytes = mib * 1024ULL * 1024; break;
		}
		case 'd': case 'u':
			value = strtod(optarg, &tail);
			if (!*optarg || *tail || !isfinite(value) || value < 0 || value > 3600 ||
			    (option == 'd' && value < 0.1)) {
				fprintf(stderr, "duration/warmup out of range\n"); return 1;
			}
			if (option == 'd') duration = value; else warmup = value;
			break;
		case 'h':
			printf("Usage: %s --workload cpu|shortburst|wakeheavy|mixed|multicore|affinity|memory\n"
			       "  [--duration 10] [--warmup 2] [--cpus 0,1,2,3] [--memory-mib 1024]\n", argv[0]);
			return 0;
		default: return 1;
		}
	}
	if (optind != argc) return 1;
	if (!cpu_count) {
#ifdef __linux__
		cpu_set_t available;
		if (sched_getaffinity(0, sizeof(available), &available)) fail("sched_getaffinity");
		for (int cpu = 0; cpu < CPU_SETSIZE && cpu_count < MAX_CPUS; cpu++)
			if (CPU_ISSET(cpu, &available)) cpus[cpu_count++] = cpu;
#else
		cpus[cpu_count++] = 0;
#endif
	}
	setup(workload);
	/* CPU and burst contention categories share one explicitly chosen CPU. */
	if (!spread) cpu_count = 1;
	for (int i = 0; i < worker_count; i++)
		if (pthread_create(&workers[i].thread, NULL, run_worker, &workers[i])) fail("pthread_create");
	/* First wait for calibration and page faults, then publish one future clock
	 * window through the condition variable's synchronization edge. */
	pthread_mutex_lock(&ready_mutex);
	while (workers_ready < worker_count) pthread_cond_wait(&ready_condition, &ready_mutex);
	start_ns = clock_ns(CLOCK_MONOTONIC) + 100000000;
	measured_start_ns = start_ns + (uint64_t)(warmup * NS_SECOND);
	end_ns = measured_start_ns + (uint64_t)(duration * NS_SECOND);
	start_ready = 1;
	pthread_cond_broadcast(&ready_condition);
	pthread_mutex_unlock(&ready_mutex);
	for (int i = 0; i < worker_count; i++)
		if (pthread_join(workers[i].thread, NULL)) fail("pthread_join");
	printf("{\"event\":\"window\",\"workload\":\"%s\",\"pid\":%ld,\"start_ns\":%" PRIu64
	       ",\"measured_start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64
	       ",\"duration_seconds\":%.6f,\"warmup_seconds\":%.6f,\"cpu_count\":%d,\"workers\":%d,\"selected_cpus\":[",
	       workload, (long)getpid(), start_ns, measured_start_ns, end_ns, duration, warmup, cpu_count, worker_count);
	for (int i = 0; i < cpu_count; i++) printf("%s%d", i ? "," : "", cpus[i]);
	printf("]}\n");
	for (int i = 0; i < worker_count; i++) {
		struct worker *worker = &workers[i];
		const char *role = worker->role == ROLE_BURST ? "burst" : worker->role == ROLE_MEMORY ? "memory" : "cpu";
		printf("{\"event\":\"worker\",\"index\":%u,\"tid\":%" PRIu64 ",\"role\":\"%s\","
		       "\"samples\":%u,\"dropped_samples\":%u,\"units\":%" PRIu64 ",\"cpu_service_ns\":%" PRIu64
		       ",\"process_cpu_ns\":%" PRIu64 ",\"max_progress_gap_ns\":%" PRIu64 ",\"observed_migrations\":%u,"
		       "\"affinity_changes\":%u,\"voluntary_switches\":%ld,\"involuntary_switches\":%ld,"
		       "\"minor_faults\":%ld,\"major_faults\":%ld,\"burst_target_ns\":%" PRIu64
		       ",\"sleep_ns\":%" PRIu64 ",\"memory_bytes\":%zu,\"batch\":%" PRIu64 ",\"initial_allowed_cpus\":[",
		       worker->index, worker->tid, role, worker->sample_count, worker->dropped_samples,
		       worker->units, worker->cpu_service_ns, worker->process_cpu_ns, worker->max_gap_ns,
		       worker->migrations, worker->affinity_changes, worker->voluntary_switches,
		       worker->involuntary_switches, worker->minor_faults, worker->major_faults,
		       worker->burst_ns, worker->sleep_ns, worker->role == ROLE_MEMORY ? worker->memory_bytes : 0,
		       worker->batch);
		for (unsigned j = 0; j < worker->initial_allowed_count; j++)
			printf("%s%d", j ? "," : "", worker->initial_allowed_cpus[j]);
		printf("]}\n");
		for (unsigned j = 0; j < worker->sample_count; j++) {
			struct sample *sample = &worker->samples[j];
			printf("{\"event\":\"sample\",\"tid\":%" PRIu64 ",\"role\":\"%s\",\"end_ns\":%" PRIu64
			       ",\"cpu_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64 ",\"wake_lateness_ns\":%" PRIu64
			       ",\"completion_ns\":%" PRIu64 ",\"units\":%" PRIu64 ",\"progress_gap_ns\":%" PRIu64
			       ",\"cpu\":%d}\n", worker->tid, role, sample->end_ns, sample->cpu_ns, sample->wall_ns,
			       sample->wake_lateness_ns, sample->completion_ns, sample->units, sample->progress_gap_ns, sample->cpu);
		}
		free(worker->samples);
		if (worker->memory) {
#ifdef __linux__
			munmap(worker->memory, worker->memory_bytes);
#else
			free(worker->memory);
#endif
		}
	}
	pthread_cond_destroy(&ready_condition);
	pthread_mutex_destroy(&ready_mutex);
	return 0;
}
