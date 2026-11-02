/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include "lerwatt_plugin.h"

#define BPF_FS_MAGIC_VALUE 0xcafe4a11
#define MAX_POLICIES 64

static bool read_line(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");
	bool ok;

	if (!f)
		return false;
	ok = fgets(buf, (int)size, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = 0;
	return ok;
}

int ltlw_parse_cpulist(const char *text, unsigned char *mask, int max)
{
	const char *s = text;
	int count = 0;

	while (*s) {
		char *end;
		long lo, hi;

		while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n')
			s++;
		if (!*s)
			break;
		errno = 0;
		lo = strtol(s, &end, 10);
		if (end == s || errno || lo < 0)
			return -1;
		hi = lo;
		s = end;
		if (*s == '-') {
			s++;
			errno = 0;
			hi = strtol(s, &end, 10);
			if (end == s || errno || hi < lo)
				return -1;
			s = end;
		}
		if (*s && *s != ' ' && *s != ',' && *s != '\t' && *s != '\n')
			return -1;
		/* Ignore unsupported IDs without walking an unbounded input range. */
		long last = hi < max ? hi : (long)max - 1;
		for (long cpu = lo; cpu <= last; cpu++) {
			if (cpu < max && !mask[cpu]) {
				mask[cpu] = 1;
				count++;
			}
		}
	}
	return count;
}

struct policy {
	int first;
	unsigned char mask[LERTICK_MAX_CPUS];
};

static int cmp_policy(const void *a, const void *b)
{
	return ((const struct policy *)a)->first - ((const struct policy *)b)->first;
}

/* Read <root>/sys/devices/system/cpu/cpufreq/policy* into p; returns count. */
static int read_policies(const char *root, struct policy *p, int max)
{
	char dir[PATH_MAX], path[PATH_MAX + 512], line[4096];
	struct dirent *e;
	DIR *d;
	int n = 0;

	snprintf(dir, sizeof(dir), "%s/sys/devices/system/cpu/cpufreq", root);
	d = opendir(dir);
	if (!d)
		return 0;
	while ((e = readdir(d)) && n < max) {
		int id;

		if (sscanf(e->d_name, "policy%d", &id) != 1)
			continue;
		snprintf(path, sizeof(path), "%s/%s/related_cpus", dir, e->d_name);
		if (!read_line(path, line, sizeof(line))) {
			snprintf(path, sizeof(path), "%s/%s/affected_cpus", dir, e->d_name);
			if (!read_line(path, line, sizeof(line)))
				continue;
		}
		memset(&p[n], 0, sizeof(p[n]));
		if (ltlw_parse_cpulist(line, p[n].mask, LERTICK_MAX_CPUS) <= 0)
			continue;
		p[n].first = -1;
		for (int cpu = 0; cpu < (int)LERTICK_MAX_CPUS; cpu++)
			if (p[n].mask[cpu]) {
				p[n].first = cpu;
				break;
			}
		if (p[n].first >= 0)
			n++;
	}
	closedir(d);
	qsort(p, (size_t)n, sizeof(*p), cmp_policy);
	return n;
}

static void assign_clusters(struct ltlw_topology *t, const struct policy *p, int n)
{
	for (int k = 0; k < n && k < LW_CONTRACT_MAX_CLUSTERS; k++) {
		__u64 hash = lw_contract_topology_seed();
		__u32 count = 0;

		/* Identity includes the full policy, not just this guest's CPUs. */
		for (int cpu = 0; cpu < (int)LERTICK_MAX_CPUS; cpu++) {
			if (p[k].mask[cpu]) {
				hash = lw_contract_topology_add_cpu(hash, (__u32)cpu);
				count++;
			}
		}
		t->cluster_topology_id[k] = lw_contract_topology_finish(hash, count);
		for (int cpu = 0; cpu < t->ncpus; cpu++) {
			if (p[k].mask[cpu] && t->cpu_cluster[cpu] == LERTICK_NO_CLUSTER) {
				t->cpu_cluster[cpu] = (unsigned char)k;
				t->cluster_ncpus[k]++;
			}
		}
	}
	t->npolicies = n;
	t->nclusters = n < LW_CONTRACT_MAX_CLUSTERS ? n : LW_CONTRACT_MAX_CLUSTERS;
}

static bool sim_root_valid(const char *root)
{
	char path[PATH_MAX];

	if (!root || !*root)
		return false;
	snprintf(path, sizeof(path), "%s/%s", root, LTLW_SIM_MARKER);
	return access(path, F_OK) == 0;
}

int ltlw_discover(struct ltlw_topology *t, int ncpus, const char *hw_root,
		  const char *sim_root)
{
	static struct policy p[MAX_POLICIES];
	int n;

	memset(t, 0, sizeof(*t));
	if (ncpus <= 0)
		return -1;
	t->ncpus = ncpus < (int)LERTICK_MAX_CPUS ? ncpus : (int)LERTICK_MAX_CPUS;
	memset(t->cpu_cluster, LERTICK_NO_CLUSTER, sizeof(t->cpu_cluster));
	if (!hw_root)
		hw_root = "";

	n = read_policies(hw_root, p, MAX_POLICIES);
	if (n > 0) {
		snprintf(t->source, sizeof(t->source), "hw");
		snprintf(t->root, sizeof(t->root), "%s", hw_root);
		assign_clusters(t, p, n);
		return 0;
	}

	if (!sim_root)
		sim_root = getenv(LTLW_SIM_ROOT_ENV);
	if (!sim_root || !*sim_root)
		sim_root = LTLW_SIM_DEFAULT_ROOT;
	if (sim_root_valid(sim_root)) {
		n = read_policies(sim_root, p, MAX_POLICIES);
		if (n > 0) {
			char path[PATH_MAX + 64], line[64];

			snprintf(t->source, sizeof(t->source), "sim");
			snprintf(t->root, sizeof(t->root), "%s", sim_root);
			assign_clusters(t, p, n);
			for (int cpu = 0; cpu < t->ncpus; cpu++) {
				unsigned long cap;
				char *end;

				snprintf(path, sizeof(path),
					 "%s/sys/devices/system/cpu/cpu%d/cpu_capacity", sim_root, cpu);
				if (!read_line(path, line, sizeof(line)))
					continue;
				cap = strtoul(line, &end, 10);
				if (end != line && cap >= 1 && cap <= LERTICK_CPUPERF_ONE)
					t->cpu_capacity[cpu] = (unsigned short)cap;
			}
			return 0;
		}
	}

	snprintf(t->source, sizeof(t->source), "single");
	t->nclusters = 1;
	for (int cpu = 0; cpu < t->ncpus; cpu++)
		t->cpu_cluster[cpu] = 0;
	t->cluster_ncpus[0] = t->ncpus;
	__u64 hash = lw_contract_topology_seed();
	for (int cpu = 0; cpu < t->ncpus; cpu++)
		hash = lw_contract_topology_add_cpu(hash, (__u32)cpu);
	t->cluster_topology_id[0] = lw_contract_topology_finish(hash, (__u32)t->ncpus);
	return 0;
}

bool ltlw_schedutil_in_use(const char *hw_root)
{
	char dir[PATH_MAX], path[PATH_MAX + 512], line[64];
	struct dirent *e;
	bool found = false;
	DIR *d;

	snprintf(dir, sizeof(dir), "%s/sys/devices/system/cpu/cpufreq", hw_root ? hw_root : "");
	d = opendir(dir);
	if (!d)
		return false;
	while (!found && (e = readdir(d))) {
		if (strncmp(e->d_name, "policy", 6))
			continue;
		snprintf(path, sizeof(path), "%s/%s/scaling_governor", dir, e->d_name);
		found = read_line(path, line, sizeof(line)) && !strcmp(line, "schedutil");
	}
	closedir(d);
	return found;
}

__u64 ltlw_hist_quantile_ns(const __u64 *counts, int n, unsigned int milli)
{
	__u64 total = 0, target, seen = 0;

	for (int b = 0; b < n; b++)
		total += counts[b];
	if (!total)
		return 0;
	if (milli > 1000)
		milli = 1000;
	target = (total * milli + 999) / 1000;
	if (!target)
		target = 1;
	for (int b = 0; b < n; b++) {
		seen += counts[b];
		if (seen >= target)
			return 1ULL << (b + 1 < 63 ? b + 1 : 63);
	}
	return 1ULL << (n < 63 ? n : 63);
}

void ltlw_demand_compute(const struct ltlw_topology *t, const struct lertick_cpu_state *cpus,
			 int ncpus, bool cpuperf_util, struct ltlw_demand_state *st,
			 __u64 now_ns, struct lt_demand_rec *out, bool *valid)
{
	__u64 dt = st->primed && now_ns > st->last_ns ? now_ns - st->last_ns : 0;

	if (ncpus > t->ncpus)
		ncpus = t->ncpus;
	for (int c = 0; c < LW_CONTRACT_MAX_CLUSTERS; c++) {
		__u64 run = 0, lat = 0, hist[LERTICK_WAKE_HIST_BUCKETS] = {0}, delta[LERTICK_WAKE_HIST_BUCKETS];
		__u64 util_sum = 0, perf_sum = 0;
		__u32 perf_max = 0;
		int members = 0;

		memset(&out[c], 0, sizeof(out[c]));
		valid[c] = false;
		if (c >= t->nclusters)
			continue;
		for (int cpu = 0; cpu < ncpus; cpu++) {
			const struct lertick_cpu_state *s = &cpus[cpu];
			__u32 util = s->util, util_cap = s->util_cap, perf;

			if (t->cpu_cluster[cpu] != c)
				continue;
			members++;
			if (!s->run_mark_ns) {
				__u64 elapsed = s->window_start_ns && now_ns > s->window_start_ns ?
					now_ns - s->window_start_ns : 0;
				__u32 now_util = lertick_util_blend(util,
					lertick_busy_frac(s->busy_ns, elapsed), elapsed,
					LERTICK_UTIL_TAU_NS);

				util_cap = util ? (__u32)((__u64)util_cap * now_util / util) : 0;
			}
			perf = !cpuperf_util || !s->perf_sent ? LERTICK_CPUPERF_ONE : s->perf_target;
			util_sum += util_cap;
			perf_sum += perf;
			if (perf > perf_max)
				perf_max = perf;
			run += s->run_ns;
			lat += s->latcrit_ns;
			for (int b = 0; b < LERTICK_WAKE_HIST_BUCKETS; b++)
				hist[b] += s->wake_hist[b];
		}
		if (!members)
			continue;
		valid[c] = true;
		out[c].version = LW_CONTRACT_VERSION;
		out[c].pred_util = (__u32)(util_sum / (__u64)members);
		out[c].cpuperf_max = perf_max;
		out[c].cpuperf_mean = (__u32)(perf_sum / (__u64)members);
		out[c].updated_ns = now_ns;
		out[c].topology_id = t->cluster_topology_id[c];
		out[c].session_id = t->session_id;
		if (dt && run >= st->run_ns[c] && lat >= st->latcrit_ns[c]) {
			__u64 drun = run - st->run_ns[c], dlat = lat - st->latcrit_ns[c];

			/* Below 1% of the cluster's capacity, a share is noise. */
			if (drun * 100 >= dt * (__u64)members && drun) {
				__u64 milli = dlat * 1000 / drun;

				out[c].lat_crit_milli = (__u32)(milli > 1000 ? 1000 : milli);
			}
			for (int b = 0; b < LERTICK_WAKE_HIST_BUCKETS; b++)
				delta[b] = hist[b] >= st->hist[c][b] ? hist[b] - st->hist[c][b] : 0;
			out[c].p99_wake_ns = ltlw_hist_quantile_ns(delta, LERTICK_WAKE_HIST_BUCKETS, 990);
		}
		st->run_ns[c] = run;
		st->latcrit_ns[c] = lat;
		memcpy(st->hist[c], hist, sizeof(hist));
	}
	st->last_ns = now_ns;
	st->primed = true;
}

void ltlw_publish(struct lt_demand_rec *slot, const struct lt_demand_rec *val)
{
	__u32 seq = __atomic_load_n(&slot->seq, __ATOMIC_RELAXED);

	if (seq & 1)
		seq++;	/* a previous writer died mid-update */
	__atomic_store_n(&slot->seq, seq + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	slot->version = LW_CONTRACT_VERSION;
	slot->pred_util = val->pred_util;
	slot->cpuperf_max = val->cpuperf_max;
	slot->cpuperf_mean = val->cpuperf_mean;
	slot->lat_crit_milli = val->lat_crit_milli;
	slot->p99_wake_ns = val->p99_wake_ns;
	slot->updated_ns = val->updated_ns;
	slot->reserved = 0;
	slot->topology_id = val->topology_id;
	slot->session_id = val->session_id;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&slot->seq, seq + 2, __ATOMIC_RELEASE);
}

static int pin_prepare(const char *dir, const char *const *pins, int npins,
		       char *err, size_t errlen, bool remove_stale)
{
	char parent[PATH_MAX];
	struct statfs fs;
	struct stat st;
	char *slash;
	size_t length;
	int fd;

	if (!dir || dir[0] != '/' || !pins || npins < 1 ||
	    !(length = strlen(dir)) || length >= sizeof(parent) || dir[length - 1] == '/') {
		snprintf(err, errlen, "invalid private BPF pin directory");
		return -1;
	}
	for (int i = 0; i < npins; i++) {
		const char *name;

		if (!pins[i] || strncmp(pins[i], dir, length) || pins[i][length] != '/' ||
		    !(name = pins[i] + length + 1)[0] || strchr(name, '/') ||
		    !strcmp(name, ".") || !strcmp(name, "..")) {
			snprintf(err, errlen, "pin must be a direct child of its private BPF directory");
			return -1;
		}
	}
	memcpy(parent, dir, length + 1);
	slash = strrchr(parent, '/');
	if (slash && slash != parent)
		*slash = 0;
	if (statfs(parent, &fs) || (unsigned long)fs.f_type != BPF_FS_MAGIC_VALUE) {
		snprintf(err, errlen, "%s is not a BPF filesystem; mount it with: "
			 "mount -t bpf bpf %s", parent, parent);
		return -1;
	}
	if (mkdir(dir, 0700) && errno != EEXIST) {
		snprintf(err, errlen, "cannot create %s: %s", dir, strerror(errno));
		return -1;
	}
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "cannot securely open %s: %s", dir, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
	    (st.st_mode & 0022) || fstatfs(fd, &fs) ||
	    (unsigned long)fs.f_type != BPF_FS_MAGIC_VALUE) {
		snprintf(err, errlen, "%s must be an owned, protected directory on bpffs", dir);
		close(fd);
		return -1;
	}
	if (remove_stale && fchmod(fd, 0700)) {
		snprintf(err, errlen, "cannot protect BPF pin directory %s: %s", dir, strerror(errno));
		close(fd);
		return -1;
	}
	for (int i = 0; i < npins; i++) {
		const char *name = pins[i] + length + 1;

		if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
			if (errno == ENOENT)
				continue;
		} else if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
			   (st.st_mode & 0022) || st.st_nlink != 1) {
			snprintf(err, errlen, "refusing unexpected stale pin %s", pins[i]);
			close(fd);
			return -1;
		} else if (!remove_stale) {
			snprintf(err, errlen, "automatic discovery keeps existing pin %s unchanged", pins[i]);
			close(fd);
			return -1;
		} else if (!unlinkat(fd, name, 0) || errno == ENOENT) {
			continue;
		}
		{
			snprintf(err, errlen, "cannot remove stale pin %s: %s", pins[i],
				 strerror(errno));
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}

int ltlw_pin_prepare(const char *dir, const char *const *pins, int npins,
		     char *err, size_t errlen)
{
	return pin_prepare(dir, pins, npins, err, errlen, true);
}

int ltlw_pin_prepare_auto(const char *dir, const char *const *pins, int npins,
			  char *err, size_t errlen)
{
	return pin_prepare(dir, pins, npins, err, errlen, false);
}
