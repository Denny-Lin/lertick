<h1 align="center">Lertick</h1>

<p align="center">CPU scheduling that learns inside the Linux kernel.</p>

Lertick always owns its scheduling model. New installations automatically exchange
compatible, fresh advice with a running Lerwatt; no peer-specific install flag is
required. Missing or removed peers leave scheduling independent. Neither project
starts, replaces or retrains the other. [Automatic coordination](docs/INTEGRATION.md).

## Performance at a glance

**Better than Linux's default scheduler (EEVDF) on 6 of 8 hackbench and schbench measures, including 15% faster hackbench and 61% lower wakeup latency on a busy machine.** One is the same and one is worse: 4% lower throughput when overloaded.

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf.svg" alt="Bar chart of Lertick against Linux EEVDF: hackbench pipes 15% faster, sockets 8% faster; schbench busy wakeup latency 61% lower and request latency 6% lower; light load the same; overloaded wakeup latency 9% lower, request latency 2% lower, and 4% fewer requests per second." width="1120" />
</p>

| Test | Linux EEVDF | Lertick | Result |
| --- | ---: | ---: | :--- |
| hackbench, pipes | 0.83 s | 0.70 s | **15% faster** |
| hackbench, sockets | 1.63 s | 1.50 s | **8% faster** |
| schbench busy, wakeup latency | 121 µs | 47 µs | **61% lower** |
| schbench busy, request latency | 18.2 ms | 17.2 ms | **6% lower** |
| schbench light, request latency | 11.8 ms | 11.7 ms | Same |
| schbench overloaded, wakeup latency | 8.9 ms | 8.1 ms | **9% lower** |
| schbench overloaded, request latency | 379 ms | 372 ms | **2% lower** |
| schbench overloaded, throughput | 125 req/s | 119 req/s | 4% fewer |

<sub>Lertick's learned (`online`) mode against Linux EEVDF on the same machine: the median of 10 runs on one ARM64 VM (Linux 7.0, 4 CPUs), with latency at p99. Fixed-slice mode is within 1% of the learned mode, so most of the gain comes from dispatch rather than from learning so far. The settings were tuned on these two benchmarks (tests not used for tuning are below), and physical hardware is untested. [Method, commands and all runs →](results/deadline-ordering/README.md)</sub>

### On tests not used for tuning

These tests were never used to choose settings. The community sched_ext scheduler [scx_bpfland](https://github.com/sched-ext/scx) is included as a reference.

| Against Linux EEVDF | Better | Same | Worse |
| --- | ---: | ---: | ---: |
| **Lertick, 8 CPUs** | **6** | 2 | 3 |
| **Lertick, 4 CPUs** | **4** | 4 | 3 |
| scx_bpfland, 8 CPUs | 5 | 1 | 5 |
| scx_bpfland, 4 CPUs | 4 | 0 | 7 |

| Test | Lertick, 4 CPUs | Lertick, 8 CPUs |
| --- | :--- | :--- |
| perf bench sched messaging, processes | Same | 6% slower |
| perf bench sched messaging, threads | Same | 9% slower |
| perf bench sched pipe | **92% faster**\* | **89% faster**\* |
| hackbench, threads | **33% faster** | **29% faster** |
| hackbench, 16 groups | Same | **17% faster** |
| schbench, 1 thread, request latency | **25% lower** | **22% lower** |
| schbench, 1 thread, wakeup latency | **7% lower** | **32% lower** |
| schbench, 4 message threads, request latency | 2.3× higher | Same |
| schbench, 4 message threads, wakeup latency | 42% higher | 27% higher |
| schbench, 4 message threads, throughput | Same | **5% higher** |
| Kernel compile (`kernel/sched`) | 9% slower | Same |

<sub>Lertick here is its default learned (`online`) mode. Median of 5 runs on the same VM with 4 or 8 CPUs; "Same" means within 5%. scx_bpfland is the sched-ext/scx 1.1.2 build; its per-test numbers are in the full results. scx_lavd 1.1.2 failed to load on this kernel. \*Linux takes about 20 µs per pipe round trip in this VM, against 1.6 to 2.9 µs for the sched_ext schedulers. This is probably an artefact of the VM, not a general result. Many message threads waking groups of workers remain Linux's strength. [All runs →](results/deadline-ordering/README.md#held-out-result-on-8-cpus-2026-11-16)</sub>

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
  <a href="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml"><img src="https://github.com/Denny-Lin/lertick/actions/workflows/docs.yml/badge.svg?branch=main" alt="CI status" /></a>
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

Dispatch keeps one weighted virtual-time queue. Its clock advances with the CPU time actually delivered, so tasks that keep waking cannot hold back tasks that have already run. A CPU prefers a task that last ran on it when that task is within 2 ms of the queue head. A synchronous wakeup can queue on the waker's CPU. `nice` values and cgroup `cpu.weight` (for example systemd `CPUWeight=`) set each task's share. CPU quotas such as `cpu.max` are not enforced.

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
| schbench and hackbench | 10-repetition comparison of the current scheduler with EEVDF and the previous release; earlier screening rounds are listed with their decisions |
| Historical benchmarks | 360 completed scheduler/control trials on the project's own workloads, with retained results and uncertainty |
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
