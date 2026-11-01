/* SPDX-License-Identifier: MIT */
/*
 * Lerwatt <-> Lertick contract: the only state the two controllers share.
 *
 * Both maps are BPF_MAP_TYPE_ARRAY with BPF_F_MMAPABLE. Lertick creates them
 * with its BPF object and pins them under /sys/fs/bpf/lertick/ only when it is
 * started with --lerwatt; it unpins them when it exits. Key = cluster index,
 * where clusters are the cpufreq policies sorted by their first CPU (RK3588:
 * policy0 -> 0, policy4 -> 1, policy6 -> 2). Without real cpufreq policies
 * (a VM), Lertick uses the policies of a running lerwatt-sim and maps CPU N to
 * simulated CPU N.
 *
 * Every field has exactly one writer:
 *   lw_contract  - written by Lerwatt (userspace, through mmap), read by Lertick (BPF)
 *   lt_demand    - written by the Lertick loader (userspace, through mmap) from
 *                  per-CPU state its BPF program keeps; read by Lerwatt and lerwatt-sim
 *
 * Consistency: seqcount. The writer makes seq odd, writes the fields, then
 * makes seq even. A reader that sees an odd seq, or a different seq before and
 * after copying, keeps its previous copy. Readers never spin in BPF callbacks.
 *
 * This header is the canonical definition. Lertick vendors it unchanged.
 * Bump LW_CONTRACT_VERSION on any layout change.
 */
#ifndef LERWATT_CONTRACT_H
#define LERWATT_CONTRACT_H

#if defined(__VMLINUX_H__)
/* BPF side: vmlinux.h already provides __u32/__s32/__u64. */
#elif defined(__linux__)
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
#endif

#define LW_CONTRACT_VERSION	1
#define LW_CONTRACT_MAX_CLUSTERS	8
#define LW_CONTRACT_PIN_DIR	"/sys/fs/bpf/lertick"
#define LW_CONTRACT_PIN_LW	LW_CONTRACT_PIN_DIR "/lw_contract"
#define LW_CONTRACT_PIN_LT	LW_CONTRACT_PIN_DIR "/lt_demand"
#define LW_CAP_ONE		1024	/* SCX_CPUPERF_ONE / SCHED_CAPACITY_SCALE */
#define LW_MILLI_ONE		1000

/* Written by Lerwatt each control period (default 100 ms). 32 bytes. */
struct lw_contract_rec {
	__u32 seq;
	__u32 version;
	__u32 ceiling_cap;		/* highest perf Lertick may request, 0..1024 */
	__u32 floor_cap;		/* lowest perf while latency-critical work runs */
	__u32 energy_price_milli;	/* normalized price of energy, 0..1000 */
	__s32 thermal_headroom_mc;	/* soft limit minus zone temperature, millidegC */
	__u64 valid_until_ns;		/* CLOCK_MONOTONIC; ignore the record after this */
};

/*
 * Written by Lertick every 100 ms. 48 bytes. Capacity values use the scale of
 * scx_bpf_cpuperf_set(): utilization in units where 1024 is the fastest CPU.
 */
struct lt_demand_rec {
	__u32 seq;
	__u32 version;
	__u32 pred_util;		/* smoothed per-CPU utilization, cluster mean, 0..1024 */
	__u32 cpuperf_max;		/* max scx cpuperf target in the cluster, 0..1024 */
	__u32 cpuperf_mean;		/* mean scx cpuperf target in the cluster, 0..1024 */
	__u32 lat_crit_milli;		/* share of sched_ext runtime that is latency-critical, 0..1000 */
	__u64 p99_wake_ns;		/* p99 wake-up latency, upper edge of its log2 bucket */
	__u64 updated_ns;		/* CLOCK_MONOTONIC time of the last update */
	__u64 reserved;
};

#endif /* LERWATT_CONTRACT_H */
