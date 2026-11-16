<h1 align="center">Lertick</h1>

<p align="center">CPU scheduling that learns inside the Linux kernel.</p>

Lertick always owns its scheduling model. New installations automatically exchange
compatible, fresh advice with a running Lerwatt; no peer-specific install flag is
required. Missing or removed peers leave scheduling independent. Neither project
starts, replaces or retrains the other. [Automatic coordination](docs/INTEGRATION.md).

## Performance at a glance

**Better than Linux's default scheduler (EEVDF) on 5 of 11 standard kernel scheduler benchmarks with 4 CPUs and on 6 with 8 CPUs.** It is worse on 2 and 3, mostly when many message threads wake groups of workers.

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu4.svg" alt="4 CPUs: Lertick is better than Linux EEVDF on perf pipe, hackbench threads, schbench 1-thread request and wakeup latency, and kernel compile; the same on perf messaging, hackbench 16 groups and schbench 4-thread throughput; worse on schbench 4 message threads request and wakeup latency." width="1120" />
</p>

<p align="center">
  <img src="docs/assets/lertick-vs-eevdf-cpu8.svg" alt="8 CPUs: Lertick is better than Linux EEVDF on perf pipe, hackbench threads and 16 groups, schbench 1-thread request and wakeup latency, and schbench 4-thread throughput; the same on schbench 4-thread request latency and kernel compile; worse on perf messaging processes and threads and schbench 4 message threads wakeup latency." width="1120" />
</p>

| Against Linux EEVDF | Better | Same | Worse |
| --- | ---: | ---: | ---: |
| **Lertick, 4 CPUs** | **5** | 4 | 2 |
| **Lertick, 8 CPUs** | **6** | 2 | 3 |
| [scx_bpfland](https://github.com/sched-ext/scx), 4 CPUs | 4 | 1 | 6 |
| [scx_bpfland](https://github.com/sched-ext/scx), 8 CPUs | 5 | 1 | 5 |

<sub>Lertick in its default learned (`online`) mode. None of these benchmarks was used to choose its settings. Median of 5 runs on one ARM64 VM (Linux 7.0) using 4 or 8 CPUs; "Same" means within 5%. Kernel compile rebuilds all of `kernel/` (about 30 s). scx_bpfland is the community sched_ext scheduler, version 1.1.2. \*Linux takes about 20 µs per pipe round trip in this VM against 1.6 to 2.3 µs for Lertick, which is probably an artefact of the VM. Physical hardware is untested. [All runs →](results/deadline-ordering/README.md#longer-kernel-compile-result-2026-11-16)</sub>

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

You need a Linux kernel with sched_ext: `cat /sys/kernel/sched_ext/state` should print `disabled`. Switching schedulers needs no reboot.

**1. Download**

```sh
git clone https://github.com/Denny-Lin/lertick.git
cd lertick
```

**2. Install and turn on** (Ubuntu 24.04)

```sh
sudo apt-get update
sudo apt-get install -y build-essential clang-18 llvm-18 pkg-config python3 \
  libelf-dev zlib1g-dev libzstd-dev linux-tools-common "linux-tools-$(uname -r)"
./deploy.sh install
lertick enable
```

**3. Remove**

```sh
lertick uninstall
```

Linux's default scheduler takes over immediately.

**Everyday commands**

| Command | What it does |
| --- | --- |
| `lertick status` | Show whether Lertick is running |
| `lertick stop` / `lertick start` | Pause it, or start it again |
| `lertick update` | Update to the latest version |
| `lertick help` | All other commands |

If anything fails, Lertick stops and Linux's default scheduler keeps running. [Other distributions and details →](docs/INSTALL.md)

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
