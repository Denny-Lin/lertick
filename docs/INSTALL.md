# Install and try Lertick

[← Back to Lertick](../README.md)

Lertick builds on the target Linux machine and supports continuous foreground use or a managed systemd service. Installation adds the commands and service unit without starting or enabling it. Run `lertick enable` afterward to start now and enable startup at boot. See [daily use, updates and reporting](DAILY_USE.md).

**A compatible running kernel can load and unload Lertick without a reboot.** If your current kernel lacks the required sched_ext support, changing kernels requires booting the replacement first. Installing the helper alone cannot add that support.

## 1. Check the running kernel

```sh
uname -r
uname -m
cat /sys/kernel/sched_ext/state
test -r /sys/kernel/btf/vmlinux
```

The running kernel needs sched_ext, BPF syscall/JIT support and live BTF. Its configuration must be available at `/proc/config.gz` or `/boot/config-$(uname -r)` for the current checks. The main configuration requirements are:

```text
CONFIG_SCHED_CLASS_EXT=y
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_DEBUG_INFO_BTF=y
```

The [kernel's sched_ext guide](https://docs.kernel.org/scheduler/sched-ext.html#switching-to-and-from-sched-ext) also lists `CONFIG_BPF_JIT_ALWAYS_ON=y` and `CONFIG_BPF_JIT_DEFAULT_ON=y`. The actual BPF load must succeed with your current privileges and security policy; a readable configuration or BTF file alone does not establish that.

Upstream sched_ext first appeared in Linux 6.12, but **“6.12 or newer” is not a Lertick compatibility guarantee**. Its callbacks, kfuncs and constants can change between kernels without a stable ABI. Lertick must be built and verified against the running kernel. See the [kernel's ABI statement](https://docs.kernel.org/scheduler/sched-ext.html#abi-instability).

Ubuntu's release number is insufficient to identify its kernel. Ubuntu 24.04 [originally shipped Linux 6.8](https://canonical.com/blog/canonical-releases-ubuntu-24-04-noble-numbat), while [HWE and OEM installations](https://ubuntu.com/kernel/lifecycle) can use different kernels. Stock Ubuntu 22.04/24.04 GA kernels are not the tested sched_ext deployment target. Check the running interfaces instead of assuming an Ubuntu version enables them. The helper does not upgrade kernels, edit boot configuration or reboot the machine.

## 2. Install build prerequisites

For Ubuntu 24.04 with its normal repositories enabled:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends \
  git ca-certificates build-essential clang-18 llvm-18 python3 \
  pkg-config libelf-dev zlib1g-dev libzstd-dev util-linux
```

Clang 18 must include its BPF target. Ubuntu lists [clang-18 in Noble's Universe repository](https://packages.ubuntu.com/noble/clang-18). The native build also uses `flock` to serialize builds in one checkout; Ubuntu's base installation normally includes it through `util-linux`. Python 3 runs the local support-report collector. Other Ubuntu releases and Linux distributions may package the required toolchain differently; the commands above do not establish compatibility for those releases.

You also need a working `bpftool` that can dump the live BTF and generate a libbpf skeleton. On Ubuntu releases that supply it through versioned kernel tools, install the tools for the **running** kernel:

```sh
sudo apt-get install --no-install-recommends \
  linux-tools-common "linux-tools-$(uname -r)"
bpftool version
```

Some newer Ubuntu releases provide a standalone `bpftool` package instead. Consult [Ubuntu's package listing](https://packages.ubuntu.com/bpftool) and the diagnostic from `bpftool version`; do not substitute tools for an unrelated kernel when a distribution wrapper requests an exact package. If the required package is unavailable, resolve that requirement before building.

The helper builds the pinned **libbpf 1.7.0** locally under `build/deps`; the first dependency build needs network access to its upstream Git repository. It does not replace the distribution's libbpf. The recorded bpftool 7.7 emits skeleton fields absent from Ubuntu's older libbpf 1.3 development headers, so mixing those headers with the newer skeleton fails. The local dependency build keeps that pairing explicit.

This is a BPF/userspace build. Kernel types come from `/sys/kernel/btf/vmlinux`; Lertick does not build a kernel module against `linux-headers-$(uname -r)`. Linux userspace API headers are still needed by the C toolchain. Installing kernel headers cannot supply missing live sched_ext or BTF support.

## 3. Build, check and verify

Use your normal user account in a trusted checkout:

```sh
git clone https://github.com/Denny-Lin/lertick.git
cd lertick
./deploy.sh check
./deploy.sh verify
./deploy.sh plan
```

The source helper builds the native loader and pinned dependency as needed before requesting sudo. Do not run a fresh source build through `sudo`; build as your normal user. Installation and BPF operations require the permissions reported by the checks.

| Command | What it establishes or changes |
| --- | --- |
| `check` | Inspect the running kernel, live interfaces and current BPF permissions. It does not attach a scheduler. |
| `verify` | Ask the kernel verifier to load Lertick's BPF programs, then release them. It does not attach the scheduler or pin its optional shared maps. |
| `plan` | Print the proposed foreground run without building, sudo, BPF loading or attachment. |
| `status` | Report service and sched_ext state when available. |

Verification is more than compilation, but it is not a scheduling test. Successful registration and actual execution are established only when a run attaches and executes. Verifier-only loading can check a candidate while the current scheduler remains active; it never replaces that scheduler. A new active run refuses to replace an enabled sched_ext scheduler. The managed updater stops only its own service after preparing the candidate; it does not stop a foreign or foreground scheduler for you.

## 4. Install and enable daily use

```sh
./deploy.sh install
lertick status
lertick enable
lertick status
```

The default installation is:

| File | Purpose |
| --- | --- |
| `/usr/local/bin/lertick` | Deployment helper |
| `/usr/local/sbin/scx_lertick` | Native loader with its embedded BPF object |

The installation also includes its management/reporting helpers and a systemd unit. On Ubuntu/systemd, `lertick enable` starts the service immediately and enables it at boot; `lertick disable` stops it and disables boot startup. Installing the files alone does neither. The service remains on Linux's default scheduler if Lertick fails and does not automatically retry in a loop. See [service behavior](DAILY_USE.md#service-readiness-and-fallback).

For a first finite check, stop the service before starting a foreground trial:

```sh
lertick stop
lertick trial --duration 30
```

`run` defaults to **continuous online mode**, all-task model tracking and automatic CPU-performance targets. It runs until stopped; `--duration 0` makes that choice explicit. `trial` defaults to 30 seconds, and `run --duration 30` is also finite. Both schedule the machine's eligible fair-class tasks through sched_ext while active. Daily use remains experimental, and enabling a service does not establish production stability. Keep an independent administrator terminal or VM console for a new-kernel run.

Lertick currently implements no cgroup scheduling callbacks. Per-task weighted CPU-service accounting does not establish cgroup bandwidth or hierarchical isolation. Do not assume `cpu.max`, hierarchical `cpu.weight` or `cpu.idle` retain the default scheduler's behavior during a trial; the kernel documents these as [scheduler-dependent controls](https://docs.kernel.org/scheduler/sched-ext.html#scheduler-dependent-knobs).

Keep the machine awake during an initial trial. Finite duration uses monotonic running time, so time suspended is not counted. Suspend/resume and CPU hotplug are not established by the short deployment checks below; inspect status and re-verify before resuming use after such changes.

Press **Ctrl-C** to stop early. Normal expiry and loader termination detach its unpinned scheduler link, returning tasks to the kernel's default scheduler. Check the exit diagnostics and status afterward:

```sh
lertick status
```

The expected state after Lertick stops, with no other scheduler starting, is `disabled`. The kernel also aborts sched_ext on detected internal errors or runnable-task stalls. That fallback does not establish acceptable performance or prevent every scheduling regression. See [kernel switching and fallback behavior](https://docs.kernel.org/scheduler/sched-ext.html#switching-to-and-from-sched-ext).

For another control mode or fresh telemetry:

```sh
lertick trial --mode shadow --duration 30 \
  --sample-ms 1000 --stats-json lertick-shadow-new.jsonl
```

**Shadow mode is active scheduling.** It trains and reports predictions while retaining fixed slice budgets; it still replaces the scheduler during the trial. The helper's `plan` command is the non-running way to inspect a proposed trial. There is no Lerwatt-style non-actuating `preview` command.

| Mode | Model behavior | Newly assigned slice budgets |
| --- | --- | --- |
| `fixed` | No training or inference | Fixed, 2 ms by default |
| `shadow` | In-kernel training and inference | Fixed, 2 ms by default |
| `ewma` | Historical burst EWMA | Bounded EWMA after warm-up |
| `online` | In-kernel linear model | Bounded prediction after warm-up |

The default bounds are 0.5–5 ms after eight valid labels, with a 2 ms fallback. They bound newly assigned budgets, not exact execution time or the remaining budget of an already running task. Lower prediction error has not established a general throughput or latency advantage; consult the [recorded comparisons](PHASE2_RESULTS.md) and [long-window confirmation](../results/model-confirmation/README.md).

The helper forwards `--mode`, `--duration`, `--sample-ms` and `--stats-json`; tracking and CPU-performance defaults are fixed for this simple path. For targeted tracking, partial switching, full research snapshots, model experiments or optional Lerwatt coordination, use the advanced loader instructions in [deployment and reproduction](DEPLOYMENT.md). Model tracking and the set of scheduled tasks are distinct: selecting a TGID for learning alone does not isolate scheduling to that process.

## Telemetry and permissions

Continuous runs default to compact aggregate health/counters every 60 seconds, without per-task names/PIDs or per-CPU dumps. Explicit finite raw-loader runs retain the older research defaults: full snapshots at one-second intervals. `--full-stats` explicitly enables full research snapshots for a continuous raw-loader run. Full snapshots can contain task names and PIDs; kernel failure diagnostics can contain them too. The service uses journald, whose retention is managed by the host. [Daily logs and report privacy](DAILY_USE.md#logs-and-private-reports) explains the distinction.

`--stats-json FILE` creates a fresh private file with mode `0600`. Choose a new filename for each run; existing files, symlinks and special files are rejected. `--stats-json -` writes to stdout, so ordinary shell redirection can create a user-owned log. Use a directory you trust. Explicit standalone log files are not automatically rotated or size-limited.

System installation requires a root-owned prefix and ancestors that are not group/other writable and do not contain symbolic-link components. The default `/usr/local` is the normal path; a protected prefix such as `/opt/lertick` can be selected with `PREFIX`. A custom prefix's `bin` directory may need to be added to your PATH, or invoke its helper by absolute path.

`DESTDIR` is for package staging, preferably as a normal user. Staging copies files without attaching a scheduler or changing the host kernel. Privileged staging also enforces protected destination paths. The helper does not download distribution packages, change security policy, send SysRq or stop another manager's scheduler. Installing a service unit does not enable or start it; those actions require `lertick enable` or `lertick start`.

Run privileged commands only from source you trust. The installer does not authenticate the repository. A successful check does not override BPF restrictions from your kernel or security configuration; preserve the reported error rather than automatically disabling protections.

## Kernel updates, stop and uninstall

The native helper fingerprints its kernel-derived build inputs and invalidates stale generated files when needed. A manual Make build must still start with `make clean` after changing kernels so that `vmlinux.h`, the BPF object and its skeleton are regenerated from the running kernel. Re-run checks and verification before a new-kernel trial. An installed binary built for the previous kernel does not acquire compatibility simply because the kernel still exposes sched_ext.

Use the managed update path to prepare, verify and replace the installed bundle for the running kernel:

```sh
lertick update
```

The installed updater fetches upstream `main` and builds as your normal user before requesting privilege for staging and activation. For a reviewed local checkout, `./deploy.sh update` builds that checkout instead. Read [update and rollback semantics](DAILY_USE.md#update-and-rollback) before changing a running service.

Stop a foreground run with Ctrl-C. To stop managed daily use and remove the installation:

```sh
lertick disable
lertick status
lertick uninstall
```

Uninstall disables/stops Lertick's managed service before removing the installation. It refuses to abort a foreign scheduler or an independently launched foreground loader; stop those through their owner first. File maintenance can still run after a kernel change when sched_ext interfaces are absent; removing installed files does not require loading BPF or restoring a previous kernel. Keep the original checkout until the scheduler has stopped and its status is understood. On a non-systemd machine, use the [continuous foreground path](DAILY_USE.md#without-systemd).

If an independent administrator needs an emergency kernel abort, the documented mechanism is uppercase SysRq-S. The helper never sends it automatically; it would abort the active sched_ext scheduler, including one owned by somebody else. Follow the [manual recovery instructions](DEPLOYMENT.md#recovery-and-watchdog-test) from an independent control path.

## Tested scope and reproduction

The native deployment checks were completed on **2026-11-02, America/Los_Angeles**, in an **Ubuntu 24.04.5 LTS ARM64 VM running Linux `7.0.0-38-generic`**, with Clang 18.1.3, bpftool 7.7 and pinned libbpf 1.7.0. These tests exercised a running sched_ext kernel, beyond compiling in a container.

The daily-service checks used **systemd 255.4** in that same VM:

| Daily lifecycle check | Recorded result |
| --- | --- |
| Continuous default and readiness | Raw-loader continuous/notify tests passed. The real systemd service stayed active with the same main process for 65 seconds, beyond the former 30-second trial default. |
| Installation and explicit intent | A normal-user build installed files without enabling/starting. Enable, stop, start, restart and disable behaved as documented; stop confirmed kernel-default scheduling. No reboot was needed. |
| Controlled update | Candidate verification ran while the old service remained active. A helper/report-only change produced a distinct full-bundle revision; the active service switched successfully. |
| Failed activation and rollback | An injected `/bin/false` candidate unit failed startup after BPF verification. One recovery restored the old active/enabled bundle and retained the original previous revision. Manual rollback also passed. |
| Stopped-service update | Update preserved inactive state and the existing boot-enabled preference. Explicit rapid start/restart succeeded after the helper cleared the unit's failed/start-limit state. |
| Local reports | Ordinary-user and explicit-root JSON reports parsed, matched installed bundle hashes, omitted raw logs and task identifiers, and did not upload. Root journal access supplied aggregate counters. |
| Cleanup | Disable/uninstall removed the owned unit and boot link. Final state was `sched_ext=disabled`, unit not found/inactive. |

These are short lifecycle checks, not a multi-day soak test. Fixture tests additionally exercise rejected verification, a candidate with broken maintenance/startup, unsafe paths, foreign schedulers, contention and report privacy; they do not substitute for native kernel tests. The earlier finite checks remain recorded below.

| Native VM check | Recorded result |
| --- | --- |
| Clean build as a normal user | Native build downloaded the pinned libbpf source, built it locally and compiled the embedded BPF object and loader successfully. |
| System installation | Normal-user `./deploy.sh install`, with sudo escalation, installed into a protected `/opt` test prefix. |
| Installed inspection commands | `status`, `check`, `verify` and `plan` passed without attaching a scheduler. |
| Ordinary-task learning | A three-second installed online trial with `--track-all` recorded **973 model updates and 974 inferences** for a task named `native-demo`. |
| Concurrent-operation guards | A second run and uninstall were rejected while the trial was active. |
| Normal expiry and removal | The trial returned sched_ext to `disabled`; uninstall then removed the installed helper and loader. |
| Controlled loader exits | Guarded SIGINT, SIGTERM, SIGHUP, SIGKILL and telemetry `EFBIG` failure cases detached the scheduler. |
| Direct-loader verification | Verifier-only loads passed in fixed, shadow, EWMA and online modes. Verification with `--lerwatt` did not pin its shared maps. |
| Actual bpffs integration | A short optional Lerwatt trial created two owned pins in a private `0700` directory and cleaned them on detach. An unsafe pre-existing directory was refused before attachment and preserved. |

Separate portable deployment fixtures passed in the following userspace environments:

| Environment | Checks completed |
| --- | --- |
| macOS host | Runtime unit tests and shell-helper fixtures. |
| Ubuntu 22.04.5 x86_64 container, emulated userspace; GCC 11, Python 3.10 | Portable runtime and helper deployment fixtures. |
| Alpine 3.22, musl | Portable runtime and helper deployment fixtures. |

The portable fixtures do not load or attach Lertick to those systems' kernels. Containers share their host kernel, and emulated x86_64 userspace does not establish physical x86 support. The native VM result does not establish compatibility with every Ubuntu release or kernel, physical-board behavior, energy savings or production readiness. These deployment checks also do not erase the separate [unresolved model-confirmation watchdog event](../results/model-confirmation/README.md#unresolved-fallback-and-omissions).

Historical scheduler benchmarks used four vCPUs; later integration and model-confirmation runs used eight. See [the exact VM deployment record](DEPLOYMENT.md#recorded-environment) for the original environment and preserved experiments.

The original [VM preparation and reproduction guide](DEPLOYMENT.md), [validation suite](../tests/README.md), [benchmark protocol](PHASE2_BENCHMARKS.md) and [third-party licensing](../THIRD_PARTY.md) retain the deeper procedures and evidence. Distro packages for other sched_ext schedulers are not Lertick packages.
