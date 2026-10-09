# Research and feasibility assessment

> Historical Phase 0 planning snapshot. The user approved ScxSprout on 2026-10-09. Implementation and actual Linux validation now exist; see [Phase 1 results](../PHASE1_RESULTS.md), [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md). Statements below describe the original planning stage.

Status: **Phase 0 research; implementation direction awaits review and approval.**

Sources checked: **2026-10-09**. Proposed project name: **ScxSprout** (`scx-sprout`).

This repository has no scheduler, measured performance result, or established research novelty. The survey below supports a proposed experiment; it does not establish that Linux needs an ML replacement.

## Evidence labels and verification method

- **Verified fact** means a claim appears in the linked primary documentation, source repository, author paper, or publisher abstract. It does not mean we reproduced the result.
- **Engineering estimate** means a proposed scope, cost, or technical judgment based on those facts.
- **Untested hypothesis** means a question that needs implementation and measurements.

We inspected kernel documentation, project READMEs, conference slides, paper metadata, and selected paper sections through web access. We did not build, clone and audit complete implementations, install packages, boot a Linux VM, execute BPF, or reproduce any benchmarks. Search coverage was targeted, not a systematic literature review. Current branch URLs can change; implementation work must pin source commits and kernel versions. Publication dates came from source metadata rather than search crawl timestamps. Preprints and presentations are identified separately from conference papers.

## What already exists

**Verified facts.** Linux `sched_ext` supports BPF-defined scheduling and has been upstream since Linux 6.12. It provides runtime loading/unloading and fallback on detected scheduler errors or stalled runnable tasks. The kernel must enable the required sched_ext, BPF, JIT, and BTF configuration; a version number alone is insufficient. Its BPF interfaces are not stable across kernel versions. See the [versioned 6.12 documentation](https://docs.kernel.org/6.12/scheduler/sched-ext.html) and [current documentation](https://docs.kernel.org/scheduler/sched-ext.html).

The [sched-ext/scx project](https://github.com/sched-ext/scx) already offers mature scheduler policies, hybrid BPF/userspace designs, telemetry, and deployment tooling. Its C examples now have a [separate repository](https://github.com/sched-ext/scx-c-examples), which identifies upstream `tools/sched_ext` as their definitive home. The [kernel examples](https://github.com/torvalds/linux/tree/master/tools/sched_ext) are the appropriate starting point for a pinned C implementation.

ML scheduling is existing work: Meta reports a small neural yield predictor in the [scx overview](https://github.com/sched-ext/scx/blob/main/OVERVIEW.md); a [2025 ML load-balancing presentation](https://static.sched.com/hosted_files/ossna2025/d2/Improve-Load-Balancing-With-Machine-Learning-Techniques-based-on-sched_ext.pdf) describes userspace retraining and BPF inference. [KML](https://github.com/sbu-fsl/kernel-ml) supports kernel ML training and inference but requires kernel modifications. These sources rule out broad claims such as “first learning Linux scheduler” or “first fully in-kernel learning system.”

Memory management is also adaptive today. [DAMON/DAMOS](https://docs.kernel.org/mm/damon/design.html) provides access monitoring and configurable access-aware actions; [MGLRU](https://docs.kernel.org/mm/multigen_lru.html) uses recency generations and refault feedback. [TMO](https://www.cs.cmu.edu/~dskarlat/publications/tmo_asplos22.pdf) already applies PSI-guided proactive reclaim through a userspace controller. [LearnedCache](https://arxiv.org/html/2605.26168v2) demonstrates integer perceptron page-cache policies with offline training and a custom cache_ext kernel. Predictive allocation has substantial history, including [LLAMA](https://research.google/pubs/learning-based-memory-allocation-for-c-server-workloads/).

The detailed comparisons and verified bibliography are in [PRIOR_ART.md](PRIOR_ART.md).

## Proposed primary experiment: bounded learned CPU slices

**Engineering estimate.** A small online supervised predictor coupled to an otherwise straightforward weighted-vtime policy is the best initial experiment under the upstream-only, C/eBPF, low-dependency constraints. The proposed model predicts CPU-burst behavior from a small, bounded history vector and changes only a capped slice. Dispatch and fairness accounting remain explicit and independently testable. This direction is conditional on review; it is not an approved implementation specification.

The project should begin with one homogeneous CPU domain in a disposable Ubuntu QEMU VM. A C userspace loader would load and observe the policy. Fixed-point model inference and bounded incremental updates would run inside BPF if their verifier behavior, synchronization requirements, and measured overhead are acceptable. A hybrid model-update path remains an allowed alternative if the evidence favors it.

An initial model can be a small linear regressor trained through clipped, bounded incremental updates. A perceptron is suitable for a binary short/long classification target but is not interchangeable with duration regression. Either choice must define its target, feature normalization, scaling, rounding, update rate, loss, and saturation behavior. EWMA is a baseline; placing EWMA inside a feature vector does not by itself establish meaningful ML.

### Label correctness is an implementation gate

**Engineering inference.** An observed run interval can end because of preemption or slice exhaustion. It is not automatically a complete burst ending in voluntary blocking. Training on those intervals while changing slices can make the model learn its own policy limits.

Before connecting predictions to slices, validate a label definition with trace evidence. A candidate is accumulated on-CPU time across runnable preemptions until a validated blocking event. Exits, missing observations, and capped histories need explicit handling. If only run-interval duration is observable reliably, call the target that and document censoring; do not rename it a true burst. Persist the features used at prediction time so later labels cannot accidentally use future information.

### What the experiment would test

**Untested hypotheses.** A tiny online model may predict behavior better than EWMA on workload transitions; bounded slice changes may help selected mixed workloads without unacceptable fairness or overhead costs; in-BPF updates may cost less than frequent userspace synchronization. None is a performance claim.

Test prediction loss and application outcomes separately. Better prediction does not guarantee better scheduling. A model-disabled run, EWMA policy, frozen-model run, and online-model run using identical scheduling plumbing are necessary to identify the source of any change. Compare against the actual guest default scheduler, `scx_simple`, and a supported mature scx policy.

## Backup experiment: learned proactive cgroup reclaim

**Engineering estimate.** If burst labels, verifier complexity, or scheduling overhead make the primary experiment impractical, use a small userspace online model controlling bounded `memory.reclaim` requests for an explicitly selected test cgroup. Read PSI and memory statistics; optionally read DAMON observations without granting it reclaim control in the first experiment.

**Verified facts.** The [cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html) exposes proactive reclaim. The kernel may reclaim more or less than requested, and an insufficient reclaim can return `EAGAIN`; successful writes are not proof that the target amount was recovered. [PSI](https://docs.kernel.org/accounting/psi.html) measures time stalled on resources. These interfaces do not reveal per-page future reuse or application SLOs by themselves.

**Engineering inference.** This backup avoids inventing page-cache hooks, but substantially overlaps TMO/Senpai. Its value would need to come from a reproducible learning-versus-feedback-controller experiment, not from creating the first PSI-driven reclaim daemon. Include a fixed controller and a Senpai-style feedback baseline. Stop further requests when the test stops; previously reclaimed pages cannot be instantly restored by “rollback.”

**Untested hypothesis.** An online model might improve reclaim-budget selection for workloads that change phases. Delayed refault effects, I/O interference, swap configuration, and imperfect application-health signals could erase that benefit.

## Algorithm feasibility comparison

The following are **engineering estimates**, not measurements.

| Method | Initial suitability | Main cost or uncertainty |
| --- | --- | --- |
| EWMA/statistical history | Required baseline | Cheap and understandable; no learned feature weights |
| Online linear regression | Primary model candidate | Fixed-point overflow, normalization, clipping, drift, and noisy labels |
| Perceptron | Binary behavior alternative | Threshold labels and asymmetric mistakes must be justified |
| Contextual bandit | Later bounded-action experiment | Reward attribution, safe exploration, and policy-induced selection bias |
| Small neural network | Later model comparison | More arithmetic and state; Meta and other prior work already explore it |
| Online reinforcement learning | Defer | Multiobjective rewards, delayed effects, and exploration can harm fairness or latency |

## Credible project gaps

**Engineering judgments.** The inspected sources suggest useful work in a compact implementation with reproducible VM instructions, documented labels, bounded numeric behavior, model/heuristic ablations, drift tests, separate instrumentation/training/inference costs, and recovery checks. The survey does not prove that these features are absent from every project.

Approximately **1,000–3,000 lines of core C/eBPF** is a scope target, excluding tests, documentation, generated headers, and vendored libraries. It is not a production-readiness estimate. CPU placement, NUMA optimization, page eviction, allocation, and coordinated CPU/memory control should remain out of the first experiment.

## Risks and decision gates

1. **Labels and policy feedback:** validate blocking/preemption semantics and avoid future-information leakage before model deployment.
2. **Numeric and concurrent state:** prove accumulator bounds, use deterministic saturation, and choose a model ownership/update design that avoids races.
3. **Fairness:** account for actual runtime independently of predicted slices; test unequal task weights, continuously runnable tasks, sleepers, and adversarial churn.
4. **Overhead:** feature gathering, map access, synchronization, and statistics can exceed model arithmetic. Measure each and keep a model-disabled comparison.
5. **Compatibility:** pin the first kernel and scx reference; test later kernels separately. Do not equate CO-RE with stable sched_ext kfuncs.
6. **Safety:** watchdog fallback limits scheduler stalls; it does not guarantee application SLOs, prevent every kernel bug, or undo already incurred latency.
7. **Evaluation:** QEMU is a deployment/correctness environment. Topology, cache, frequency, and virtualization effects require subsequent bare-metal validation before general claims.

Proceed only after review of [PROJECT_COMPARISON.md](PROJECT_COMPARISON.md), [RECOMMENDATION.md](RECOMMENDATION.md), and [ARCHITECTURE.md](ARCHITECTURE.md). The next concrete task is a reviewed label specification and a pinned VM capability checklist, followed by a non-learning baseline and reference arithmetic tests. No experimental BPF is authorized by this planning document.
