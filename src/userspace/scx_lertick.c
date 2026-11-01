/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Derived from scx_simple.c in the official sched-ext/scx-c-examples.
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 * Modified for Lertick on 2026-10-09: finite lifecycle, kernel probes,
 * task/model telemetry, bounded policy configuration, and guarded fault tests.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <scx/common.h>
#include "lertick.h"
#include "lerwatt_plugin.h"
#include "scx_lertick.bpf.skel.h"

#define U64(x) ((unsigned long long)(x))
#define STATE_PATH "/sys/kernel/sched_ext/state"
#define OPS_PATH "/sys/kernel/sched_ext/root/ops"

static bool verbose;
static volatile sig_atomic_t exit_req;
static const char *const stat_names[LERTICK_STAT_COUNT] = {
	[LERTICK_STAT_LOCAL_ENQUEUE] = "local_enqueue",
	[LERTICK_STAT_GLOBAL_ENQUEUE] = "global_enqueue",
	[LERTICK_STAT_RUNS] = "runs",
	[LERTICK_STAT_RUNTIME_NS] = "runtime_ns",
	[LERTICK_STAT_COMPLETED_BURSTS] = "completed_bursts",
	[LERTICK_STAT_CENSORED_BURSTS] = "censored_bursts",
	[LERTICK_STAT_TRAINING_UPDATES] = "training_updates",
	[LERTICK_STAT_INFERENCES] = "inferences",
	[LERTICK_STAT_ADAPTIVE_SLICES] = "adaptive_slices",
	[LERTICK_STAT_SLICE_CHANGES] = "slice_changes",
	[LERTICK_STAT_FAULT_STALLS] = "fault_stalls",
	[LERTICK_STAT_COUNTER_ERRORS] = "counter_errors",
	[LERTICK_STAT_ALLOC_FAILURES] = "alloc_failures",
	[LERTICK_STAT_SNAPSHOT_FAILURES] = "snapshot_failures",
	[LERTICK_STAT_PREDICTION_NS_SUM] = "prediction_ns_sum",
	[LERTICK_STAT_SELECTED_SLICE_NS_SUM] = "selected_slice_ns_sum",
	[LERTICK_STAT_MATCHING_DISPATCH_SLICES] = "matching_dispatch_slices",
	[LERTICK_STAT_CONTINUATION_SLICES] = "continuation_slices",
	[LERTICK_STAT_CONTINUATION_PENDING_NS] = "continuation_pending_ns",
	[LERTICK_STAT_INIT_TASKS] = "init_tasks",
	[LERTICK_STAT_EXIT_TASKS] = "exit_tasks",
	[LERTICK_STAT_TRAINING_TIME_NS] = "training_time_ns",
	[LERTICK_STAT_INFERENCE_TIME_NS] = "inference_time_ns",
	[LERTICK_STAT_PREDICTION_SAMPLES] = "prediction_samples",
	[LERTICK_STAT_LINEAR_ABS_ERROR_NS] = "linear_abs_error_ns",
	[LERTICK_STAT_EWMA_ABS_ERROR_NS] = "ewma_abs_error_ns",
	[LERTICK_STAT_WAKE_SAMPLES] = "wake_samples",
	[LERTICK_STAT_WAKE_TO_RUN_NS] = "wake_to_run_ns",
	[LERTICK_STAT_WAKE_TO_RUN_MAX_NS] = "wake_to_run_max_ns",
	[LERTICK_STAT_CPUPERF_SETS] = "cpuperf_sets",
	[LERTICK_STAT_CPUPERF_IDLE_SETS] = "cpuperf_idle_sets",
	[LERTICK_STAT_CONTRACT_APPLIED] = "contract_applied",
	[LERTICK_STAT_CONTRACT_STALE] = "contract_stale",
};

static int print_libbpf(enum libbpf_print_level level, const char *fmt, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, fmt, args);
}

static void signal_exit(int sig)
{
	exit_req = sig;
}

static __u64 now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void pause_ms(unsigned ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
	nanosleep(&ts, NULL);
}

static bool read_text(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");
	size_t n;
	if (!f || size < 2) {
		if (f) fclose(f);
		return false;
	}
	n = fread(buf, 1, size - 1, f);
	fclose(f);
	buf[n] = 0;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = 0;
	return n > 0;
}

static void json_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			fputc('\\', f);
			fputc(c, f);
		} else if (c < 32) {
			fprintf(f, "\\u%04x", c);
		} else {
			fputc(c, f);
		}
	}
	fputc('"', f);
}

static bool virtual_machine(void)
{
	const char *const paths[] = {
		"/sys/class/dmi/id/product_name", "/sys/class/dmi/id/sys_vendor",
		"/sys/hypervisor/type", "/proc/device-tree/model",
	};
	char buf[256];
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (read_text(paths[i], buf, sizeof(buf)) &&
		    (strcasestr(buf, "qemu") || strcasestr(buf, "kvm") ||
		     strcasestr(buf, "linux,dummy-virt")))
			return true;
	}
	return false;
}

static int kernel_check(FILE *out, bool require_disabled)
{
	const char *const flags[] = {
		"CONFIG_SCHED_CLASS_EXT", "CONFIG_BPF", "CONFIG_BPF_SYSCALL",
		"CONFIG_BPF_JIT", "CONFIG_DEBUG_INFO_BTF",
	};
	bool found[sizeof(flags) / sizeof(flags[0])] = {0};
	char path[512], line[1024], state[64] = "unavailable";
	struct utsname u;
	struct btf *btf = NULL;
	gzFile cfg;
	int fd, status = 0;
	uname(&u);
	if (strcmp(u.sysname, "Linux")) {
		fprintf(stderr, "BLOCKER: an actual Linux guest kernel is required.\n");
		return 1;
	}
	snprintf(path, sizeof(path), "/boot/config-%s", u.release);
	cfg = gzopen("/proc/config.gz", "rb");
	if (!cfg) cfg = gzopen(path, "rb");
	if (!cfg) {
		fprintf(stderr, "BLOCKER: cannot inspect /proc/config.gz or %s.\n", path);
		status = 1;
	} else {
		while (gzgets(cfg, line, sizeof(line))) {
			for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
				size_t n = strlen(flags[i]);
				if (!strncmp(line, flags[i], n) && !strcmp(line + n, "=y\n"))
					found[i] = true;
			}
		}
		gzclose(cfg);
	}
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		if (!found[i]) {
			fprintf(stderr, "BLOCKER: %s=y could not be confirmed.\n", flags[i]);
			status = 1;
		}
	}
	btf = btf__load_vmlinux_btf();
	if (!btf || btf__find_by_name_kind(btf, "sched_ext_ops", BTF_KIND_STRUCT) < 0) {
		fprintf(stderr, "BLOCKER: running kernel BTF/sched_ext_ops unavailable.\n");
		status = 1;
	}
	if (!read_text(STATE_PATH, state, sizeof(state))) {
		fprintf(stderr, "BLOCKER: %s unavailable.\n", STATE_PATH);
		status = 1;
	} else if (require_disabled && strcmp(state, "disabled")) {
		fprintf(stderr, "Refusing to replace existing sched_ext scheduler: state=%s.\n", state);
		status = 1;
	}
	fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, "lertick_probe", sizeof(__u32), sizeof(__u64), 1, NULL);
	if (fd < 0) {
		fprintf(stderr, "BLOCKER: BPF map creation failed: %s. Run as root in the guest.\n", strerror(errno));
		status = 1;
	} else close(fd);
	fprintf(out, "{\"event\":\"kernel_check\",\"kernel\":");
	json_string(out, u.release);
	fprintf(out, ",\"arch\":"); json_string(out, u.machine);
	fprintf(out, ",\"state\":"); json_string(out, state);
	fprintf(out, ",\"config\":{");
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
		if (i) fputc(',', out);
		json_string(out, flags[i]); fprintf(out, ":%s", found[i] ? "true" : "false");
	}
	fprintf(out, "},\"bpf_map_create\":%s,\"btf_sched_ext_ops\":%s,\"passed\":%s}\n",
		fd >= 0 ? "true" : "false",
		btf && btf__find_by_name_kind(btf, "sched_ext_ops", BTF_KIND_STRUCT) >= 0 ? "true" : "false",
		status ? "false" : "true");
	fflush(out);
	btf__free(btf);
	return status;
}

static bool wait_state(const char *wanted)
{
	char state[64];
	for (unsigned i = 0; i < 30; i++) {
		if (read_text(STATE_PATH, state, sizeof(state)) && !strcmp(state, wanted))
			return true;
		pause_ms(100);
	}
	return false;
}

/* Per-CPU state, all possible CPUs; buf holds libbpf_num_possible_cpus() entries. */
static int read_cpu_state(struct scx_lertick *skel, struct lertick_cpu_state *buf)
{
	__u32 zero = 0;

	return bpf_map_lookup_elem(bpf_map__fd(skel->maps.cpu_state), &zero, buf);
}

static int emit_cpus(FILE *out, struct scx_lertick *skel)
{
	int ncpu = libbpf_num_possible_cpus();
	struct lertick_cpu_state *v;

	if (ncpu <= 0)
		return -1;
	v = calloc((size_t)ncpu, sizeof(*v));
	if (!v)
		return -1;
	if (read_cpu_state(skel, v)) {
		free(v);
		return -1;
	}
	fprintf(out, ",\"cpus\":[");
	for (int i = 0; i < ncpu; i++)
		fprintf(out, "%s{\"cpu\":%d,\"util\":%u,\"util_cap\":%u,\"perf\":%u,\"perf_sent\":%u}",
			i ? "," : "", i, v[i].util, v[i].util_cap, v[i].perf_target, v[i].perf_sent);
	fputc(']', out);
	free(v);
	return 0;
}

static int publish_demand(struct scx_lertick *skel, struct lt_demand_rec *mem,
			  const struct ltlw_topology *topo, bool cpuperf_util,
			  struct ltlw_demand_state *st, struct lertick_cpu_state *buf)
{
	struct lt_demand_rec recs[LW_CONTRACT_MAX_CLUSTERS];
	bool valid[LW_CONTRACT_MAX_CLUSTERS];
	int ncpu = libbpf_num_possible_cpus();

	if (ncpu <= 0 || read_cpu_state(skel, buf))
		return -1;
	ltlw_demand_compute(topo, buf, ncpu, cpuperf_util, st, now_ns(), recs, valid);
	for (int c = 0; c < LW_CONTRACT_MAX_CLUSTERS; c++)
		if (valid[c])
			ltlw_publish(&mem[c], &recs[c]);
	return 0;
}

static int emit_snapshot(FILE *out, struct scx_lertick *skel, const char *phase, const char *mode_name,
			 bool cpus)
{
	int ncpu = libbpf_num_possible_cpus();
	__u64 sums[LERTICK_STAT_COUNT] = {0};
	__u64 *cpu_values;
	__u32 key, next;
	struct lertick_task_snapshot s;
	unsigned count = 0, scanned = 0;
	int err = 0;
	if (ncpu <= 0) return -1;
	cpu_values = calloc((size_t)ncpu, sizeof(*cpu_values));
	if (!cpu_values) return -1;
	for (key = 0; key < LERTICK_STAT_COUNT; key++) {
		if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats), &key, cpu_values)) {
			err = -1;
			break;
		}
		for (int cpu = 0; cpu < ncpu; cpu++) {
			if (key == LERTICK_STAT_WAKE_TO_RUN_MAX_NS) {
				if (cpu_values[cpu] > sums[key]) sums[key] = cpu_values[cpu];
			} else sums[key] += cpu_values[cpu];
		}
	}
	free(cpu_values);
	if (err) return err;
	fprintf(out, "{\"event\":\"snapshot\",\"phase\":"); json_string(out, phase);
	fprintf(out, ",\"mode\":"); json_string(out, mode_name);
	fprintf(out, ",\"monotonic_ns\":%llu,\"stats\":{", U64(now_ns()));
	for (key = 0; key < LERTICK_STAT_COUNT; key++) {
		if (key) fputc(',', out);
		json_string(out, stat_names[key]); fprintf(out, ":%llu", U64(sums[key]));
	}
	fprintf(out, ",\"live_tasks\":%lld", (long long)sums[LERTICK_STAT_INIT_TASKS] -
		(long long)sums[LERTICK_STAT_EXIT_TASKS]);
	fprintf(out, "},\"tasks\":[");
	err = bpf_map_get_next_key(bpf_map__fd(skel->maps.task_snapshots), NULL, &next);
	/* LRU eviction may invalidate the previous key and restart enumeration. */
	while (!err && count < LERTICK_MAX_TASK_SNAPSHOTS &&
	       scanned++ < 2 * LERTICK_MAX_TASK_SNAPSHOTS) {
		key = next;
		if (!bpf_map_lookup_elem(bpf_map__fd(skel->maps.task_snapshots), &key, &s)) {
			if (count++) fputc(',', out);
			fprintf(out, "{\"pid\":%u,\"tgid\":%u,\"comm\":", s.pid, s.tgid);
			s.comm[LERTICK_COMM_LEN - 1] = 0;
			json_string(out, s.comm);
#define SNAP_U64(field) fprintf(out, ",\"" #field "\":%llu", U64(s.field))
			SNAP_U64(runs); SNAP_U64(enqueue_count); SNAP_U64(total_runtime_ns);
			SNAP_U64(completed_bursts); SNAP_U64(censored_bursts); SNAP_U64(training_updates);
			SNAP_U64(inferences); SNAP_U64(adaptive_slices); SNAP_U64(slice_changes);
			SNAP_U64(last_burst_ns); SNAP_U64(max_burst_ns); SNAP_U64(episode_runtime_ns);
			SNAP_U64(prediction_ns); SNAP_U64(selected_slice_ns);
			SNAP_U64(min_selected_slice_ns); SNAP_U64(max_selected_slice_ns);
			SNAP_U64(min_adaptive_slice_ns); SNAP_U64(max_adaptive_slice_ns);
			SNAP_U64(observed_running_slice_ns); SNAP_U64(min_running_slice_ns);
			SNAP_U64(max_running_slice_ns); SNAP_U64(matching_dispatch_slices);
			SNAP_U64(adaptive_runs);
			SNAP_U64(slice_sum_ns); SNAP_U64(last_runtime_delta_ns);
			SNAP_U64(last_sleep_interval_ns); SNAP_U64(last_queue_depth);
			SNAP_U64(continuation_slices); SNAP_U64(continuing_pending_ns);
			SNAP_U64(continuing_consumed_ns); SNAP_U64(continuing_prediction_ns);
			SNAP_U64(continuing_selected_slice_ns);
			SNAP_U64(continuing_model_updates); SNAP_U64(continuing_episode_valid);
			SNAP_U64(prediction_samples); SNAP_U64(linear_abs_error_ns); SNAP_U64(ewma_abs_error_ns);
			SNAP_U64(linear_squared_error_us2); SNAP_U64(ewma_squared_error_us2);
			SNAP_U64(raw_burst_ns_sum); SNAP_U64(clipped_targets);
			SNAP_U64(wake_samples); SNAP_U64(wake_to_run_ns); SNAP_U64(wake_to_run_max_ns);
#undef SNAP_U64
			fprintf(out, ",\"model_weight_q16\":[");
			for (int i = 0; i < LERTICK_FEATURES; i++) fprintf(out, "%s%d", i ? "," : "", s.model_weight_q16[i]);
			fprintf(out, "],\"features_q16\":[");
			for (int i = 0; i < LERTICK_FEATURES; i++) fprintf(out, "%s%d", i ? "," : "", s.features_q16[i]);
			fprintf(out, "],\"last_target_q16\":%d,\"last_prediction_q16\":%d,\"last_training_error_q16\":%d,"
				"\"last_censor_reason\":%u,\"episode_active\":%u,\"episode_valid\":%u,\"exited\":%u}",
				s.last_target_q16, s.last_prediction_q16, s.last_training_error_q16,
				s.last_censor_reason, s.episode_active, s.episode_valid, s.exited);
		}
		err = bpf_map_get_next_key(bpf_map__fd(skel->maps.task_snapshots), &key, &next);
	}
	fputc(']', out);
	if (cpus && emit_cpus(out, skel))
		return -1;
	fprintf(out, "}\n");
	return fflush(out) || ferror(out) ? -1 : 0;
}

static unsigned parse_uint(const char *text, unsigned min, unsigned max)
{
	char *end;
	unsigned long val;
	errno = 0;
	val = strtoul(text, &end, 10);
	if (errno || !*text || *end || val < min || val > max) {
		fprintf(stderr, "Invalid numeric argument: %s (range %u..%u)\n", text, min, max);
		exit(2);
	}
	return (unsigned)val;
}

static void usage(FILE *out, const char *argv0)
{
	fprintf(out, "Usage: %s [--mode fixed|shadow|online|ewma] [--duration SEC] [--stats-json FILE]\n"
		"  --check                 Probe kernel config, BTF, BPF permission, sched_ext\n"
		"  --verbose               Emit libbpf debug/verifier diagnostics\n"
		"  --profile               Time in-kernel model operations (observer cost)\n"
		"  --model-init bias|ewma  Linear-model prior (default bias; ewma uses burst history)\n"
		"  --sample-ms N           Snapshot interval (100..10000, default 1000)\n"
		"  --tracked-tgid PID      Track this TGID (default comm prefix lertick)\n"
		"  --track-all             Learn all tasks (includes compiler descendants)\n"
		"  --fixed-slice-us N      Fixed/warmup slice (default 2000)\n"
		"  --min-slice-us N        Prediction slice lower bound (default 500)\n"
		"  --max-slice-us N        Prediction slice upper bound (default 5000)\n"
		"  --partial               Schedule only explicit SCHED_EXT tasks\n"
		"  --cpuperf auto|util|max CPU frequency requests to schedutil (default auto:\n"
		"                          util when a policy uses schedutil or with --lerwatt;\n"
		"                          max keeps the kernel default, every CPU at 1024)\n"
		"  --lerwatt               Attach the optional Lerwatt plug-in: pin contract\n"
		"                          maps under " LW_CONTRACT_PIN_DIR ", publish demand\n"
		"  --fault-stall --stall-tgid PID --partial --dangerous-vm-test\n"
		"                          QEMU-only opted-task watchdog injection\n"
		"  --expect-watchdog       Succeed only for observed stall watchdog fallback\n"
		"Duration is finite (1..3600 seconds, default 30). FILE '-' means stdout.\n"
		"This GPL-2.0-only scheduler comes with no warranty. See LICENSES/.\n", argv0);
}

static void lerwatt_detach(struct scx_lertick *skel, bool pinned, void *mem, size_t len)
{
	if (mem && mem != MAP_FAILED)
		munmap(mem, len);
	if (!pinned)
		return;
	bpf_map__unpin(skel->maps.lw_contract, NULL);
	bpf_map__unpin(skel->maps.lt_demand, NULL);
	rmdir(LW_CONTRACT_PIN_DIR);
}

static void emit_topology(FILE *out, const struct ltlw_topology *t)
{
	fprintf(out, ",\"lerwatt_topology\":{\"source\":"); json_string(out, t->source);
	fprintf(out, ",\"root\":"); json_string(out, t->root[0] ? t->root : "/");
	fprintf(out, ",\"policies\":%d,\"clusters\":[", t->npolicies);
	for (int c = 0; c < t->nclusters; c++) {
		bool first = true;

		fprintf(out, "%s[", c ? "," : "");
		for (int cpu = 0; cpu < t->ncpus; cpu++) {
			if (t->cpu_cluster[cpu] != c)
				continue;
			fprintf(out, "%s%d", first ? "" : ",", cpu);
			first = false;
		}
		fputc(']', out);
	}
	fprintf(out, "],\"capacity_override\":[");
	for (int cpu = 0; cpu < t->ncpus; cpu++)
		fprintf(out, "%s%u", cpu ? "," : "", t->cpu_capacity[cpu]);
	fprintf(out, "]}");
}

int main(int argc, char **argv)
{
	struct scx_lertick *skel;
	struct bpf_link *link;
	FILE *out = stdout;
	struct sigaction sa = { .sa_handler = signal_exit };
	struct rlimit memlock = { RLIM_INFINITY, RLIM_INFINITY };
	const char *stats_path = "-", *mode_name = "online";
	unsigned duration = 30, sample_ms = 1000, mode = LERTICK_MODE_ONLINE;
	unsigned fixed_us = 2000, min_us = 500, max_us = 5000, tracked_tgid = 0, stall_tgid = 0;
	bool check = false, partial = false, fault = false, danger = false, expect_watchdog = false, profile = false;
	bool track_all = false;
	bool fallback, restored, watchdog_observed = false;
	__u64 start, next_sample, exit_code;
	u64 stall_kind = 0, unreg_kind = 0;
	int opt, status = 0, final_kind;
	char ops[128] = "unavailable";
	/* CPU frequency requests and the optional Lerwatt plug-in. */
	const char *cpuperf_arg = "auto", *cpuperf_reason = "explicit";
	unsigned cpuperf = LERTICK_CPUPERF_MAX;
	bool lerwatt = false, pinned = false, cpu_report;
	bool model_init_ewma = false;
	static struct ltlw_topology topo;
	struct ltlw_demand_state demand = { 0 };
	struct lt_demand_rec *demand_mem = MAP_FAILED;
	struct lertick_cpu_state *cpu_buf = NULL;
	size_t demand_len = 0;
	__u64 next_publish;
	int ncpus;
	enum { OPT_CHECK = 1000, OPT_SAMPLE, OPT_TRACK, OPT_FIXED, OPT_MIN, OPT_MAX,
	       OPT_PARTIAL, OPT_FAULT, OPT_STALL, OPT_DANGER, OPT_EXPECT, OPT_PROFILE, OPT_ALL,
	       OPT_CPUPERF, OPT_LERWATT, OPT_MODEL_INIT };
	static const struct option opts[] = {
		{ "mode", required_argument, NULL, 'm' }, { "duration", required_argument, NULL, 'd' },
		{ "stats-json", required_argument, NULL, 'o' }, { "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' }, { "check", no_argument, NULL, OPT_CHECK },
		{ "profile", no_argument, NULL, OPT_PROFILE },
		{ "track-all", no_argument, NULL, OPT_ALL },
		{ "sample-ms", required_argument, NULL, OPT_SAMPLE }, { "tracked-tgid", required_argument, NULL, OPT_TRACK },
		{ "fixed-slice-us", required_argument, NULL, OPT_FIXED }, { "min-slice-us", required_argument, NULL, OPT_MIN },
		{ "max-slice-us", required_argument, NULL, OPT_MAX }, { "partial", no_argument, NULL, OPT_PARTIAL },
		{ "fault-stall", no_argument, NULL, OPT_FAULT }, { "stall-tgid", required_argument, NULL, OPT_STALL },
		{ "dangerous-vm-test", no_argument, NULL, OPT_DANGER }, { "expect-watchdog", no_argument, NULL, OPT_EXPECT },
		{ "cpuperf", required_argument, NULL, OPT_CPUPERF }, { "lerwatt", no_argument, NULL, OPT_LERWATT },
		{ "model-init", required_argument, NULL, OPT_MODEL_INIT },
		{ NULL, 0, NULL, 0 },
	};
	while ((opt = getopt_long(argc, argv, "m:d:o:vh", opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			mode_name = optarg;
			if (!strcmp(optarg, "fixed")) mode = LERTICK_MODE_FIXED;
			else if (!strcmp(optarg, "shadow")) mode = LERTICK_MODE_SHADOW;
			else if (!strcmp(optarg, "online")) mode = LERTICK_MODE_ONLINE;
			else if (!strcmp(optarg, "ewma")) mode = LERTICK_MODE_EWMA;
			else { usage(stderr, argv[0]); return 2; }
			break;
		case 'd': duration = parse_uint(optarg, 1, 3600); break;
		case 'o': stats_path = optarg; break;
		case 'v': verbose = true; break;
		case 'h': usage(stdout, argv[0]); return 0;
		case OPT_CHECK: check = true; break;
		case OPT_SAMPLE: sample_ms = parse_uint(optarg, 100, 10000); break;
		case OPT_TRACK: tracked_tgid = parse_uint(optarg, 1, 0x7fffffff); break;
		case OPT_FIXED: fixed_us = parse_uint(optarg, 100, 20000); break;
		case OPT_MIN: min_us = parse_uint(optarg, 100, 20000); break;
		case OPT_MAX: max_us = parse_uint(optarg, 100, 20000); break;
		case OPT_PARTIAL: partial = true; break;
		case OPT_FAULT: fault = true; break;
		case OPT_STALL: stall_tgid = parse_uint(optarg, 1, 0x7fffffff); break;
		case OPT_DANGER: danger = true; break;
		case OPT_EXPECT: expect_watchdog = true; break;
		case OPT_PROFILE: profile = true; break;
		case OPT_ALL: track_all = true; break;
		case OPT_CPUPERF:
			cpuperf_arg = optarg;
			if (strcmp(optarg, "auto") && strcmp(optarg, "util") && strcmp(optarg, "max")) {
				usage(stderr, argv[0]);
				return 2;
			}
			break;
		case OPT_LERWATT: lerwatt = true; break;
		case OPT_MODEL_INIT:
			if (strcmp(optarg, "bias") && strcmp(optarg, "ewma")) {
				usage(stderr, argv[0]);
				return 2;
			}
			model_init_ewma = !strcmp(optarg, "ewma");
			break;
		default: usage(stderr, argv[0]); return 2;
		}
	}
	if (optind != argc || min_us > fixed_us || fixed_us > max_us) {
		fprintf(stderr, "Require min_slice <= fixed_slice <= max_slice and no positional arguments.\n");
		return 2;
	}
	if ((fault && (!partial || !danger || !stall_tgid || !virtual_machine())) ||
	    (!fault && (stall_tgid || danger || expect_watchdog))) {
		fprintf(stderr, "Fault injection requires --partial --dangerous-vm-test --stall-tgid PID in a detected QEMU/KVM VM.\n");
		return 2;
	}
	libbpf_set_print(print_libbpf);
	if (strcmp(stats_path, "-")) {
		out = fopen(stats_path, "w");
		if (!out) { perror(stats_path); return 1; }
	}
	setrlimit(RLIMIT_MEMLOCK, &memlock);
	status = kernel_check(out, !check);
	if (check || status) {
		if (out != stdout) fclose(out);
		return status;
	}
	ncpus = libbpf_num_possible_cpus();
	if (ncpus <= 0) {
		fprintf(stderr, "BLOCKER: cannot count possible CPUs.\n");
		return 1;
	}
	/* auto: request frequencies only where schedutil reads them, or for Lerwatt. */
	if (!strcmp(cpuperf_arg, "util")) {
		cpuperf = LERTICK_CPUPERF_UTIL;
	} else if (!strcmp(cpuperf_arg, "max")) {
		cpuperf = LERTICK_CPUPERF_MAX;
	} else if (lerwatt) {
		cpuperf = LERTICK_CPUPERF_UTIL;
		cpuperf_reason = "lerwatt";
	} else if (ltlw_schedutil_in_use("")) {
		cpuperf = LERTICK_CPUPERF_UTIL;
		cpuperf_reason = "schedutil";
	} else {
		cpuperf_reason = "no-schedutil";
	}
	cpu_report = cpuperf == LERTICK_CPUPERF_UTIL || lerwatt;
	if (lerwatt && ltlw_discover(&topo, ncpus, "", NULL)) {
		fprintf(stderr, "Lerwatt plug-in: cannot discover CPU clusters.\n");
		return 1;
	}
	if (lerwatt && topo.npolicies > LW_CONTRACT_MAX_CLUSTERS)
		fprintf(stderr, "Lerwatt plug-in: %d cpufreq policies; CPUs past the first %d have no contract.\n",
			topo.npolicies, LW_CONTRACT_MAX_CLUSTERS);
	if (ncpus > (int)LERTICK_MAX_CPUS && cpu_report)
		fprintf(stderr, "Warning: CPUs %u and above keep the kernel's performance target.\n",
			LERTICK_MAX_CPUS);
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
	skel = SCX_OPS_OPEN(lertick_ops, scx_lertick);
	if (verbose) {
		struct bpf_program *prog;
		bpf_object__for_each_program(prog, skel->obj)
			bpf_program__set_log_level(prog, 2);
	}
	skel->rodata->mode = mode;
	skel->rodata->fixed_slice_ns = (__u64)fixed_us * 1000;
	skel->rodata->min_slice_ns = (__u64)min_us * 1000;
	skel->rodata->max_slice_ns = (__u64)max_us * 1000;
	skel->rodata->tracked_tgid = tracked_tgid;
	skel->rodata->fault_stall = fault;
	skel->rodata->stall_tgid = stall_tgid;
	skel->rodata->profile_model = profile;
	skel->rodata->model_init_ewma = model_init_ewma;
	skel->rodata->track_all = track_all;
	skel->rodata->cpuperf_mode = cpuperf;
	skel->rodata->lerwatt_enabled = lerwatt;
	skel->rodata->nr_cpus = ncpus < (int)LERTICK_MAX_CPUS ? (unsigned)ncpus : LERTICK_MAX_CPUS;
	if (lerwatt) {
		for (unsigned i = 0; i < LERTICK_MAX_CPUS; i++) {
			skel->rodata->cpu_cluster[i] = topo.cpu_cluster[i];
			skel->rodata->cpu_cap_override[i] = topo.cpu_capacity[i];
		}
	}
	if (partial) {
		__u64 flag = SCX_OPS_SWITCH_PARTIAL;
		if (!flag) { fprintf(stderr, "BLOCKER: kernel lacks partial switch flag.\n"); scx_lertick__destroy(skel); return 1; }
		skel->struct_ops.lertick_ops->flags |= flag;
	}
	if (expect_watchdog && !__COMPAT_read_enum("scx_exit_kind", "SCX_EXIT_ERROR_STALL", &stall_kind)) {
		fprintf(stderr, "BLOCKER: cannot identify watchdog exit kind in running kernel BTF.\n");
		scx_lertick__destroy(skel);
		return 1;
	}
	if (!__COMPAT_read_enum("scx_exit_kind", "SCX_EXIT_UNREG", &unreg_kind)) {
		fprintf(stderr, "BLOCKER: kernel BTF lacks SCX_EXIT_UNREG; cannot validate shutdown/fallback semantics.\n");
		scx_lertick__destroy(skel);
		return 1;
	}
	SCX_OPS_LOAD(skel, lertick_ops, scx_lertick, uei);
	fprintf(out, "{\"event\":\"loaded\",\"verifier_accepted\":true}\n"); fflush(out);
	if (lerwatt) {
		const char *const pins[] = { LW_CONTRACT_PIN_LW, LW_CONTRACT_PIN_LT };
		long page = sysconf(_SC_PAGESIZE);
		char err[512] = "out of memory";
		bool ok;

		/* Pin before attach so that a failure leaves the default scheduler in place. */
		cpu_buf = calloc((size_t)ncpus, sizeof(*cpu_buf));
		demand_len = (sizeof(struct lt_demand_rec) * LW_CONTRACT_MAX_CLUSTERS +
			      (size_t)page - 1) / (size_t)page * (size_t)page;
		ok = cpu_buf && !ltlw_pin_prepare(LW_CONTRACT_PIN_DIR, pins, 2, err, sizeof(err));
		if (ok) {
			pinned = true;
			if (bpf_map__pin(skel->maps.lw_contract, LW_CONTRACT_PIN_LW) ||
			    bpf_map__pin(skel->maps.lt_demand, LW_CONTRACT_PIN_LT)) {
				snprintf(err, sizeof(err), "cannot pin contract maps: %s", strerror(errno));
				ok = false;
			}
		}
		if (ok) {
			demand_mem = mmap(NULL, demand_len, PROT_READ | PROT_WRITE, MAP_SHARED,
					  bpf_map__fd(skel->maps.lt_demand), 0);
			if (demand_mem == MAP_FAILED) {
				snprintf(err, sizeof(err), "cannot map lt_demand: %s", strerror(errno));
				ok = false;
			}
		}
		if (!ok) {
			fprintf(stderr, "Lerwatt plug-in: %s\n", err);
			lerwatt_detach(skel, pinned, demand_mem, demand_len);
			free(cpu_buf);
			scx_lertick__destroy(skel);
			if (out != stdout) fclose(out);
			return 1;
		}
	}
	link = SCX_OPS_ATTACH(skel, lertick_ops, scx_lertick);
	if (!wait_state("enabled") || !read_text(OPS_PATH, ops, sizeof(ops)) || strcmp(ops, "scx_lertick")) {
		fprintf(stderr, "Registration did not produce enabled scx_lertick (ops=%s).\n", ops);
		status = 1;
	} else {
		fprintf(out, "{\"event\":\"registered\",\"ops\":"); json_string(out, ops);
		fprintf(out, ",\"partial\":%s,\"mode\":", partial ? "true" : "false"); json_string(out, mode_name);
		fprintf(out, ",\"fixed_slice_ns\":%llu,\"min_slice_ns\":%llu,\"max_slice_ns\":%llu,\"timeout_ms\":%llu,\"profile_model\":%s,\"track_all\":%s",
			U64((__u64)fixed_us * 1000), U64((__u64)min_us * 1000), U64((__u64)max_us * 1000),
			U64(skel->struct_ops.lertick_ops->timeout_ms), profile ? "true" : "false",
			track_all ? "true" : "false");
		fprintf(out, ",\"cpuperf\":\"%s\",\"cpuperf_reason\":\"%s\",\"lerwatt\":%s",
			cpuperf == LERTICK_CPUPERF_UTIL ? "util" : "max", cpuperf_reason,
			lerwatt ? "true" : "false");
		fprintf(out, ",\"model_init\":\"%s\"", model_init_ewma ? "ewma" : "bias");
		if (lerwatt)
			emit_topology(out, &topo);
		fprintf(out, "}\n"); fflush(out);
	}
	start = now_ns(); next_sample = start; next_publish = start;
	while (!status && !exit_req && !UEI_EXITED(skel, uei) && now_ns() - start < (__u64)duration * 1000000000) {
		if (lerwatt && now_ns() >= next_publish) {
			if (publish_demand(skel, demand_mem, &topo, cpuperf == LERTICK_CPUPERF_UTIL,
					   &demand, cpu_buf)) {
				fprintf(stderr, "Failed to publish Lerwatt demand.\n"); status = 1; break;
			}
			next_publish = now_ns() + LTLW_PUBLISH_PERIOD_NS;
		}
		if (now_ns() >= next_sample) {
			if (emit_snapshot(out, skel, "periodic", mode_name, cpu_report)) {
				fprintf(stderr, "Failed to read/write scheduler telemetry.\n"); status = 1; break;
			}
			next_sample = now_ns() + (__u64)sample_ms * 1000000;
		}
		pause_ms(50);
	}
	fallback = UEI_EXITED(skel, uei) != 0;
	if (fallback) {
		watchdog_observed = expect_watchdog && (__u64)skel->data->uei.kind == stall_kind;
		if (!watchdog_observed) status = 1;
	}
	/* The link is never pinned. Closing it, including process death, unregisters ops. */
	bpf_link__destroy(link);
	restored = wait_state("disabled");
	if (!restored) { fprintf(stderr, "Default scheduler restoration could not be confirmed.\n"); status = 1; }
	/* Catch every abort racing shutdown, including below-ERROR kinds (SysRq). */
	final_kind = UEI_EXITED(skel, uei);
	if ((__u64)final_kind != unreg_kind) {
		fallback = true;
		watchdog_observed = expect_watchdog && (__u64)final_kind == stall_kind;
		if (!watchdog_observed) status = 1;
	}
	if (expect_watchdog && !watchdog_observed) status = 1;
	if (emit_snapshot(out, skel, "final", mode_name, cpu_report)) status = 1;
	/* Lerwatt sees the demand go stale and falls back to running alone. */
	lerwatt_detach(skel, pinned, demand_mem, demand_len);
	free(cpu_buf);
	exit_code = UEI_REPORT(skel, uei);
	fprintf(out, "{\"event\":\"exit\",\"unexpected_fallback\":%s,\"watchdog_observed\":%s,\"exit_kind\":%d,\"exit_code\":%lld,\"reason\":",
		fallback && !watchdog_observed ? "true" : "false", watchdog_observed ? "true" : "false",
		skel->data->uei.kind, (long long)exit_code);
	json_string(out, skel->data->uei.reason);
	fprintf(out, ",\"message\":"); json_string(out, skel->data->uei.msg);
	fprintf(out, ",\"default_restored\":%s,\"signal\":%d,\"status\":%d}\n", restored ? "true" : "false", (int)exit_req, status);
	fflush(out);
	scx_lertick__destroy(skel);
	if (out != stdout && fclose(out)) status = 1;
	return status;
}
