# Candidate project comparison

> Historical Phase 0 planning snapshot. The user approved ScxSprout on 2026-10-09. Implementation and actual Linux validation now exist; see [Phase 1 results](../PHASE1_RESULTS.md), [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md). Statements below describe the original planning stage.

Source review date: **2026-10-09**. This is a Phase 0 planning assessment. No candidate is implemented or benchmarked in this repository. Scores, code budgets, and development estimates are engineering judgments, not measured results or established research novelty.

## Decision criteria

Every score uses **1 = least favorable, 5 = most favorable**. In particular, ease means lower implementation complexity, overhead means lower expected runtime cost, and maintenance means lower expected maintenance burden. Novelty scores reflect how much room the reviewed prior art appears to leave; they do not establish that an idea is new.

The weighted assessment prioritizes reproducible evidence and practical kernel integration. Weights sum to 100%:

| Criterion | Weight |
| --- | ---: |
| Technical novelty (N) | 5% |
| Real-world usefulness (U) | 10% |
| Kernel integration feasibility (I) | 15% |
| Implementation ease (E) | 10% |
| Low runtime overhead (O) | 15% |
| Reproducibility (R) | 25% |
| Low maintenance burden (M) | 10% |
| Academic research potential (A) | 5% |
| Community adoption potential (C) | 5% |

## Scored candidates

| Candidate | N | U | I | E | O | R | M | A | C | Total /45 | Weighted /5 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **A. Online CPU burst regression with bounded sched_ext slices** | 2 | 4 | 4 | 4 | 4 | 4 | 3 | 4 | 4 | **33** | **3.80** |
| B. Contextual bandit CPU placement | 3 | 4 | 4 | 2 | 3 | 2 | 2 | 5 | 4 | 29 | 2.95 |
| C. Learned DAMON/DAMOS reclaim quota and cold-age selection | 2 | 4 | 3 | 3 | 4 | 3 | 4 | 3 | 3 | 29 | 3.30 |
| D. Predictive page allocation or learned allocator | 3 | 3 | 1 | 1 | 2 | 2 | 1 | 4 | 2 | 19 | 1.90 |
| E. Joint CPU and memory resource coordination | 4 | 5 | 3 | 1 | 2 | 2 | 1 | 5 | 3 | 26 | 2.55 |
| **F. Learned cgroup-v2 proactive reclaim controller** | 2 | 4 | 5 | 4 | 5 | 2 | 5 | 2 | 3 | **32** | **3.65** |

The apparent 0.15-point gap between A and F is much smaller than the uncertainty in these estimates. The recommendation depends on explicit feasibility gates, not the appearance of numerical precision.

## Candidate assessments

### A. Online CPU burst regression with bounded sched_ext slices

Build a small sched_ext scheduler using a weighted virtual-time policy as the safety baseline. A bounded, fixed-point online linear model predicts a task's next completed CPU-service burst from a small set of historical features. The prediction changes only a clamped slice budget; actual service consumption determines fairness accounting. EWMA and fixed slices are separate baselines.

The kernel exposes programmable scheduling callbacks and a mechanism that returns tasks to its fair scheduler when the loaded policy exits or stalls. Upstream example schedulers provide a small starting point. These are verified interfaces, not a guarantee that our future policy will preserve latency, fairness, or performance. [Linux 6.12 sched_ext documentation](https://docs.kernel.org/6.12/scheduler/sched-ext.html), [upstream scheduler examples](https://github.com/torvalds/linux/blob/master/tools/sched_ext/README.md).

This space is already crowded. Meta describes an experiment predicting imminent CPU yield with a small neural network; LAVD and BPFLand already use observed task behavior to adapt decisions. A new regression model alone is weak novelty. The credible initial contribution would be a readable implementation, explicit training labels, measured numeric and runtime limits, and reproducible comparisons against non-ML policies. [sched_ext project overview](https://sched-ext.com/docs/OVERVIEW), [LAVD](https://github.com/sched-ext/scx/blob/main/scheds/rust/scx_lavd/README.md), [BPFLand](https://github.com/sched-ext/scx/blob/main/scheds/rust/scx_bpfland/README.md).

The most important gate is label correctness: preemption and slice expiry are censored observations, not completed bursts. Accumulate service across those events, confirm a genuine sleep boundary using the pinned kernel's callback/dequeue semantics, exclude exits, and count incomplete samples. CPU-bound tasks may supply too few completed samples. If reliable service accounting or useful sample coverage cannot be demonstrated, this candidate does not pass Phase 1. Expected core size: **1,450–2,400 C/eBPF LOC** for a deliberately limited scheduler; topology optimization and complete cgroup-controller semantics are outside this estimate.

### B. Contextual bandit CPU placement

Choose among a bounded set of eligible CPU domains using workload history, local queue pressure, and cache/topology context. The learning objective might combine runnable wait with migration cost. The scheduler must respect affinity and retain a deterministic safe selection path.

sched_ext provides CPU selection and dispatch interfaces, but choosing a CPU at wakeup is only a hint; the eventual dispatch can differ. Reward attribution must therefore use the CPU actually used, not merely the CPU returned from `select_cpu`. [Current kernel scheduling-cycle documentation](https://docs.kernel.org/scheduler/sched-ext.html).

Placement is attractive research but a weak first small project: cache effects, queue contention, topology, and exploration interact; a QEMU vCPU topology does not reproduce a physical LLC or heterogeneous core hierarchy. A three-action bandit can fit in 1,000–3,000 lines, but a credible placement scheduler with topology, lifecycle, and fairness handling probably exceeds that budget. Expected bounded prototype: **2,000–3,000 LOC**, with much greater validation effort than A. Contextual bandits are established algorithms; applying one here is not evidence of novelty. [Original contextual-bandit/LinUCB paper](https://arxiv.org/abs/1003.0146).

### C. Learned DAMON/DAMOS reclaim quota and cold-age selection

Run a slow userspace learner that selects from bounded reclaim configurations using DAMON access statistics and feedback. Keep its decisions out of allocation fast paths. A single-process virtual-address prototype is plausible in **1,500–2,800 core C LOC**; a robust multi-cgroup physical-address backend needs substantially more validation and interface handling.

DAMOS already has quotas, watermarks, filters, and automatic quota tuning, including PSI feedback. Merely reading PSI and increasing/decreasing reclaim aggressiveness duplicates existing functionality. Linux 6.12's memcg and anonymous-page filters apply to the physical-address operations set, which complicates a seemingly simple cgroup target. [Linux 6.12 DAMON design](https://docs.kernel.org/6.12/mm/damon/design.html), [DAMON sysfs usage](https://docs.kernel.org/6.12/admin-guide/mm/damon/usage.html), [existing DAMO userspace tooling](https://github.com/damonitor/damo).

The potential gap is whether learned selection across several configurations improves a workload-level objective beyond the native tuner, not whether reclaim can be adaptive. Action feedback and harm are delayed; applied bytes are not proof of retained memory savings. DAMOS is a persistent kernel control loop: userspace death does not imply that its context is switched off. Disable the owned context explicitly, restore captured settings, and verify its state. For those reasons, DAMOS pageout is deferred beyond the first memory prototype.

### D. Predictive allocation or learned allocator

Predict allocation demand, object lifetime, or page destination. A userspace allocator experiment and a kernel page-allocation policy are different projects and should not share claims of Linux-kernel integration.

The reviewed kernel interfaces do not offer a sched_ext-like general eBPF replacement point for the page allocator. Tracing allocations is observation, not authority to replace allocation decisions. A no-patch kernel allocator therefore has a poor fit with this project's initial constraints. [Kernel memory-management APIs](https://docs.kernel.org/core-api/mm-api.html).

Research has explored reinforcement learning for hybrid-memory allocation, including simulator-based approaches; those results do not establish deployable upstream-Linux support. [PARL research paper](https://www.sciencedirect.com/science/article/pii/S1383762124002479), [Berkeley learned-allocation project report](https://people.eecs.berkeley.edu/~kubitron/courses/cs262a-F21/projects/reports/project5_report_ver4.pdf). A narrow predictor might be 1,000–3,000 lines, but the allocator replacement, correctness work, and application compatibility would not credibly fit the same budget.

### E. Joint CPU and memory coordination

Choose CPU slices, placement, and memory reclaim as a joint controller. This could address contention that separate controllers miss, but it multiplies actions, timescales, attribution problems, and safety responsibilities.

CPU and memory PSI are available system-wide and per cgroup; they are useful measurements, not a causal description of which policy produced each stall or an application-level latency guarantee. [PSI documentation](https://docs.kernel.org/accounting/psi.html). Existing Linux reclaim also contains feedback mechanisms; the project should treat MGLRU as a baseline rather than rename its adaptation as ML. [MGLRU design](https://docs.kernel.org/mm/multigen_lru.html).

Expected production-shaped core: **more than 3,000 LOC** even before broad topology support. Defer until one resource controller has passed validation and a measured interaction motivates combining them.

### F. Learned cgroup-v2 proactive reclaim controller

Use a small userspace C controller to select a no-op or bounded one-shot reclaim request for one isolated, explicitly opted-in cgroup. Read PSI, refault deltas, memory composition, and workload outcomes. Keep the kernel's native page selection unchanged. Optional DAMON diagnostics come later and begin in read-only mode.

This is the most practical memory alternative because `memory.reclaim` submits a one-shot request without leaving a lowered memory limit behind. Meta's TMO/Senpai already demonstrates PSI-guided proactive offloading; a fixed proportional controller is therefore a mandatory competitor, not a novel feature. [Linux 6.12 cgroup v2 interface](https://docs.kernel.org/6.12/admin-guide/cgroup-v2.html), [Meta TMO engineering description](https://engineering.fb.com/2022/06/20/data-infrastructure/transparent-memory-offloading-more-memory-at-a-fraction-of-the-cost-and-power/), [Senpai source](https://github.com/facebookincubator/senpai).

Expected core: **1,200–2,300 userspace C LOC**, no mandatory eBPF or ML framework. Genuine ML would use context-conditioned reward models updated from each observed outcome, with bounded exploration; plain thresholds and EWMA remain baselines. Bounds apply to requested bytes and submission rate, not actual reclaimed bytes or kernel execution time. The principal uncertainty is reward attribution: `memory.current` changes include application allocations/frees, requests can under/over-reclaim, and refaults can appear after the measurement window. Newer kernels expose proactive-specific counters, but the Linux 6.12 documentation does not expose `pgsteal_proactive`; feature-detect rather than infer support from a version string. Stopping submissions prevents future controller actions; an in-flight kernel reclaim may finish and already evicted pages are not instantly restored.

## Recommendation and sensitivity

**Primary: A, conditional on the completed-burst accounting and numeric-safety gates. Backup: F.** This selection follows an assessment of memory options, not a presumption that CPU scheduling must win. A exposes a more directly testable prediction target and transparent scheduler fallback, while F has lower maintenance and broader stock-kernel deployment prospects. Both require evidence before performance claims.

If lower maintenance becomes more important than reproducibility, shift 10 percentage points from R to M: A becomes **3.70**, F becomes **3.95**, and the memory controller is preferred. If A cannot produce sufficiently reliable and representative labels, switch to F after agreeing on a reward definition and evaluation window. If F's reward cannot be distinguished from background workload changes in controlled trials, keep it as an observability/shadow-mode experiment rather than advertise an adaptive reclamation product.

Read [RECOMMENDATION.md](RECOMMENDATION.md), [ARCHITECTURE.md](ARCHITECTURE.md), [BENCHMARK_PLAN.md](BENCHMARK_PLAN.md), and [ROADMAP.md](ROADMAP.md) for the decision gates and implementation scope. Neither recommendation authorizes implementation in this Phase 0 repository.
