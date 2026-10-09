# Development roadmap

> Historical Phase 0 planning snapshot. The user approved ScxSprout on 2026-10-09. Implementation and actual Linux validation now exist; see [Phase 1 results](../PHASE1_RESULTS.md), [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md). Statements below describe the original planning stage.

Source review date: **2026-10-09**. Current phase: **Phase 0 — research and planning**. Scheduler implementation has not begun. Linux loading, compatibility, safety and performance claims remain unverified for this repository.

The recommended direction is conditional; it must pass the accounting, numeric-safety and deployment gates in [RECOMMENDATION.md](RECOMMENDATION.md). The backup is a cgroup-v2 proactive reclaim controller, not a persistent DAMOS actuator.

## Phase 0 — research and direction review

Current deliverables: source-linked prior-art review; at least five scored project candidates; architecture, license, contributor and benchmark plans; a realistic small-code scope; and explicit verified-fact/estimate/hypothesis boundaries. Public planning does not establish that a learning scheduler exists.

Exit gate: project direction, initial workload, label target, intended kernel/toolchain, licensing, fairness limitations and success criteria are reviewed. **Do not begin implementation before that approval.** Record the decision in a short architecture decision document when it is made. No system packages, experimental BPF programs or kernel source changes are needed to complete this phase.

Remaining effort after this planning publication: **4–8 hours** of review and source/version pinning, excluding reviewer waiting time.

## Phase 1 — baseline and trustworthy observations

Estimated effort: **30–55 hours** for an experienced C/eBPF engineer.

Build a fixed-slice weighted virtual-time scheduler and C loader for one pinned Linux environment. Keep activation foreground-only. Configure the documented sched_ext prerequisites; prove load/unload and fallback on an Ubuntu QEMU guest using an independently retained console or SSH path. The kernel documents fair-scheduler fallback on policy termination, stalled runnable tasks and SysRq-S; verify the relevant paths in the guest rather than claiming safety from documentation alone. [Linux 6.12 sched_ext documentation](https://docs.kernel.org/6.12/scheduler/sched-ext.html).

Observe task lifecycle and completed CPU-service bursts without making learned scheduling decisions. Audit event ordering, sleep classification, exit handling, service-accounting validity, CPU migration and censored samples. Use controlled workloads with known sleep/work phases to assess whether the target is measurable. Virtual-machine steal/interrupt time must not be silently described as task CPU service.

Exit gate: reproducible build; verified foreground start/stop; controlled recovery test; correct state cleanup; baseline weighted service behavior; reliable labels and published coverage limits. If label coverage is too low or accounting cannot be made trustworthy within scope, stop CPU implementation and review the memory backup.

## Phase 2 — bounded online learning

Estimated effort: **40–75 hours**.

Implement a small fixed-point online linear model with explicit feature/target scaling, clipping, learning rate and saturating arithmetic. Define synchronization and state ownership. Add fixed, EWMA, frozen-model and online-model modes. Begin in prediction-only mode; connect predictions to bounded slice budgets after arithmetic, verifier, lifecycle and baseline-fairness checks pass.

Measure update count, label/censor counts, prediction error, feature saturation, training/inference time and scheduling callback cost. Calibrate instrumentation overhead using matched runs with diagnostics disabled and enabled. Do not attribute a benchmark improvement to learning merely because the online mode was running.

Exit gate: real incremental coefficient updates are observable; controlled numeric/lifecycle tests pass; verified loading on the pinned guest; model cost is measured; predictions affect actual scheduling decisions within the declared bounds; fallback remains verified. If BPF training is a poor fit, document the measurements before reviewing a hybrid design.

## Phase 3 — performance and robustness validation

Estimated effort: **55–100 hours**, plus machine runtime.

Compare the same workload configurations against the pinned kernel's default fair scheduler, matching `scx_simple`, a pinned mature scheduler such as LAVD/BPFLand, and the project's non-ML/frozen-model variants. The mature scheduler is a competitor, not an assumed superior baseline in every workload. [Upstream example schedulers](https://github.com/torvalds/linux/blob/master/tools/sched_ext/README.md), [sched-ext/scx](https://github.com/sched-ext/scx).

Measure throughput; application p95/p99 latency; runnable wait where available; migrations; weighted fairness; starvation guardrails; model cost; and total controller overhead. Randomize run order, separate warmup from measurement, repeat complete experiments, publish failures and raw data, and distinguish application latency from scheduling latency. Assess phase-changing workloads and CPU-bound tasks that expose weak label coverage.

QEMU validates deployment and controlled comparisons in the declared guest. Add physical Linux hardware before generalizing cache-locality, energy, heterogeneous-core or production claims. A model with better prediction error but worse policy outcomes is a negative result, not a successful performance release.

Exit gate: benchmark records are complete and reproducible; confidence/variability is reported; guardrails and abort conditions were honored; rollback paths still pass; demonstrated limitations determine the release claims. See [BENCHMARK_PLAN.md](BENCHMARK_PLAN.md).

## Phase 4 — first experimental code release

Estimated effort: **45–80 hours**.

Prepare verified build/runtime/recovery instructions, tested compatibility entries, CI, numerical/lifecycle tests, benchmark scripts, machine-readable results, license notices and maintainer review. Review permissions, input/path handling, task data privacy, map resource bounds and failure behavior. Maintain the Phase 0 versus implemented/verified status distinction throughout the README and changelog.

The initial terminal demo should show: foreground activation, real label/model counters, a bounded policy adjustment, a repeatable workload run, termination, and verified return to the default fair scheduler. A replay or prerecorded demonstration must be marked as such. The demo must not invent performance improvements.

Exit gate: a tagged experimental release whose claims match the published evidence. Publication of a planning repository is not completion of this phase and does not constitute a functioning scheduler release. CI success on non-Linux hosts or compilation alone does not establish Linux/BPF runtime compatibility.

## Phase 5 — community development

Calendar horizon: **ongoing, typically months**; no guaranteed adoption or star count.

Use issue and benchmark submission templates to gather actionable reports. Publish good-first-issue tasks for documentation, controlled workload descriptions, compatibility reports, test fixtures and analysis tools. Ask contributors to include pinned versions, hardware/guest details, reproduction steps and raw benchmark output.

Expand compatibility only with tested evidence. Write technical reports describing both successful and unsuccessful experiments. Consider collaboration with sched-ext maintainers when results and code quality justify it. Extend CPU placement, memory control or CPU-memory coordination only after a measured gap motivates the added complexity. See [OPEN_SOURCE_STRATEGY.md](OPEN_SOURCE_STRATEGY.md).

## Effort summary and rescoping rules

| Work | Estimated engineer hours |
| --- | ---: |
| Remaining direction review/pinning | 4–8 |
| Phase 1 | 30–55 |
| Phase 2 | 40–75 |
| Phase 3 | 55–100 |
| Phase 4 | 45–80 |
| **Total remaining** | **174–318** |

Use **180–330 hours** as a rounded planning envelope. These estimates assume an experienced engineer, available Linux test infrastructure, one narrow kernel target and no major redesign. They exclude learning a new kernel subsystem, waiting for reviewers, unavailable hardware, and long-term maintenance. Budget and LOC limits must cause an explicit scope review, not removal of safety checks or publication of unmeasured claims.

If CPU labels fail, the memory backup begins with a fresh reward/measurement review and shadow mode. If online learning is no better than EWMA within the measured uncertainty, publish that result and reconsider the hypothesis. If a mature scheduler already solves the selected workload adequately, a measurement toolkit or carefully bounded extension may be more useful than another standalone scheduler.
