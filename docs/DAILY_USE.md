# Daily use and controlled updates

[← Back to Lertick](../README.md) · [Installation and requirements](INSTALL.md)

Lertick can stay active until stopped and run as an Ubuntu/systemd service. Daily use remains experimental: service management makes deployment and recovery easier, while scheduling performance and long-term reliability still need representative measurements.

## Install once, enable daily use

After the [kernel, toolchain and verification checks](INSTALL.md):

```sh
./deploy.sh install
lertick enable
lertick status
```

Run these commands as your normal user; privileged management asks for sudo. Installation adds the files and unit without starting or enabling it. `enable` means **enable at boot and start now**, equivalent to `systemctl enable --now` for Lertick's unit. A compatible running kernel needs no reboot to begin scheduling. If you first need a different kernel, boot it before verifying and starting Lertick.

| Command | Effect |
| --- | --- |
| `lertick start` | Start the installed service now, leaving its boot preference unchanged. |
| `lertick stop` | Stop this service now, leaving its boot preference unchanged. |
| `lertick restart` | Stop and start this service. |
| `lertick enable` | Enable boot startup and start now. |
| `lertick disable` | Disable boot startup and stop now. |
| `lertick status` | Inspect the service and running sched_ext scheduler. |
| `lertick integration on\|off\|status` | Configure or inspect optional Lerwatt coordination, preserving service intent. |
| `lertick version` | Show the installed version. |
| `lertick update` | Build and verify a candidate, then perform a controlled bundle update. |
| `lertick rollback` | Restore the retained previous bundle, preserving service intent. |
| `lertick report` | Produce a local diagnostic report without uploading it. |

`start` and `restart` are explicit requests to run the service. A stopped service is not automatically restarted by `update` merely because an update succeeded. Other active sched_ext schedulers and foreground scheduler processes must be handled through their own owner; Lertick does not stop them for you.

## Optional Lerwatt coordination

After installing both projects, enable their local link using the [shared integration guide](INTEGRATION.md). Lertick's part is:

```sh
lertick integration on
lertick enable
lertick status
```

Integration is off by default. `integration on` verifies the installed loader's optional BPF path before saving its private setting; if this managed service is already active, it performs a controlled restart while preserving active/enabled intent. `lertick integration off` removes the handoff while keeping the scheduler's own policy. A plain update preserves the mode. Configuration alone does not prove a connected peer; inspect current worker status/logs.

Either project can start, fail or update independently. They do not restart their peer or share model weights. Model-only updates that retain the contract ABI do not require updating or retraining the other project. Mixed incompatible ABIs/topologies disable the link and leave independent operation. The contract is advisory; kernel/firmware protections and each project's own recovery remain separate.

## Service readiness and fallback

The service runs the loader continuously with all-task learning and automatic CPU-performance targets. Its normal output is a compact aggregate summary every 60 seconds. Models start fresh whenever the scheduler attaches; restarting does not preserve a learned checkpoint.

The unit uses `Type=notify`. The loader sends readiness only after confirming its actual sched_ext registration and completing an initial health check. A successful process launch alone is insufficient. This uses systemd's [readiness notification protocol](https://www.freedesktop.org/software/systemd/man/latest/sd_notify.html).

`Restart=no` is deliberate. If the kernel falls back or the loader fails, the service stays stopped/failed instead of repeatedly replacing the default scheduler. Inspect the failure, collect a report and choose whether to restart or roll back. An enabled unit will attempt startup again at the next boot; use `lertick disable` to keep it off across boots. Startup/shutdown timeouts bound service management; they do not prove acceptable application latency. See [systemd's service semantics](https://www.freedesktop.org/software/systemd/man/latest/systemd.service.html).

```sh
lertick status
lertick report
```

After stopping Lertick, sched_ext should be `disabled` unless another owner has started a scheduler. The kernel's normal fallback path also detaches a faulty sched_ext scheduler; that mechanism is distinct from a systemd service watchdog. The helper never sends a global SysRq abort automatically. Manual emergency recovery is documented in the [VM/recovery guide](DEPLOYMENT.md#recovery-and-watchdog-test).

Lertick does not implement cgroup CPU-bandwidth or hierarchical-weight callbacks. Do not assume systemd `CPUQuota`/`CPUWeight`, Docker CPU quotas or equivalent controls retain the default scheduler's resource-policy semantics while Lertick is active. Per-task weighted service accounting and successful service startup do not establish that isolation.

## Foreground runs and finite trials

```sh
lertick run
```

`run` defaults to unlimited duration (`--duration 0`). Ctrl-C stops the owned loader and waits for it to detach. It does not enable startup at boot. Stop the service first if it is already running; a second active scheduler is refused.

For an explicit short trial:

```sh
lertick stop
lertick trial --duration 30
```

`trial` defaults to 30 seconds. `lertick run --duration 30` is also finite. **Shadow mode is active scheduling**, with learning and fixed slice budgets; it is not a passive preview. `lertick plan` prints a proposed command without building, sudo, loading BPF or attaching anything.

The raw loader also defaults to continuous operation. Continuous output defaults to aggregate summaries at 60-second intervals. Explicit finite raw-loader commands retain the older research defaults: full snapshots every second. For full research snapshots during a continuous run, select raw `--full-stats` explicitly; those snapshots include task names and PIDs. Use the [advanced deployment guide](DEPLOYMENT.md) for targeted tracking and experimental flags.

## Update and rollback

```sh
lertick version
lertick update
lertick status
```

The installed updater fetches upstream `main` into a private user-owned temporary checkout and builds for the running kernel. It does not fetch or compile as root. To deploy a reviewed local checkout instead, run `./deploy.sh update` from that checkout as your normal user.

The update sequence is:

1. Build the candidate and fingerprint its running-kernel/BTF inputs while the old service remains available.
2. Stage an exact protected bundle and verify those bytes without attaching it, before stopping the service.
3. Stop only Lertick's owned unit, confirm maintenance is safe, and promote the loader, helper, manager and unit together.
4. If the service was active, start the candidate and wait for actual readiness. Preserve the prior boot-enable and active/inactive intent.
5. If activation fails, restore the previous bundle and attempt its previous active state once. If recovery also fails, leave Linux's default scheduler in place and report the failure.

Verification proves that selected BPF programs can load; it does not prove attachment, long-term stability or faster applications. A failed build or verification must not stop the existing service. Another manager's active scheduler, or an unmanaged foreground scheduler, blocks this service replacement.

To select the retained previous bundle deliberately:

```sh
lertick rollback
lertick version
lertick status
```

Rollback covers the installed bundle, not the kernel, upstream Git history or all host configuration. An older binary may also fail on a newly booted kernel. After a kernel change, rebuild/re-verify before continuing. Suspend/resume and CPU hotplug also require fresh status inspection and verification; automatic rediscovery and uninterrupted behavior through those events are not established by short lifecycle checks.

Promotion replaces fixed files individually while holding the maintenance lock. It is not a crash-atomic filesystem transaction: power loss or a hard kill during replacement can leave mixed files. The local report checks installed bundle hashes. If they differ, keep the service stopped and restore a complete release through rollback or a reviewed source update before resuming.

Explicit `start`, `restart` and `enable` clear this unit's failed/start-limit state before the requested attempt. Automatic update recovery does not clear it or retry indefinitely.

## Logs and private reports

Daily aggregate output excludes per-task names/PIDs and per-CPU dumps. It still reports version, health and scheduler/model counters. The scheduler continues maintaining the internal per-task state it needs to operate. Kernel-generated failure diagnostics may contain task identifiers even when periodic summaries do not.

The service sends output to journald and applies a per-service message rate limit. Retained bytes and history follow the host's journald policy; a rate limit is not a per-service disk quota. Logs may be dropped or unavailable after rotation or reboot. Lertick does not silently change the host-wide journal retention configuration. See [journal rate limiting](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html#LogRateLimitIntervalSec=) and [journal size/retention controls](https://www.freedesktop.org/software/systemd/man/latest/journald.conf.html).

Capture a fresh private report as your normal user:

```sh
(umask 077; set -C; lertick report > lertick-report.json)
```

The default report contains selected version/kernel/service information and strictly filtered aggregate data. It excludes raw journal messages and per-task details, and never uploads anything. It runs with your current permissions; unavailable optional information is recorded as missing rather than silently running sudo. If additional permitted information is needed, `sudo lertick report > another-report.json` runs the collector with explicit privilege while your shell still opens the output file.

Raw log inclusion is a separate choice:

```sh
(umask 077; set -C; lertick report --include-logs > lertick-report-with-logs.json)
```

This includes at most 200 entries with an additional byte limit. Logs can contain names, PIDs, paths or other personal information. Journald also maintains process and host metadata. Review the output before sharing; bounded size does not make it anonymous. Full research telemetry should also be inspected before inclusion in an issue. See the [security policy](../.github/SECURITY.md).

Reports describe observed health, configuration and failures. They cannot infer an application speedup from model counters or prove that the absence of a log entry means no error occurred.

## Improve performance with evidence

Use daily observations to identify a concrete workload or regression. Then compare a frozen baseline and candidate on the same representative inputs, kernel and machine conditions. Include the previous Lertick version and a simple fixed/EWMA or default-Linux control where appropriate. Measure application throughput, p95/p99 responsiveness, CPU overhead and fairness separately from burst-prediction MAE.

Randomize repeated comparisons, retain failures and state the measurement units. A single fast run or lower prediction error is insufficient to change defaults. Isolated tests and a successful update transaction do not establish performance on every machine. The [benchmark protocol](PHASE2_BENCHMARKS.md) and [preserved comparisons](PHASE2_RESULTS.md) show the project's existing measurement boundaries.

A useful report for a maintainer or coding assistant includes:

```text
Workload and expected outcome:
Observed regression, including its units:
Installed version and candidate commit:
Kernel, architecture and relevant configuration:
Exact reproduction and workload inputs (reviewed for secrets):
Baseline/candidate repeated measurements and run order:
Throughput, p99, overhead and fairness observations:
Fallbacks, failures, missing data and recovery result:
Local report and benchmark artifacts reviewed before sharing:
```

For a Codex improvement task, give a bounded request such as:

```text
Investigate this reproduced Lertick regression using the attached reviewed
report and benchmark artifacts. Keep a frozen baseline and a separate candidate.
Identify the bottleneck before changing the model or scheduling policy. Implement
one focused change, run the relevant portable tests and verifier/runtime checks
in the designated test environment, and compare repeated workload measurements.
Report regressions, uncertainty and failures as well as improvements. Do not
deploy to my daily service or change defaults solely because prediction MAE fell.
```

That is a repeatable improvement workflow, not an automatic guarantee that each revision is faster. A report is not uploaded to Codex or converted into a scheduled task by Lertick; you choose what to share and which update to activate.

## Without systemd

Build and install on a compatible native Linux kernel, then use:

```sh
lertick run
```

It remains in the foreground until Ctrl-C, a signal or kernel fallback. Use `lertick trial --duration 30` for a bounded session. The systemd service commands require a working system manager; this guide does not install another init-system integration or a background shell loop.

## Stop daily use or remove Lertick

```sh
lertick disable
lertick status
lertick uninstall
```

`disable` stops now and disables boot startup. Uninstall also disables/stops Lertick's own managed service before removing its files. It refuses a foreign scheduler or independently launched foreground loader; stop those separately through their owner. User reports and separately captured telemetry are not an instruction to delete unrelated data.

## Validation scope

The [recorded native checks](INSTALL.md#tested-scope-and-reproduction) cover the loader and real systemd lifecycle on Ubuntu 24.04.5 ARM64, Linux `7.0.0-38-generic`, systemd 255.4. The daily-service run held the same process for 65 seconds and exercised enable, updates, injected startup failure/recovery, rollback, reports and cleanup. This establishes the continuous service path, not days of uptime, physical-hardware performance or production stability. New lifecycle results should identify the exact environment, version, duration and failure cases exercised.
