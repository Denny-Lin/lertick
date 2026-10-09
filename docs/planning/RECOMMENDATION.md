# Initial project recommendation

> Historical Phase 0 planning snapshot. The user approved ScxSprout on 2026-10-09. Implementation and actual Linux validation now exist; see [Phase 1 results](../PHASE1_RESULTS.md), [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md). Statements below describe the original planning stage.

Source review date: **2026-10-09**. Status: **Phase 0 planning only; project direction awaiting review**. There is no scheduler implementation, tested kernel support matrix, or measured performance result in this repository.

## Recommended direction

Recommend a small **online CPU-burst regression experiment built on sched_ext**, conditionally named **ScxSprout / scx-sprout**, under the public umbrella repository **linux-resource-learning**. It should learn a limited prediction target, use predictions to adjust bounded slice budgets, and expose sufficient diagnostics to tell whether the learning helps.

The name is provisional. A GitHub repository-name search on 2026-10-09 returned no `scx-sprout` `in:name` matches; this is limited collision checking, not trademark clearance or a promise of global uniqueness. The public repository name remains the neutral umbrella name.

This is an engineering project with a research question, not a claim to have invented learned scheduling. Meta has described a small neural-network CPU-yield predictor, and the sched-ext ecosystem already contains sophisticated behavior-adaptive schedulers. Prior work reviewed in [PRIOR_ART.md](PRIOR_ART.md) must shape the final differentiator. [sched_ext project overview](https://sched-ext.com/docs/OVERVIEW), [sched-ext/scx](https://github.com/sched-ext/scx).

## Why this direction

The most useful initial question is narrow: **can a small online model predict a completed CPU-service burst more accurately than EWMA, at sufficiently low cost, and does using those predictions improve a defined workload without unacceptable fairness or throughput regressions?** Prediction accuracy alone does not answer the policy question.

The initial advantage is testability. A carefully defined completed burst can provide a supervised learning label rather than a delayed causal reward. sched_ext also supplies reversible runtime loading and fallback when the policy exits or stalls. The kernel's BPF/sched_ext protections reduce some failure risks; they do not establish application-level fairness, good latency, or absence of kernel defects. [Linux 6.12 sched_ext documentation](https://docs.kernel.org/6.12/scheduler/sched-ext.html).

The comparison also considered learned reclaim, allocation, placement, and joint CPU-memory control. The strongest memory alternative has attractive integration and maintenance properties but noisier outcome attribution. The estimated score advantage is small and changes with priorities; see [PROJECT_COMPARISON.md](PROJECT_COMPARISON.md).

## Honest MVP scope

After review approval, implement one foreground C loader and one C/eBPF scheduler for a pinned kernel environment. Begin with a fixed-slice weighted virtual-time baseline, then add a small feature vector, bounded fixed-point online linear regression, and a bounded slice adjustment. Include explicit fixed-slice, EWMA, frozen-model, and learning-enabled modes so that adaptation and prediction can be separated experimentally.

The model must update coefficients from actual labeled outcomes using an incremental learning rule. Collecting a moving average or choosing among hand-tuned thresholds does not satisfy the ML requirement. Neither an external AI API, GPU, proprietary dependency, nor ML framework is needed.

The label contract is a hard gate. Accumulate CPU service across preemption and slice expiry, distinguish genuine sleep from exit/yield/censoring using audited callbacks for the pinned kernel, and report censoring/sample coverage. Do not train on the truncated runtime of every dispatched slice as though it were a complete burst. Demonstrate reliable accounting and useful coverage for the intended workloads before enabling learned decisions. The exact accounting mechanism remains a feasibility item, described in [ARCHITECTURE.md](ARCHITECTURE.md).

Predictions should influence only clamped slice budgets. Charge fairness from actual service, preserve weights, respect CPU affinity, and exercise task create/exit/migration cases. No topology-aware placement learner, neural network, persistent service, kernel patch, or combined memory actuator is included in the initial scope. A complete general-purpose replacement for Linux's scheduling and cgroup semantics is outside the MVP.

Estimated core implementation budget, excluding tests, documentation, generated headers, and vendored compatibility code:

| Component | Estimated core C/eBPF LOC |
| --- | ---: |
| C loader, CLI, lifecycle and compatibility probes | 300–500 |
| Scheduling callbacks, task state, service accounting and safe baseline | 600–900 |
| Fixed-point features, regression and numeric bounds | 250–450 |
| Counters and sampled diagnostics | 200–400 |
| Shared data definitions | 100–150 |
| **Total** | **1,450–2,400** |

These figures are scope estimates, not a promise. A requirement for full cgroup-controller semantics, broad kernel compatibility, or sophisticated CPU topology handling would require a larger implementation or an explicit scope change.

## Backup: learned proactive reclaim

The backup is a bounded **cgroup-v2 `memory.reclaim` controller** in userspace C. Start with one isolated cgroup, fixed interval actions, no-op as an action, and shadow-mode logging. Keep native reclaim/MGLRU page selection unchanged. Add a small contextual learner only after its observed reward is defined and measurement windows are validated. DAMON can be optional read-only diagnostics; persistent DAMOS pageout belongs to a later reviewed phase.

Senpai/TMO already uses PSI to guide reclaim and explains why one-shot `memory.reclaim` is preferable to leaving a reduced limit behind. The differentiator must therefore be learned context-conditioned behavior, evidence across workload changes, and auditable overhead; PSI-driven reclaim itself is established prior art. [Meta TMO](https://engineering.fb.com/2022/06/20/data-infrastructure/transparent-memory-offloading-more-memory-at-a-fraction-of-the-cost-and-power/), [Senpai](https://github.com/facebookincubator/senpai).

The controller must treat footprint changes as noisy observed outcomes, not exact reclaimed-byte labels, and account for delayed refaults. Bounds apply to requested bytes and submission rate, not actual reclaimed bytes or kernel execution time. Requests may under/over-reclaim. Controller termination prevents new submissions but does not cancel an already executing kernel request or restore evicted pages instantly. [cgroup-v2 `memory.reclaim` interface](https://docs.kernel.org/6.12/admin-guide/cgroup-v2.html). Estimated core size: **1,200–2,300 C LOC**. If the CPU label or verifier gates fail, reassess this alternative before moving beyond shadow mode.

## Main risks and decision gates

| Risk | Required evidence or response |
| --- | --- |
| Labels are censored, incorrect, or too sparse | Audit callback semantics, validate controlled burst workloads, publish sample coverage; switch direction if inadequate |
| The model learns predictions but policy does not benefit | Separate prediction and policy tests; compare against EWMA/frozen/fixed baselines; report negative results |
| Fixed-point overflow, scaling or verifier constraints | Prove bounded arithmetic, test extremes, verify on the pinned kernel; consider userspace training with in-BPF inference if measurements justify it |
| Model concurrency/lifecycle errors | Document ownership and synchronization; test fork, exit, migration and state cleanup |
| Smaller slices damage throughput or fairness | Clamp slice range, charge actual service, run fairness/starvation and throughput guardrails |
| Kernel API changes | Pin source versions and toolchain, feature-probe, keep a small explicit tested matrix |
| QEMU results do not transfer to hardware | Use QEMU for deployment/recovery and correctness; add physical Linux measurements before generalizing cache, energy or production claims |
| Prior art removes apparent novelty | Reframe as reproducible engineering, or choose a narrower demonstrated gap; never claim first-of-kind without evidence |

## Effort and concrete next steps

An experienced C/eBPF engineer should budget approximately **180–330 engineering hours** for a reviewable MVP with tests, deployment verification, benchmark automation, and release-quality documentation, assuming access to a suitable Linux VM and no major label/API redesign. This is roughly **5–9 full-time engineering weeks** or **9–17 weeks at 20 hours/week**. Kernel/BPF learning, hardware access delays, and long benchmark campaigns can add substantially more calendar time. Community maintenance and a broadly usable release are continuing work, not part of the core LOC estimate.

1. Review and approve or revise the direction, label contract, fairness scope, license choices, and benchmark guardrails.
2. Pin an Ubuntu/QEMU image, upstream kernel source reference and build toolchain; verify required config and features without assuming the Ubuntu release implies sched_ext support.
3. Implement the baseline and controlled accounting checks only after approval. Demonstrate load, workload observation, orderly exit and fallback before adding learning.
4. Add the online model and ablations, measure training/inference cost, then run the benchmark matrix and publish raw results including regressions.
5. Decide whether the evidence supports a first experimental release, a redesigned hypothesis, or the memory backup.

See [ROADMAP.md](ROADMAP.md), [DEPLOYMENT.md](../DEPLOYMENT.md), [BENCHMARK_PLAN.md](BENCHMARK_PLAN.md), and [LICENSE_ANALYSIS.md](LICENSE_ANALYSIS.md). Phase 0 publication of these planning documents does not imply approval to build or run an experimental scheduler.
