# Linux Resource Learning

**ScxSprout: a small C/eBPF sched_ext scheduler with online learning inside eBPF.**

[繁體中文](README.zh-TW.md) · [Deployment](docs/DEPLOYMENT.md) · [Phase 1 results](docs/PHASE1_RESULTS.md) · [Prior art](docs/planning/PRIOR_ART.md)

**Status: Phase 1 implemented and validated on an actual Linux kernel.** On
2026-10-09, the QEMU Ubuntu guest compiled the scheduler and loader, accepted the
BPF programs, registered sched_ext, executed real workloads, updated model
parameters inside eBPF, and applied predictions to bounded dispatch slices.
Normal unload, signal/process termination, SysRq abort and kernel watchdog
fallback restored the default Linux scheduler. This establishes architecture
feasibility in one tested environment; no performance improvement is claimed.

## What is implemented

ScxSprout follows the official sched_ext C examples, pinned at
`370b1cea95c6bb668d5b9261f069d433142b9986`. It retains global weighted virtual-time
dispatch and charges actual CPU service from `sum_exec_runtime`. Only verified
wake-to-sleep episodes produce completed burst labels; preempted execution
segments accumulate in the same episode, and continuously runnable controls
provide no completed labels.

A per-task five-feature Q16 linear model performs projected, error-clipped LMS
training and inference in eBPF. Its causal inputs are a bias, previous completed
burst, burst EWMA, previous sleep interval and queue depth. After eight labels,
predictions can select dispatch slices bounded to 0.5–5 ms. Exhausted or invalid
predictions use the fixed slice. Predictions do not determine dispatch ordering
or substitute for actual CPU accounting.

| Mode | Model | Dispatch slice |
| --- | --- | --- |
| `fixed` | Disabled | Fixed 2 ms by default |
| `shadow` | Training and inference in eBPF | Fixed 2 ms by default |
| `online` | Training and inference in eBPF | Bounded predictions after warmup |

The C/libbpf loader checks compatibility, owns an unpinned attachment, records
JSONL telemetry and detaches after a finite duration. A calibrated workload
measures real thread CPU service, wake lateness, completion latency and completed
work. Host tests check arithmetic against an independent 128-bit oracle.

## Actual validation

The tested environment was **Ubuntu 24.04.5 ARM64**, Linux
**`7.0.0-38-generic`**, in **QEMU 10.2.1 with HVF hardware acceleration** on
Apple Silicon macOS, with four vCPUs and 4 GiB RAM. Required sched_ext, BPF and
BTF configuration and live interfaces were checked.

The Linux suite passed fixed, shadow and online loading and execution. The
online evidence run recorded **1,345 model updates and 1,355 adaptive execution
segments**; shadow recorded 1,348 updates, and fixed recorded none. Selected
budgets were compared with slices observed by real `running` callbacks.
Recovery checks included a deliberately stalled SCHED_EXT task triggering the
kernel watchdog.

Three four-second repetitions compared the default Linux scheduler, official
non-ML `scx_simple`, fixed ScxSprout and online ScxSprout. These short VM
measurements establish a reproducible baseline and do not justify a speedup
claim. Median trial completion percentiles were:

| Policy | p95 (ms) | p99 (ms) |
| --- | --- | --- |
| Default Linux | 3.259 | 3.350 |
| Official `scx_simple` | 20.016 | 20.128 |
| ScxSprout fixed | 2.015 | 2.031 |
| ScxSprout online | 2.023 | 3.996 |

Online p99 was worse than fixed and default in this short comparison. There is
no demonstrated ML improvement. See the [results and limitations](docs/PHASE1_RESULTS.md),
[machine-readable validation](results/phase1/validation.json) and
[environment manifest](results/phase1/environment.txt).

## Build and reproduce

Portable model and documentation checks:

```sh
git clone https://github.com/Denny-Lin/linux-resource-learning.git
cd linux-resource-learning
make test-model
python3 tests/check_docs.py
```

For the Linux deployment, follow the [QEMU setup, dependency and build
commands](docs/DEPLOYMENT.md). Build against the running guest's BTF; containers
cannot replace this kernel validation. The complete suite runs inside that
guest after building ScxSprout and the pinned reference:

```sh
sudo python3 tests/validate_linux.py \
  --output results/local/validation \
  --reference build/reference/scx-c-examples/build/scheds/c/scx_simple \
  --repetitions 3 --seconds 4
```

The [terminal example](examples/README.md), [source overview](src/README.md),
[model tests](tests/README.md) and [workload guide](benchmarks/README.md) describe
the implemented interfaces. See [CONTRIBUTING](CONTRIBUTING.md) for contributions.

## Scope and next work

The current priority is deployment, actual learning and safe recovery. This is
one kernel/architecture validation, not a production release or a proof of
fairness. Longer randomized trials, model error analysis, independent hardware,
migration/affinity stress and overhead measurement remain. CPU placement,
neural networks, reinforcement learning and CPU/memory coordination are deferred.
A learned cgroup memory-reclaim controller remains a research backup direction.

Existing learned scheduling and memory-management work predates this project.
The [primary-source review](docs/planning/PRIOR_ART.md) records that context;
ScxSprout makes no novelty claim.

## Original research and planning

These Phase 0 documents preserve the original proposals and estimates. The
deployment guide and Phase 1 results describe what was actually implemented.

| Document | Purpose |
| --- | --- |
| [RESEARCH](docs/planning/RESEARCH.md) | Evidence, algorithm options and open questions |
| [PRIOR_ART](docs/planning/PRIOR_ART.md) | Existing projects and papers with primary links |
| [PROJECT_COMPARISON](docs/planning/PROJECT_COMPARISON.md) | Six candidate directions |
| [ARCHITECTURE](docs/planning/ARCHITECTURE.md) | Original primary and backup designs |
| [OPEN_SOURCE_STRATEGY](docs/planning/OPEN_SOURCE_STRATEGY.md) | Repository and contribution plan |
| [LICENSE_ANALYSIS](docs/planning/LICENSE_ANALYSIS.md) | MIT/GPL boundaries |
| [ROADMAP](docs/planning/ROADMAP.md) | Original phases, estimates and decision gates |
| [BENCHMARK_PLAN](docs/planning/BENCHMARK_PLAN.md) | Broader experimental protocol |
| [RECOMMENDATION](docs/planning/RECOMMENDATION.md) | Approved direction and research risks |

## License

Original documents, the independently written model and workload use the
[MIT License](LICENSE). The scheduler, loader, associated build scripts and
vendored official-example material carry GPL notices. The root MIT license
does not override those notices. See [THIRD_PARTY](THIRD_PARTY.md) for pinned
provenance and distribution obligations.
