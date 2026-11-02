<h1 align="center">Lertick</h1>

<p align="center">CPU scheduling that learns inside the Linux kernel.</p>

## Performance at a glance

**Lertick online mode recorded 50.34% lower short-burst completion p99 than Linux EEVDF in our VM benchmark.** Mixed-contention tests also expose clear regressions:

| Workload | Compared with | Metric | Measured change |
| --- | --- | --- | --- |
| Short CPU bursts (`shortburst`) | Linux EEVDF | Burst completion p99 | **50.34% lower** |
| Frequent wakeups (`wakeheavy`) | Linux EEVDF | Burst completion p99 | **27.88% lower** |
| Mixed CPU contention (`mixed`) | Linux EEVDF | Kernel wake p99 | **157.54% higher** |
| Mixed CPU contention (`mixed`) | `scx_bpfland` v1.0.16 | Burst completion p99 | **155.40% higher** |

These are median paired trial changes from **five repetitions** on **ARM64 QEMU/HVF, Linux `7.0.0-38-generic`, four vCPUs**, using implementation `109ce71`, 10-second measured windows and two-second warm-up. Completion includes the burst's CPU service; wake latency measures wakeup to running. The intervals and every policy are in the [Phase 2 results](docs/PHASE2_RESULTS.md#selected-gains-and-regressions--measured-tradeoffs), with [raw evidence and reproduction commands](results/phase2/README.md). These results do not establish a learning benefit over fixed slices or performance on current kernels and physical hardware.

**Latest model work: better predictions do not yet mean faster scheduling.** In the separate [240-trial confirmation](results/model-confirmation/README.md), optional EWMA initialization and fractional updates reduced online prediction MAE by **61–82%** across four custom workloads versus the original online model. There was no reliable throughput gain; shortburst wake p99 **increased 35.65%** versus original online, and full-preemption held-out alternating-burst MAE **increased 17.82%**. Model defaults remain `bias` / `trunc`. A [natural watchdog fallback remains unresolved](results/model-confirmation/README.md#unresolved-fallback-and-omissions); a successful retry did not erase it.

<p align="center">
  <a href="#get-started-on-linux">Get started →</a> &nbsp;&nbsp;
  <a href="docs/DAILY_USE.md">Daily use →</a> &nbsp;&nbsp;
  <a href="#how-it-works">Explore Lertick →</a>
</p>

<p align="center">
  <img src="docs/assets/lertick-overview.svg" alt="Lertick learns task CPU bursts inside eBPF and selects bounded time-slice budgets on a compatible sched_ext Linux kernel." width="1120" />
</p>

<p align="center">
  <strong>Learns task bursts.</strong> Training and inference run inside eBPF.<br />
  <strong>Adapts time slices.</strong> Predictions select bounded new CPU budgets.<br />
  <strong>Stays on until stopped.</strong> Continuous foreground or systemd service, with kernel fallback.
</p>

<p align="center">
  <a href="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml"><img src="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml/badge.svg?branch=main" alt="Lertick checks" /></a>
  <img src="https://img.shields.io/badge/sched__ext-experimental-6e6e73?style=flat-square" alt="Experimental sched_ext scheduler" />
</p>

<br />

## Get started on Linux

**A compatible running sched_ext kernel is required.** Loading and unloading Lertick on that kernel needs no reboot. Installing or booting a different kernel may require one; the helper never changes your kernel or boot configuration.

### 1. Check your running kernel

```sh
cat /sys/kernel/sched_ext/state
```

Continue when it reports `disabled`. If the file is missing, your running kernel is not ready for this installation path. If another scheduler is enabled, stop it through its own manager first. [Kernel requirements →](docs/INSTALL.md#1-check-the-running-kernel)

### 2. Build and install

On **Ubuntu 24.04**, install the build tools and matching kernel tools, then use your normal account to build:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends \
  git ca-certificates build-essential clang-18 llvm-18 pkg-config python3 \
  libelf-dev zlib1g-dev libzstd-dev util-linux linux-tools-common "linux-tools-$(uname -r)"
git clone https://github.com/Denny-Lin/lertick.git
cd lertick
./deploy.sh install
```

The first build downloads pinned libbpf source and builds it locally. No distribution library is replaced. If `bpftool version` fails or your distribution packages it differently, use the [toolchain guide](docs/INSTALL.md#2-install-build-prerequisites).

### 3. Enable your daily scheduler

```sh
lertick check
lertick verify
lertick enable
lertick status
```

`verify` loads the BPF programs **without attaching the scheduler**. `enable` starts Lertick now and at boot. Installation alone leaves it stopped. The service uses online mode, all-task learning and compact aggregate health summaries every 60 seconds. No per-task names or PIDs appear in those summaries.

```mermaid
flowchart LR
    A[Install] --> B[Verify]
    B --> C[Enable]
    C --> D[Continuous scheduling]
    D --> E[Stop or fallback]
    E --> F[Kernel default scheduler]
    classDef default fill:#f5f5f7,stroke:#d2d2d7,color:#1d1d1f
    linkStyle default stroke:#86868b,stroke-width:1.5px
```

The service reports ready only after actual scheduler registration and its first health check. If it fails or the kernel falls back, `Restart=no` leaves the default scheduler running instead of retrying in a loop. Daily operation remains experimental. [Service behavior and recovery →](docs/DAILY_USE.md#service-readiness-and-fallback)

### Keep control

| Command | What it does |
| --- | --- |
| `lertick update` | Build and verify a candidate before replacing the managed service; retain a complete previous bundle. |
| `lertick rollback` | Restore the retained previous bundle, preserving enabled/active intent. |
| `lertick report` | Print a local JSON report for review and manual sharing; no upload or raw logs by default. |
| `lertick stop` | Stop the service now. `lertick disable` also turns off boot startup. |
| `lertick run` | Run continuously in the foreground until Ctrl-C; no boot-time enablement. |
| `lertick trial` | Run an explicit 30-second foreground trial. |

`run` and the raw loader default to `--duration 0`, meaning until stopped. Use `trial` or `run --duration 30` for a finite session. Stop the service before a foreground run; Lertick refuses a second active scheduler. `lertick plan` prints a command without building, loading or attaching anything. [Daily use, updates and reporting →](docs/DAILY_USE.md)

## How it works

```mermaid
flowchart LR
    H[Task burst history] --> M[Per-task eBPF model]
    M --> B[Bounded slice budget]
    B --> D[Weighted virtual-time dispatch]
    D --> R[Actual CPU execution]
    R --> L[Completed burst label]
    L --> M
    classDef default fill:#f5f5f7,stroke:#d2d2d7,color:#1d1d1f
    classDef learned fill:#edf5ff,stroke:#a8c8ee,color:#1d1d1f
    class M,B learned
    linkStyle default stroke:#86868b,stroke-width:1.5px
```

The model learns while tasks run, with no offline dataset, model download or cloud inference. After warm-up, predictions select newly assigned slice budgets within the default **0.5–5 ms** range. Those are assigned budgets, not a guarantee of exact execution time. Fairness accounting continues charging actual CPU service independently of predictions. [Architecture, modes and limits →](docs/OVERVIEW.md)

The helper learns from all eligible tasks by default. The raw loader's historical name-prefix default remains available for reproducible experiments. `shadow` trains while using fixed slices, and **still activates the scheduler**; it is not a passive preview.

Models restart with each attachment. Continuous learning adapts within a run; updating the code does not by itself establish better throughput or latency. [Measured improvement workflow →](docs/DAILY_USE.md#improve-performance-with-evidence)

## Works with Lerwatt

Let [Lerwatt](https://github.com/Denny-Lin/lerwatt) manage CPU power constraints while Lertick handles scheduling. Their optional local link shares fresh demand and limits; models, updates and recovery stay separate. Compatible model improvements need no joint retraining or peer update. Either project can run alone. **[Connect them in a few steps →](docs/INTEGRATION.md)**

## What has been tested

| Validation | Evidence |
| --- | --- |
| Runtime environment | **Ubuntu 24.04.5 LTS, ARM64, Linux 7.0.0-38-generic**, in a VM |
| Kernel integration | BPF verifier acceptance, registration, task execution, in-kernel model updates and bounded adaptive budgets |
| Continuous loader checks in that VM | Unlimited default, compact 60-second summaries, readiness after registration/initial health, and verifier-only loading without replacing the active scheduler |
| Daily service lifecycle in that VM | 65-second continuous session; enable/start/stop/restart/disable, update, injected startup failure with old-bundle recovery, manual rollback, inactive update, local reports and uninstall |
| Historical correctness and recovery | 19 correctness cases, 15 recovery tests, churn and concurrent fork checks |
| Historical benchmarks | 360 completed scheduler/control trials with retained results and uncertainty |
| Optional model confirmation | 240 valid long-window trials; prediction gains, latency regressions and a separate failed session remain documented |

Ubuntu release numbers alone do not determine support. Stock Ubuntu 22.04/24.04 GA kernels are not the tested sched_ext target; HWE/OEM kernels can differ. Other kernels require their own build, verifier and runtime checks. Short loader checks do not establish days of service uptime, physical-hardware performance or cross-kernel compatibility. [Exact requirements and deployment evidence →](docs/INSTALL.md)

## Go further

**[Browse all documentation →](docs/README.md)**

| I want to… | Start here |
| --- | --- |
| Install on a compatible kernel | [Native installation](docs/INSTALL.md) |
| Run daily, update, roll back or collect a report | [Daily use](docs/DAILY_USE.md) |
| Inspect the model, dispatch and CPU-frequency integration | [Implementation and modes](docs/OVERVIEW.md) · [Correctness](docs/PHASE2_CORRECTNESS.md) |
| Connect the optional Lerwatt power manager | [Shared integration guide](docs/INTEGRATION.md) |
| Reproduce the VM results | [VM reproduction](docs/DEPLOYMENT.md) · [Benchmarks](benchmarks/README.md) |
| Contribute or report a problem | [Contributing](.github/CONTRIBUTING.md) · [Report a bug](https://github.com/Denny-Lin/lertick/issues/new?template=bug_report.yml) · [Security](.github/SECURITY.md) |

Lertick is **learning + tick**. Scheduler, loader and associated build material retain **GPL-2.0-only** notices; independently authored model, workloads and documents use **MIT**. [License boundaries and provenance →](docs/legal/THIRD_PARTY.md)
