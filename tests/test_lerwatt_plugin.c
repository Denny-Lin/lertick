/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Portable tests for the userspace half of the Lerwatt plug-in: cluster
 * discovery from fake sysfs trees, demand aggregation, the seqcount writer read
 * back with Lerwatt's reader rule, and the bpffs check. No kernel is needed.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "lerwatt_plugin.h"

static char base[256];

static void put(const char *root, const char *rel, const char *text)
{
	char path[1024], dir[1024], *p;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", root, rel);
	snprintf(dir, sizeof(dir), "%s", path);
	if (mkdir(root, 0755) && errno != EEXIST)
		abort();
	for (p = dir + strlen(root) + 1; (p = strchr(p, '/')); p++) {
		*p = 0;
		if (mkdir(dir, 0755) && errno != EEXIST)
			abort();
		*p = '/';
	}
	f = fopen(path, "w");
	assert(f);
	fputs(text, f);
	fclose(f);
}

static void test_cpulist(void)
{
	unsigned char mask[16];
	char extreme[80];

	memset(mask, 0, sizeof(mask));
	assert(ltlw_parse_cpulist("0 1 2 3\n", mask, 16) == 4);
	assert(mask[0] && mask[3] && !mask[4]);
	memset(mask, 0, sizeof(mask));
	assert(ltlw_parse_cpulist("0-3,6,8-9", mask, 16) == 7);
	assert(mask[2] && mask[6] && mask[9] && !mask[7]);
	memset(mask, 0, sizeof(mask));
	assert(ltlw_parse_cpulist("12-20", mask, 16) == 4);
	assert(ltlw_parse_cpulist("a", mask, 16) == -1);
	assert(ltlw_parse_cpulist("3-1", mask, 16) == -1);
	assert(ltlw_parse_cpulist("1x", mask, 16) == -1);
	memset(mask, 0, sizeof(mask));
	snprintf(extreme, sizeof(extreme), "0-%ld", LONG_MAX);
	assert(ltlw_parse_cpulist(extreme, mask, 16) == 16);
	assert(ltlw_parse_cpulist("0-92233720368547758070", mask, 16) == -1);
}

static void make_board(const char *root, bool sim)
{
	const char *pol = "sys/devices/system/cpu/cpufreq";
	char rel[256];

	/* Created out of order: clusters must follow the first CPU. */
	snprintf(rel, sizeof(rel), "%s/policy6/related_cpus", pol);
	put(root, rel, "6 7\n");
	snprintf(rel, sizeof(rel), "%s/policy0/related_cpus", pol);
	put(root, rel, "0 1 2 3\n");
	snprintf(rel, sizeof(rel), "%s/policy4/related_cpus", pol);
	put(root, rel, "4 5\n");
	snprintf(rel, sizeof(rel), "%s/policy4/scaling_governor", pol);
	put(root, rel, sim ? "schedutil\n" : "performance\n");
	for (int cpu = 0; cpu < 8; cpu++) {
		snprintf(rel, sizeof(rel), "sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
		put(root, rel, cpu < 4 ? "397\n" : "1024\n");
	}
	if (sim)
		put(root, LTLW_SIM_MARKER, "");
}

static void test_discover(void)
{
	char hw[300], empty[300], sim[300], nomark[300];
	struct ltlw_topology t;
	__u64 hw_ids[LW_CONTRACT_MAX_CLUSTERS];

	snprintf(hw, sizeof(hw), "%s/hw", base);
	snprintf(empty, sizeof(empty), "%s/empty", base);
	snprintf(sim, sizeof(sim), "%s/sim", base);
	snprintf(nomark, sizeof(nomark), "%s/nomark", base);
	mkdir(empty, 0755);
	make_board(hw, false);
	make_board(sim, true);
	make_board(nomark, false);

	assert(!ltlw_discover(&t, 8, hw, sim));
	assert(!strcmp(t.source, "hw") && t.nclusters == 3 && t.npolicies == 3);
	assert(t.cpu_cluster[0] == 0 && t.cpu_cluster[3] == 0);
	assert(t.cpu_cluster[4] == 1 && t.cpu_cluster[5] == 1);
	assert(t.cpu_cluster[6] == 2 && t.cpu_cluster[7] == 2);
	assert(t.cluster_ncpus[0] == 4 && t.cluster_ncpus[1] == 2 && t.cluster_ncpus[2] == 2);
	assert(t.cluster_topology_id[0] && t.cluster_topology_id[1] && t.cluster_topology_id[2]);
	assert(t.cluster_topology_id[0] != t.cluster_topology_id[1]);
	assert(t.cluster_topology_id[1] != t.cluster_topology_id[2]);
	memcpy(hw_ids, t.cluster_topology_id, sizeof(hw_ids));
	/* Real hardware: the kernel's capacities stand. */
	assert(t.cpu_capacity[0] == 0 && t.cpu_capacity[7] == 0);
	assert(!ltlw_schedutil_in_use(hw));

	/* A 4-CPU VM with a simulated 8-CPU board: CPU N stands for simulated CPU N. */
	assert(!ltlw_discover(&t, 4, empty, sim));
	assert(!strcmp(t.source, "sim") && t.nclusters == 3);
	assert(t.cluster_ncpus[0] == 4 && t.cluster_ncpus[1] == 0 && t.cluster_ncpus[2] == 0);
	/* Absent guest CPUs do not turn a physical policy into a different ID. */
	assert(!memcmp(hw_ids, t.cluster_topology_id, sizeof(hw_ids)));
	assert(t.cpu_capacity[0] == 397 && t.cpu_capacity[3] == 397);
	assert(ltlw_schedutil_in_use(sim));

	/* Without the marker, a tree is not a running simulator. */
	assert(!ltlw_discover(&t, 4, empty, nomark));
	assert(!strcmp(t.source, "single") && t.nclusters == 1 && t.cluster_ncpus[0] == 4);
	assert(t.cpu_cluster[3] == 0 && t.cpu_cluster[4] == LERTICK_NO_CLUSTER);
	assert(t.cluster_topology_id[0] == hw_ids[0]);
}

static void test_quantile(void)
{
	__u64 h[LERTICK_WAKE_HIST_BUCKETS] = {0};

	assert(ltlw_hist_quantile_ns(h, LERTICK_WAKE_HIST_BUCKETS, 990) == 0);
	h[10] = 990;	/* ~1 us */
	h[20] = 10;	/* ~1 ms */
	assert(ltlw_hist_quantile_ns(h, LERTICK_WAKE_HIST_BUCKETS, 990) == 1ULL << 11);
	h[20] = 11;
	assert(ltlw_hist_quantile_ns(h, LERTICK_WAKE_HIST_BUCKETS, 990) == 1ULL << 21);
	assert(ltlw_hist_quantile_ns(h, LERTICK_WAKE_HIST_BUCKETS, 500) == 1ULL << 11);
}

static void test_demand(void)
{
	struct lertick_cpu_state cpus[8];
	struct lt_demand_rec out[LW_CONTRACT_MAX_CLUSTERS];
	bool valid[LW_CONTRACT_MAX_CLUSTERS];
	struct ltlw_demand_state st = { 0 };
	struct ltlw_topology t;
	char hw[300];
	__u64 now = 10000000000ULL;

	snprintf(hw, sizeof(hw), "%s/hw", base);
	assert(!ltlw_discover(&t, 8, hw, NULL));
	t.session_id = 0x7ad1809342ULL;
	memset(cpus, 0, sizeof(cpus));
	/* Cluster 0: two busy CPUs with targets 300 and 700, two never used. */
	cpus[0] = (struct lertick_cpu_state){ .run_mark_ns = now - 1000, .util = 600,
		.util_cap = 240, .perf_target = 300, .perf_sent = 1 };
	cpus[1] = (struct lertick_cpu_state){ .run_mark_ns = now - 1000, .util = 900,
		.util_cap = 560, .perf_target = 700, .perf_sent = 1 };
	cpus[2].perf_sent = 1;
	cpus[3].perf_sent = 1;
	/* Cluster 1: CPU 4 idle for a full time constant; CPU 5 never sent a target. */
	cpus[4] = (struct lertick_cpu_state){ .window_start_ns = now - LERTICK_UTIL_TAU_NS,
		.util = 800, .util_cap = 800, .perf_target = 640, .perf_sent = 1 };

	ltlw_demand_compute(&t, cpus, 8, true, &st, now, out, valid);
	assert(valid[0] && valid[1] && valid[2] && !valid[3]);
	assert(out[0].cpuperf_max == 700 && out[0].cpuperf_mean == 250);
	assert(out[0].pred_util == 200);
	assert(out[0].version == LW_CONTRACT_VERSION && out[0].updated_ns == now);
	assert(out[0].session_id == t.session_id);
	assert(out[0].topology_id == t.cluster_topology_id[0]);
	assert(out[0].lat_crit_milli == 0 && out[0].p99_wake_ns == 0);
	/* An idle CPU's utilization decays; an unset target is the kernel's 1024. */
	assert(out[1].pred_util == 0 && out[1].cpuperf_max == 1024);

	/* Without performance targets, schedutil sees the kernel default. */
	ltlw_demand_compute(&t, cpus, 8, false, &(struct ltlw_demand_state){ 0 }, now, out, valid);
	assert(out[0].cpuperf_max == 1024 && out[0].cpuperf_mean == 1024);

	/* 100 ms later: 80 ms of runtime on cluster 2, a quarter latency-critical. */
	cpus[6].run_ns = 80000000;
	cpus[6].latcrit_ns = 20000000;
	cpus[6].wake_hist[12] = 99;
	cpus[7].wake_hist[16] = 1;
	cpus[0].run_ns = 100000;	/* cluster 0: below 1% of 4 CPUs x 100 ms */
	cpus[0].latcrit_ns = 100000;
	ltlw_demand_compute(&t, cpus, 8, true, &st, now + 100000000, out, valid);
	assert(out[2].lat_crit_milli == 250);
	assert(out[2].p99_wake_ns == 1ULL << 13);
	assert(out[0].lat_crit_milli == 0);
	/* Counters are differenced: no new runtime, no share. */
	ltlw_demand_compute(&t, cpus, 8, true, &st, now + 200000000, out, valid);
	assert(out[2].lat_crit_milli == 0 && out[2].p99_wake_ns == 0);
}

/* Lerwatt's reader rule (lerwatt/src/contract.c): even, unchanged seq. */
static bool read_like_lerwatt(const struct lt_demand_rec *slot, struct lt_demand_rec *copy)
{
	__u32 s1 = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE), s2;

	if (!s1 || (s1 & 1))
		return false;
	*copy = *slot;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	s2 = __atomic_load_n(&slot->seq, __ATOMIC_RELAXED);
	return s1 == s2 && copy->version == LW_CONTRACT_VERSION;
}

static void test_publish(void)
{
	struct lt_demand_rec slot = { 0 }, copy, val = { .pred_util = 123, .cpuperf_max = 456,
		.cpuperf_mean = 321, .lat_crit_milli = 250, .p99_wake_ns = 8192, .updated_ns = 77,
		.topology_id = 0xa781b921, .session_id = 0x3aef412331ULL };

	assert(!read_like_lerwatt(&slot, &copy));	/* never written: version 0 */
	ltlw_publish(&slot, &val);
	assert(slot.seq == 2);
	assert(read_like_lerwatt(&slot, &copy));
	assert(copy.pred_util == 123 && copy.cpuperf_max == 456 && copy.cpuperf_mean == 321);
	assert(copy.lat_crit_milli == 250 && copy.p99_wake_ns == 8192 && copy.updated_ns == 77);
	assert(copy.topology_id == val.topology_id && copy.session_id == val.session_id);
	slot.seq = 5;	/* a writer died mid-update */
	assert(!read_like_lerwatt(&slot, &copy));
	ltlw_publish(&slot, &val);
	assert(slot.seq == 8 && read_like_lerwatt(&slot, &copy));
}

static void test_contract_identity(void)
{
	const __u64 now = 10000000000ULL, session = 23;
	__u64 hash = lw_contract_topology_seed(), other;
	struct lw_contract_rec r;

	for (__u32 cpu = 0; cpu < 4; cpu++)
		hash = lw_contract_topology_add_cpu(hash, cpu);
	hash = lw_contract_topology_finish(hash, 4);
	assert(hash == 0x8d5e629bcc626c8bULL); /* frozen cross-repository test vector */
	assert(!lw_contract_topology_finish(lw_contract_topology_seed(), 0));
	other = lw_contract_topology_add_cpu(lw_contract_topology_seed(), 4);
	other = lw_contract_topology_finish(other, 1);
	assert(hash != other);
	r = (struct lw_contract_rec){ .version = LW_CONTRACT_VERSION,
		.ceiling_cap = 800, .floor_cap = 200, .energy_price_milli = 500,
		.valid_until_ns = now + LW_CONTRACT_TTL_NS,
		.topology_id = hash, .session_id = session };
	assert(lw_contract_constraint_valid(&r, hash, session, now));
	/* Same policy after a Lertick restart must reject the old session. */
	assert(!lw_contract_constraint_valid(&r, hash, session + 1, now));
	assert(!lw_contract_constraint_valid(&r, other, session, now));
	assert(!lw_contract_constraint_valid(&r, 0, session, now));
	assert(!lw_contract_constraint_valid(&r, hash, 0, now));
	r.version = LW_CONTRACT_VERSION - 1;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
	r.version = LW_CONTRACT_VERSION;
	r.valid_until_ns = now - 1;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
	r.valid_until_ns = now + LW_CONTRACT_TTL_NS + 1;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
	r.valid_until_ns = ~(__u64)0;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
	r.valid_until_ns = now;
	assert(lw_contract_constraint_valid(&r, hash, session, now));
	r.floor_cap = 801;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
	r.ceiling_cap = 0; /* explicit no-cap value still supports a floor */
	assert(lw_contract_constraint_valid(&r, hash, session, now));
	r.floor_cap = LW_CAP_ONE + 1;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
	r.floor_cap = 0;
	r.ceiling_cap = LW_CAP_ONE + 1;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
	r.ceiling_cap = LW_CAP_ONE;
	r.energy_price_milli = LW_MILLI_ONE + 1;
	assert(!lw_contract_constraint_valid(&r, hash, session, now));
}

static void test_pin_prepare(void)
{
	char dir[300], pin[350], err[512];
	const char *pins[] = { pin };

	snprintf(dir, sizeof(dir), "%s/notbpf/lertick", base);
	snprintf(pin, sizeof(pin), "%s/map", dir);
	mkdir(dir, 0755);
	assert(ltlw_pin_prepare(dir, pins, 1, err, sizeof(err)) == -1);
	assert(strstr(err, "mount -t bpf"));
	/* Reject paths outside the protected directory before filesystem access. */
	pins[0] = "/nonexistent/a";
	assert(ltlw_pin_prepare(dir, pins, 1, err, sizeof(err)) == -1);
	assert(strstr(err, "direct child"));
	snprintf(pin, sizeof(pin), "%s/../other", dir);
	pins[0] = pin;
	assert(ltlw_pin_prepare(dir, pins, 1, err, sizeof(err)) == -1);
	assert(strstr(err, "direct child"));
	snprintf(pin, sizeof(pin), "%s/..", dir);
	assert(ltlw_pin_prepare(dir, pins, 1, err, sizeof(err)) == -1);
	assert(strstr(err, "direct child"));
}

int main(void)
{
	snprintf(base, sizeof(base), "%s/lertick-lw-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	assert(mkdtemp(base));
	test_cpulist();
	test_discover();
	test_quantile();
	test_demand();
	test_publish();
	test_contract_identity();
	test_pin_prepare();
	printf("{\"test\":\"lerwatt_plugin\",\"status\":\"passed\"}\n");
	return 0;
}
