# Ubuntu/QEMU deployment proposal

Research snapshot: 2026-10-09. **Planning repository only:** the project's build,
loader, CLI, workloads and benchmark runner do not exist. No packages were
installed and no experimental BPF program or VM was executed for this research.
Instructions here specify future validation work; they are not a tested
installation guide.

## Intended compatibility target

Use an Ubuntu Linux guest in QEMU. The first intended kernel family is an
upstream-compatible **Linux 6.12.y** with the required configuration. Additional
kernel families and architectures need their own compatibility tests. Linux
6.12 is the first upstream release containing `sched_ext`, but a version number
alone does not mean the distribution enabled it or that this future project
works on it. No Ubuntu release or kernel is currently claimed as supported.

The kernel explicitly does not guarantee a stable `sched_ext` BPF ABI. Match
example/header versions to the kernel, pin all dependency/source commits, and
verify required kfuncs and callback contracts. CO-RE does not solve every kfunc
or scheduler semantic change.
[Versioned kernel interface](https://www.kernel.org/doc/html/v6.12/scheduler/sched-ext.html).

Prefer a distribution kernel exposing the required facilities. If it lacks
them, use an upstream kernel build with configuration changes after approval;
no custom source patches should be needed. Do not replace the host's kernel.
Ubuntu release choice, image checksum, kernel package/build, and exact toolchain
must be frozen in the first successful compatibility manifest.

## Kernel configuration

The v6.12 documentation lists these required BPF/scheduler configuration flags:

```text
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_BPF_JIT_ALWAYS_ON=y
CONFIG_BPF_JIT_DEFAULT_ON=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_SCHED_CLASS_EXT=y
CONFIG_PAHOLE_HAS_SPLIT_BTF=y
CONFIG_PAHOLE_HAS_BTF_TAG=y
```

The `PAHOLE_HAS_*` entries depend on detected build-tool capabilities; do not
assume hand-editing them fixes an incompatible `pahole`. Enable
`CONFIG_MAGIC_SYSRQ=y` for the independent recovery path. `CONFIG_SCHED_DEBUG`,
`CONFIG_SCHEDSTATS`, tracing/perf support and PSI are recommended for diagnostics
when available. Save the complete `.config` rather than only this excerpt.
[sched_ext configuration](https://www.kernel.org/doc/html/v6.12/scheduler/sched-ext.html),
[SysRq setup](https://docs.kernel.org/admin-guide/sysrq.html).

Future read-only guest preflight, using normal kernel interfaces:

```sh
uname -a
cat /etc/os-release
cat /sys/kernel/sched_ext/state
cat /sys/kernel/sched_ext/enable_seq
ls -l /sys/kernel/btf/vmlinux
```

Inspect `/boot/config-$(uname -r)` or the kernel build's saved config for the
flags above. If `state` is absent, diagnose kernel/config/interface availability.
If it is `enabled`, identify the existing scheduler and do not replace it during
this project's initial experiments. Record state/name before and after loading.

## Build and runtime dependencies

Proposed C implementation: a C compiler, GNU make, Clang/LLVM with BPF target,
`bpftool`, libbpf development headers, `pkg-config`, libelf, zlib and zstd
development libraries. `pahole` belongs to a BTF-enabled kernel build. No Rust,
Python ML framework, GPU, cloud account, or paid API is required by the proposed
scheduler runtime. Optional analysis tools and comparison schedulers may have
their own dependencies.

The current C examples advertise Clang >=16 (>=17 recommended), libbpf >=1.2.2
(>=1.3 recommended), `bpftool`, `pkg-config` and link dependencies. These are
upstream example requirements, **not validated scx-sprout minimums**. Freeze a
tested toolchain before adding a project support matrix.
[C example build instructions](https://github.com/sched-ext/scx-c-examples#build--install).

The current Rust `scx` tree instead lists Clang >=18, Rust >=1.91 and additional
requirements for some scheduler forms. Building a pinned `scx_rusty` baseline
may require that environment; keep its dependency manifest separate.
[Current scx build requirements](https://github.com/sched-ext/scx#build--install).

A future reproducible build must derive compatible BTF/header data, compile BPF,
generate the libbpf skeleton, build the C loader, and save tool versions,
commands and artifacts. No `make` command for this project can be run yet.
Loading will initially require guest administrator privileges and a kernel
whose security/lockdown policy permits the attachment. Do not disable host
security features to work around a guest compatibility failure.

## QEMU isolation

Keep a clean Ubuntu base disk, a stopped-VM recovery point, and a disposable
qcow2 overlay per independent experiment. Preserve benchmark artifacts outside
the overlay before discarding it. The base must never be the active write disk.
Do not enable persistent scheduler services inside the base image.

Illustrative host commands, **not run or validated here**, assume a prepared
bootable Ubuntu amd64 qcow2 guest with SSH and a configured serial console:

```sh
qemu-img create -f qcow2 -F qcow2 -b /absolute/path/ubuntu-base.qcow2 experiment.qcow2
qemu-system-x86_64 \
  -machine q35 -accel tcg -cpu max -smp 4 -m 4096 \
  -drive file=experiment.qcow2,format=qcow2,if=virtio \
  -nic user,hostfwd=tcp:127.0.0.1:2222-:22 \
  -nographic
```

This example does not provision the guest, install SSH, or configure a serial
getty. Image/firmware requirements must be adapted to the chosen guest. Use a
same-architecture guest and a supported hardware accelerator for performance
testing: KVM on suitable Linux hosts or HVF on suitable macOS hosts. On an ARM
Mac, an x86 guest under TCG is suitable for functional experiments but supplies
no credible native CPU-performance comparison. Native ARM guest setup requires
its appropriate `virt` machine and firmware.

QEMU documents TCG as software emulation and architecture-dependent
accelerators, user-network port forwarding and console multiplexing. Record
the selected accelerator, QEMU version/machine type, vCPU topology, host load
and guest steal time for every result.
[QEMU introduction](https://www.qemu.org/docs/master/system/introduction.html),
[invocation](https://www.qemu.org/docs/master/system/invocation.html),
[qemu-img](https://www.qemu.org/docs/master/tools/qemu-img.html).

## Proposed project CLI

The following interface is a proposal. **There is no executable to run.**

```text
scx-sprout --check
scx-sprout --mode fixed  --duration 60s
scx-sprout --mode shadow --duration 60s --stats-json run.json
scx-sprout --mode ewma   --duration 60s --stats-json run.json
scx-sprout --mode online --duration 60s --stats-json run.json
```

Future `--check` should explain missing configuration/kfuncs, active scheduler,
map sizing and privilege problems. A foreground loader should own an unpinned
attachment, print the actual loaded scheduler and fallback counters, enforce a
finite deadline, and detach on signals. Its telemetry should show predictions,
completed/censored episodes, updates, selected slices and fallback reasons.
The demo should be observable without claiming a speedup.

First demonstrate fixed scheduling and safe unload. Then validate label
accounting in shadow mode. Only after both succeed should model predictions
select slices. Runtime verification must include observed
`/sys/kernel/sched_ext/state` and the applicable scheduler `ops` sysfs entry,
with paths checked against the selected kernel. See
[benchmark plan](planning/BENCHMARK_PLAN.md).

## Recovery procedure

The kernel documents abort-and-return-to-default scheduling on loader
termination, detected internal errors/stalls, and uppercase SysRq `S`. The
project must actually test its attachment lifetime and handler behavior. Do
not pin or transfer ownership of the attachment in the MVP.
[Kernel recovery paths](https://www.kernel.org/doc/html/v6.12/scheduler/sched-ext.html).

1. Stop the foreground loader normally, then verify `state` is `disabled` and
   the guest remains responsive. SIGTERM/SIGKILL behavior belongs to the later
   disposable-guest recovery tests.
2. From the independent **guest** admin console, if ordinary stop fails,
   request the scheduler abort:

   ```sh
   printf S | sudo tee /proc/sysrq-trigger >/dev/null
   cat /sys/kernel/sched_ext/state
   ```

   Use uppercase `S`; lowercase `s` requests a filesystem sync. The sysrq
   interface is case-sensitive and admin `/proc/sysrq-trigger` use is distinct
   from keyboard enablement. This command is Linux guest recovery, not a macOS
   host command.
   [SysRq semantics](https://docs.kernel.org/admin-guide/sysrq.html).
3. If the guest console cannot execute, allow the configured sched_ext stall
   watchdog to act, observe recovery through the console, and save exit/error
   logs. Its timeout and actual recovery latency must be recorded, not assumed
   instantaneous.
4. If the guest stays unhealthy, use host QEMU controls to stop it and restore a
   fresh overlay from the preserved base. A forced stop may lose guest writes;
   the base/snapshot is the recovery boundary. Save host/serial logs.

Do not use broad process-kill patterns or modify the host scheduler. Do not
equate absence of a watchdog abort with adequate latency or fairness. Kernel
bugs remain possible; use an expendable guest with no important data.

## Backup memory deployment and rollback

The user-space cgroup reclaim backup would require cgroup v2, a memory
controller and PSI interfaces (`CONFIG_CGROUPS`, `CONFIG_MEMCG`, `CONFIG_PSI`
subject to the target kernel), and an experiment-owned/delegated child cgroup.
Check `memory.reclaim`, `memory.pressure`, `memory.stat` and `memory.events` before
enabling actions. Never target the root cgroup or move unrelated services. No
write is the noop action. Rate-limit small positive requests and time each
request; it may over/under reclaim and its duration is not a hard real-time
bound. Avoid changing memory limits in the first controller experiment.
[Versioned cgroup interface](https://www.kernel.org/doc/html/v6.12/admin-guide/cgroup-v2.html).

Stopping a foreground controller prevents further requests, but it does not
undo a request already underway, cache eviction, swap I/O or future refaults.
Capture and restore any changed cgroup parameters, and restore workload/disk
state between independent trials. No sched_ext watchdog protects this control
path.

Optional DAMON diagnostics need `CONFIG_DAMON`, `CONFIG_DAMON_SYSFS`, and the
selected `CONFIG_DAMON_VADDR` or `CONFIG_DAMON_PADDR` backend. Built-in
`CONFIG_DAMON_RECLAIM` is needed only to compare that kernel module, not to use
the generic sysfs interface. Distribution configuration must be checked.
[Linux v6.12 DAMON Kconfig](https://github.com/torvalds/linux/blob/v6.12/mm/damon/Kconfig).

Begin with a `stat` scheme owned by this experiment. Future DAMOS `pageout`
actions require another safety gate: refuse existing active contexts; capture
all configuration; keep an independent supervisor with a finite deadline and
heartbeat; and stop the owned `kdamond` via its `state` file on failure. The
kernel context can continue after its userspace configurator exits. Do not
assume the sched_ext loader-death behavior applies to DAMON. Both quota `ms=0`
and `bytes=0` remove limits unless a goal supplies them; zero quotas do not
disable reclamation. A cleanup must target the recorded owned context only.
[DAMON sysfs lifecycle and quotas](https://www.kernel.org/doc/html/v6.12/admin-guide/mm/damon/usage.html).

Disabling a separately used DAMON_RECLAIM baseline uses its documented
`/sys/module/damon_reclaim/parameters/enabled` control; restore the captured
value afterward. Do not alter an existing administrator's controller. The
module supplements ordinary reclaim and does not replace it.
[DAMON_RECLAIM control](https://www.kernel.org/doc/html/v6.12/admin-guide/mm/damon/reclaim.html).

## Compatibility report required before installation claims

Publish exact Ubuntu image/release, kernel version/config, architecture,
toolchain and source commits, build outcome, verifier log, normal load/unload,
signal/death/SysRq/watchdog outcomes, labels and known limitations. A second
kernel/architecture test is a separate result. Until then, the supported-version
matrix is empty and all commands remain proposals.
