/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Userspace half of the optional Lerwatt plug-in: cluster discovery, demand
 * aggregation and the seqcount writer for lt_demand. Nothing here uses libbpf,
 * so tests/test_lerwatt_plugin.c exercises it without a kernel.
 */
#ifndef LERTICK_LERWATT_PLUGIN_H
#define LERTICK_LERWATT_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include "lertick.h"
#include "lerwatt/contract.h"

#define LTLW_SIM_MARKER ".lerwatt-sim"
#define LTLW_SIM_DEFAULT_ROOT "/run/lerwatt-sim"
#define LTLW_SIM_ROOT_ENV "LERWATT_SIM_ROOT"
#define LTLW_PUBLISH_PERIOD_NS LW_CONTRACT_PUBLISH_NS

struct ltlw_topology {
	int ncpus;					/* CPUs Lertick schedules */
	int nclusters;
	int npolicies;					/* cpufreq policies found */
	unsigned char cpu_cluster[LERTICK_MAX_CPUS];	/* LERTICK_NO_CLUSTER: none */
	unsigned short cpu_capacity[LERTICK_MAX_CPUS];	/* 0: the kernel's value */
	int cluster_ncpus[LW_CONTRACT_MAX_CLUSTERS];
	__u64 cluster_topology_id[LW_CONTRACT_MAX_CLUSTERS]; /* full related CPU sets */
	__u64 session_id;				/* random nonzero loader attachment */
	char source[8];					/* "hw", "sim" or "single" */
	char root[256];					/* sysfs root used; "" is / */
};

/* Parse "0 1 2 3", "0-3" or "0-3,6"; sets mask[cpu] for cpu < max. */
int ltlw_parse_cpulist(const char *text, unsigned char *mask, int max);

/*
 * Same order as Lerwatt's own detection: real cpufreq policies under hw_root
 * first; otherwise a running lerwatt-sim (sim_root, else $LERWATT_SIM_ROOT,
 * else /run/lerwatt-sim, valid only with its marker file), whose CPU N stands
 * for this machine's CPU N and whose cpu_capacity values replace the kernel's;
 * otherwise one cluster of all CPUs. Clusters are policies sorted by first CPU.
 */
int ltlw_discover(struct ltlw_topology *t, int ncpus, const char *hw_root,
		  const char *sim_root);
bool ltlw_schedutil_in_use(const char *hw_root);

struct ltlw_demand_state {
	bool primed;
	__u64 last_ns;
	__u64 run_ns[LW_CONTRACT_MAX_CLUSTERS];
	__u64 latcrit_ns[LW_CONTRACT_MAX_CLUSTERS];
	__u64 hist[LW_CONTRACT_MAX_CLUSTERS][LERTICK_WAKE_HIST_BUCKETS];
};

/*
 * Build one record per cluster from per-CPU state (cpus[i] is CPU i). Idle
 * CPUs' utilization is decayed to now with the same moving average the BPF
 * program uses. cpuperf_util=false reports the kernel default target, 1024.
 * valid[c] is false for a cluster with none of this machine's CPUs.
 */
void ltlw_demand_compute(const struct ltlw_topology *t, const struct lertick_cpu_state *cpus,
			 int ncpus, bool cpuperf_util, struct ltlw_demand_state *st,
			 __u64 now_ns, struct lt_demand_rec *out, bool *valid);

/* Upper edge (ns) of the log2 bucket holding the milli/1000 quantile; 0 if empty. */
__u64 ltlw_hist_quantile_ns(const __u64 *counts, int n, unsigned int milli);

/* Seqcount writer, as in Lerwatt's lw_contract_publish(). */
void ltlw_publish(struct lt_demand_rec *slot, const struct lt_demand_rec *val);

/*
 * Create dir on bpffs and remove stale pins left by a Lertick that was killed.
 * The caller has already verified that no sched_ext scheduler is running.
 */
int ltlw_pin_prepare(const char *dir, const char *const *pins, int npins,
		     char *err, size_t errlen);
/* Best-effort discovery never unlinks or changes permissions on existing pins. */
int ltlw_pin_prepare_auto(const char *dir, const char *const *pins, int npins,
			  char *err, size_t errlen);

#endif /* LERTICK_LERWATT_PLUGIN_H */
