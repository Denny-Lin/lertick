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
 * Bump LW_CONTRACT_VERSION on any layout or field-meaning change. Models,
 * training state and release versions are private to their own controller.
 * Lertick owns scheduling and CPU performance requests; Lerwatt owns CPUFreq
 * min/max and PM QoS constraints. Neither starts, stops or trains its peer.
 * The contract is optional advice, never a thermal-protection interlock.
 *
 * ABI 2 binds each record to the full sorted policy CPU set (topology_id) and
 * the current Lertick attachment (random nonzero session_id). Lerwatt echoes
 * those identities only after a compatible fresh demand record. A missing,
 * old, incompatible or differently identified peer means independent control.
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

#define LW_CONTRACT_VERSION	2
#define LW_CONTRACT_MAX_CLUSTERS	8
#define LW_CONTRACT_PIN_DIR	"/sys/fs/bpf/lertick"
#define LW_CONTRACT_PIN_LW	LW_CONTRACT_PIN_DIR "/lw_contract"
#define LW_CONTRACT_PIN_LT	LW_CONTRACT_PIN_DIR "/lt_demand"
#define LW_CAP_ONE		1024	/* SCX_CPUPERF_ONE / SCHED_CAPACITY_SCALE */
#define LW_MILLI_ONE		1000
#define LW_CONTRACT_PUBLISH_NS		100000000ULL
#define LW_CONTRACT_DEMAND_MAX_AGE_NS	500000000ULL
#define LW_CONTRACT_TTL_NS		300000000ULL
#define LW_CONTRACT_RETRY_NS		1000000000ULL
#define LW_CONTRACT_STALE_NS		2000000000ULL

/* Written by Lerwatt each control period (default 100 ms). 48 bytes. */
struct lw_contract_rec {
	__u32 seq;
	__u32 version;
	__u32 ceiling_cap;		/* highest perf request, 1..1024; 0 means no cap */
	__u32 floor_cap;		/* lowest perf while latency-critical work runs */
	__u32 energy_price_milli;	/* normalized price of energy, 0..1000 */
	__s32 thermal_headroom_mc;	/* soft limit minus zone temperature, millidegC */
	__u64 valid_until_ns;		/* CLOCK_MONOTONIC; ignore the record after this */
	__u64 topology_id;		/* full policy CPU set, hashed as below */
	__u64 session_id;		/* echoes the current Lertick demand identity */
};

/*
 * Written by Lertick every 100 ms. 64 bytes. Capacity values use the scale of
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
	__u64 topology_id;
	__u64 session_id;
};

/*
 * FNV-1a over a domain tag, each sorted CPU ID as four little-endian bytes,
 * then the CPU count. Hash the full related_cpus set, including CPUs not
 * present in a simulated guest. This is an identity check, not authentication.
 */
static inline __u64 lw_contract_topology_seed(void)
{
	return 0xf3fb15f721510cdfULL; /* FNV-1a of the ASCII domain "LWTOP2" */
}

static inline __u64 lw_contract_topology_add_cpu(__u64 hash, __u32 cpu)
{
	for (int byte = 0; byte < 4; byte++) {
		hash ^= (cpu >> (byte * 8)) & 255U;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static inline __u64 lw_contract_topology_finish(__u64 hash, __u32 ncpus)
{
	hash = lw_contract_topology_add_cpu(hash, ncpus);
	return ncpus ? (hash ? hash : 1) : 0;
}

/* Shared by BPF and portable conformance tests; zero ceiling means no cap. */
static inline int lw_contract_constraint_valid(const struct lw_contract_rec *r,
					      __u64 topology_id, __u64 session_id,
					      __u64 now_ns)
{
	return r->version == LW_CONTRACT_VERSION && topology_id && session_id &&
		r->topology_id == topology_id && r->session_id == session_id &&
		r->valid_until_ns >= now_ns &&
		r->valid_until_ns - now_ns <= LW_CONTRACT_TTL_NS &&
		r->ceiling_cap <= LW_CAP_ONE && r->floor_cap <= LW_CAP_ONE &&
		(!r->ceiling_cap || r->floor_cap <= r->ceiling_cap) &&
		r->energy_price_milli <= LW_MILLI_ONE;
}

#if !defined(__VMLINUX_H__)
_Static_assert(sizeof(struct lw_contract_rec) == 48, "lw_contract ABI size");
_Static_assert(sizeof(struct lt_demand_rec) == 64, "lt_demand ABI size");
_Static_assert(__builtin_offsetof(struct lw_contract_rec, topology_id) == 32,
	       "lw_contract identity offset");
_Static_assert(__builtin_offsetof(struct lt_demand_rec, topology_id) == 48,
	       "lt_demand identity offset");
#endif

#endif /* LERWATT_CONTRACT_H */
