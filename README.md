# Linux Resource Learning

**Small, measurable online learning for Linux resource management.**

[繁體中文](README.zh-TW.md) · [Recommendation](docs/planning/RECOMMENDATION.md) · [Prior art](docs/planning/PRIOR_ART.md) · [Roadmap](docs/planning/ROADMAP.md)

**Status: Phase 0 — research and design. There is no runnable scheduler yet.**
No kernel has been modified, no experimental BPF program has been executed, and no performance result is claimed by this repository.

The proposed first project, **ScxSprout (`scx-sprout`)**, is a small C/eBPF sched_ext scheduler that learns task CPU-burst behavior online and uses predictions to select bounded time slices. A conventional weighted virtual-time policy retains responsibility for ordering and CPU-share accounting. A bounded learned memory-reclaim controller is the backup direction.

## Why explore this?

Mixed workloads can change faster than one fixed policy can anticipate. Linux's default fair scheduler and existing sched_ext policies already handle many such cases well. Our question is narrower: **can a tiny, auditable online model improve a specified workload's tradeoff after accounting for training cost and fairness?** The answer requires measurements, including negative results.

This is not the first learned scheduler. Meta's scheduler experiments, hybrid ML sched_ext projects, and learned memory-management papers already cover important parts of this space. Read the [source-linked review](docs/planning/PRIOR_ART.md) before interpreting any novelty claim.

## Proposed learning loop

1. Collect a bounded task history from scheduling callbacks.
2. Accumulate CPU execution across preemptions; identify reliable completed bursts and explicitly handle censored observations.
3. Predict the next burst with a fixed-point online linear model.
4. Update model weights from prediction error during execution.
5. Clamp the prediction to a safe time-slice range; use the conventional policy when data are missing or invalid.

Fixed slices and EWMA are baselines. They are not described as machine learning. CPU placement, neural networks, deep reinforcement learning, and CPU/memory coordination are outside the initial MVP. See the [architecture](docs/planning/ARCHITECTURE.md) for concurrency, arithmetic, labeling, and fairness risks.

## Read or contribute now

```sh
git clone https://github.com/Denny-Lin/linux-resource-learning.git
cd linux-resource-learning
python3 tests/check_docs.py
```

Python 3.10+ and Git are enough for the current documentation checks. The checker uses only the standard library and has no network or kernel access. [Contributing](CONTRIBUTING.md) describes independent documentation, experiment-design, compatibility, and algorithm work.

## Install and run later

No scheduler binary, build system, package, or service is supplied at Phase 0. The proposed target is an Ubuntu QEMU guest with a sched_ext-enabled kernel, beginning with a pinned Linux 6.12 baseline. **No kernel/Ubuntu combination is validated by this project yet.** Newer kernels must be tested against matching headers and scheduler sources because sched_ext's BPF interface can change.

The [deployment plan](docs/DEPLOYMENT.md) records kernel configuration, expected C/LLVM/libbpf dependencies, VM setup, planned commands, verification, and recovery. Its proposed `scx-sprout` commands are unavailable until implementation is approved and delivered. No persistent service is planned for early releases.

## Benchmark and recovery

The [benchmark protocol](docs/planning/BENCHMARK_PLAN.md) compares the Linux default scheduler, `scx_simple`, a pinned maintained sched_ext scheduler, fixed slices, EWMA, frozen weights, and online learning. It requires raw data for throughput, p95/p99 latency, migrations, fairness, starvation, and separate model costs. QEMU is for deployment and safety testing; it cannot establish host-independent bare-metal performance.

For a future foreground sched_ext loader, normal shutdown must destroy its BPF link; verify that `/sys/kernel/sched_ext/state` returns to `disabled`. The kernel provides fallback on scheduler errors, stalled runnable tasks, and the emergency SysRq-S path. These mechanisms do not prove a policy is fair or fast. See the exact [recovery procedure](docs/DEPLOYMENT.md) before experiments. Stopping the backup cgroup `memory.reclaim` controller prevents new requests; an in-flight request may finish and evicted cache cannot be restored immediately. Deferred DAMOS actuation needs explicit kernel-context shutdown.

## Planning documents

| Document | Purpose |
| --- | --- |
| [RESEARCH](docs/planning/RESEARCH.md) | Evidence, algorithm options, feasibility, and unanswered questions |
| [PRIOR_ART](docs/planning/PRIOR_ART.md) | Existing projects and academic work with primary links |
| [PROJECT_COMPARISON](docs/planning/PROJECT_COMPARISON.md) | Six candidates scored on nine criteria |
| [ARCHITECTURE](docs/planning/ARCHITECTURE.md) | Proposed primary and backup designs |
| [OPEN_SOURCE_STRATEGY](docs/planning/OPEN_SOURCE_STRATEGY.md) | Names, repository structure, contribution, and adoption |
| [LICENSE_ANALYSIS](docs/planning/LICENSE_ANALYSIS.md) | MIT/GPL boundaries and dependency obligations |
| [ROADMAP](docs/planning/ROADMAP.md) | Phases, effort estimates, and decision gates |
| [BENCHMARK_PLAN](docs/planning/BENCHMARK_PLAN.md) | Reproducibility and falsifiable acceptance criteria |
| [RECOMMENDATION](docs/planning/RECOMMENDATION.md) | Proposed direction, risks, and next steps |

The source review was performed on **2026-10-09**. Documents distinguish verified facts, engineering estimates, and untested hypotheses. Branch-tip links establish source availability on that date; implementation and benchmark releases must pin immutable commits.

## License

Current original documentation and repository tooling use the [MIT License](LICENSE). Future kernel-facing BPF files are planned under `GPL-2.0-only`, with an appropriate BPF license section and retained upstream notices. A permissive repository license does not override GPL-only helpers, kfunc requirements, or copied kernel code. See [LICENSE_ANALYSIS](docs/planning/LICENSE_ANALYSIS.md).
