# Lertick Linux deployment and reproduction

Phase 1 was approved on 2026-10-09. Lertick now has a C/eBPF implementation,
libbpf loader, real CPU workload, model tests, and a Linux validation runner.
The Linux validation compiled the BPF object and loader, passed the verifier in
fixed, shadow, and online modes, registered `scx_lertick`, and observed real task
execution. Online training updated model parameters inside eBPF and predictions
reached bounded kernel dispatch slices. Normal detach, SIGINT, SIGTERM, SIGKILL,
SysRq-S, and an actual stalled-task watchdog restored the default Linux
scheduler. Twelve short comparison runs completed. The measured outcomes and
limitations are recorded in [PHASE1_RESULTS.md](PHASE1_RESULTS.md); no performance
improvement is claimed. Those original records retain the old ScxSprout name.
Current commands below use Lertick; separate [rebranding regression evidence](REBRAND_VALIDATION.md)
checks the renamed binary while preserving the original records unchanged.

Run scheduler commands inside the QEMU Linux guest. Docker can supply build
dependencies, but its shared host kernel cannot replace this deployment test.

## Recorded environment

| Component | Deployment target |
| --- | --- |
| Host/accelerator | Apple Silicon macOS, QEMU HVF hardware acceleration |
| QEMU | 10.2.1, ARM64 `virt` machine |
| Guest resources | 4 vCPUs, 4 GiB RAM, 20 GiB qcow2 overlay |
| Ubuntu | 24.04.5 LTS, ARM64 |
| Running Linux | `7.0.0-38-generic`, Ubuntu package `7.0.0-38.38~24.04.4` |
| Compiler | Clang/LLVM 18.1.3 with BPF target; GCC/cc for userspace |
| bpftool | 7.7.0, linked to libbpf 1.7 |
| Loader development library | Isolated libbpf 1.7.0, commit `f5dcbae736e5d7f83a35718e01be1a8e3010fa39` |
| Official C reference | `sched-ext/scx-c-examples`, commit `370b1cea95c6bb668d5b9261f069d433142b9986` |

The [environment manifest](../results/phase1/environment.txt) records the actual
running kernel, package versions, configuration checks, BTF/config hashes, CPU
inventory, and virtualization detection. This is one tested environment, not a
claim of compatibility with every Ubuntu release or Linux version. `sched_ext`
interfaces change; another kernel needs a fresh build and runtime test.

## Prepare the QEMU guest on the host

The supplied [VM helper](../scripts/qemu_vm.py) requires Python 3.11 or newer,
`qemu-system-aarch64`, `qemu-img`, ARM64 EDK2 firmware, SSH tools, and either macOS
`hdiutil` or Linux `xorriso`. Run from the repository root:

```sh
python3 scripts/qemu_vm.py --help
python3 scripts/qemu_vm.py prepare
python3 scripts/qemu_vm.py --accelerator hvf start
python3 scripts/qemu_vm.py ssh -- 'sudo cloud-init status --wait'
python3 scripts/qemu_vm.py ssh -- 'sudo reboot'
```

The reboot disconnects SSH. Wait for the guest to return, then inspect it:

```sh
python3 scripts/qemu_vm.py status
python3 scripts/qemu_vm.py ssh -- 'uname -a'
python3 scripts/qemu_vm.py ssh -- 'cat /sys/kernel/sched_ext/state'
```

`prepare` downloads the pinned Ubuntu ARM64 cloud image from
`https://cloud-images.ubuntu.com/releases/noble/release-20260926/` and checks
SHA256 `1d6bffe64b848468ac97f821d369a4846d983de1800ccf6b5ec8853e85cefc55` before
booting. The base image remains read-only; guest changes use
`build/vm/experiment.qcow2`. Cloud-init installs the HWE kernel, Linux tools,
Clang/LLVM 18, C build tools, development dependencies, Python, and Git. SSH is
forwarded only through `127.0.0.1:22222`; this VM's key, console, launch command,
and monitor live under ignored `build/vm/`.

The HWE meta-package may select a different kernel in a later reproduction.
For the recorded target, install `linux-image-7.0.0-38-generic` and
`linux-tools-7.0.0-38-generic` if they are available in the configured Ubuntu
repositories, reboot, and confirm `uname -r`. If those packages are unavailable,
record that exact blocker or validate a newly selected kernel separately.

The helper's `auto` accelerator chooses HVF on a native ARM Mac, KVM on a native
ARM Linux host with `/dev/kvm`, and TCG otherwise. Use `--accelerator kvm` on a
suitable ARM Linux host. TCG can establish functional behavior, but comparisons
under software emulation must be labeled accordingly. This helper currently
provisions ARM64; an x86 guest needs a separate image/firmware setup.

## Copy this branch into the guest

These host commands transfer source without generated BTF headers, VM disks,
private VM keys, or build outputs:

```sh
LERTICK_ARCHIVE="${TMPDIR:-/tmp}/lertick-source.tgz"
COPYFILE_DISABLE=1 tar --no-xattrs --exclude=.git --exclude=build \
  --exclude=results/local -czf "$LERTICK_ARCHIVE" .
scp -i build/vm/id_ed25519 -P 22222 \
  -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
  -o UserKnownHostsFile=build/vm/known_hosts \
  "$LERTICK_ARCHIVE" ubuntu@127.0.0.1:/home/ubuntu/lertick-source.tgz
python3 scripts/qemu_vm.py ssh -- 'mkdir -p /home/ubuntu/lertick && tar -xzf /home/ubuntu/lertick-source.tgz -C /home/ubuntu/lertick'
```

An archive omits Git history. Record `git rev-parse HEAD` on the host and retain
the archive hash when using that method. For the Phase 2 measurements, a Git
bundle preserved the exact committed source and the runner's `source_git_commit`:

```sh
git bundle create build/phase2-source.bundle phase2-correctness-benchmarks
scp -i build/vm/id_ed25519 -P 22222 \
  -o BatchMode=yes -o UserKnownHostsFile=build/vm/known_hosts \
  build/phase2-source.bundle ubuntu@127.0.0.1:/home/ubuntu/phase2-source.bundle
python3 scripts/qemu_vm.py ssh -- 'git clone -b phase2-correctness-benchmarks /home/ubuntu/phase2-source.bundle /home/ubuntu/lertick-phase2-bench'
```

Use a fresh clone directory. Each experiment's `source_git_commit` identifies
its measured implementation. Later documentation changes do not retroactively
change its saved source and binary fingerprints.

Use the helper to run an explicit guest command, or open an interactive session:

```sh
ssh -i build/vm/id_ed25519 -p 22222 \
  -o UserKnownHostsFile=build/vm/known_hosts ubuntu@127.0.0.1
```

## Verify the kernel and build inside Linux

The guest must expose the following configuration and live interfaces:

```text
CONFIG_SCHED_CLASS_EXT=y
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_JIT=y
CONFIG_DEBUG_INFO_BTF=y
/sys/kernel/btf/vmlinux
/sys/kernel/sched_ext/state
```

`CONFIG_MAGIC_SYSRQ=y` is also required for the suite's independent SysRq
recovery test. The recorded kernel additionally enables BPF JIT by default,
PSI, and scheduler statistics. The preflight script reads `/proc/config.gz` or
`/boot/config-$(uname -r)` and fails with an explicit blocker if the required
flags or tools are unavailable.

Run these commands inside the guest:

```sh
cd /home/ubuntu/lertick
scripts/preflight.sh
scripts/build_libbpf.sh
export PKG_CONFIG_PATH="$PWD/build/deps/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
make clean
make CLANG=clang-18 BPFTOOL=bpftool
make test-model
sudo build/scx_lertick --check
```

Ubuntu's packaged `libbpf-dev` is 1.3.0. The recorded bpftool emits a skeleton
using the newer `bpf_map_skeleton.link` field, so those system headers fail to
compile it. [build_libbpf.sh](../scripts/build_libbpf.sh) installs the pinned
libbpf 1.7.0 source into the repository's `build/deps/install` prefix without
replacing Ubuntu's libraries. The original experiment used the equivalent
isolated prefix `/home/ubuntu/deps/install`; select its `lib/pkgconfig` directory
when rebuilding that existing guest. `LIBBPF_CFLAGS`, `LIBBPF_LIBS`, `CLANG`, and
`BPFTOOL` can also be overridden through Make.

The build generates `build/vmlinux.h` from this running guest's BTF, compiles
`src/bpf/scx_lertick.bpf.c` to `build/scx_lertick.bpf.o`, generates a libbpf skeleton,
and builds `build/scx_lertick`, `build/burst_workload`, `build/phase2_workload`
and `build/lifecycle_workload`. Run `make clean` after a
kernel change so a cached header cannot hide a mismatch. `--check` additionally
loads live BTF, finds `sched_ext_ops`, and tests BPF map creation with the current
privileges. Loading refuses to replace an already enabled sched_ext scheduler.

Compilation is one evidence gate. The loader emits `loaded` only after kernel
verifier acceptance and `registered` only after seeing `state=enabled` with
`/sys/kernel/sched_ext/root/ops=scx_lertick`.

## Run a finite scheduling experiment

The implemented modes share the same weighted virtual-time policy and CPU
selection:

| Mode | Model behavior | Slice behavior |
| --- | --- | --- |
| `fixed` | No training or inference | Fixed 2 ms by default |
| `shadow` | Training/inference inside eBPF | Fixed 2 ms by default |
| `online` | Training/inference inside eBPF | Predictions may select bounded slices after eight valid labels |
| `ewma` | Historical EWMA; no linear training | Same bounds and warm-up; EWMA predicts remaining service |

The default prediction bounds are 0.5–5 ms. `--fixed-slice-us`,
`--min-slice-us`, and `--max-slice-us` configure them; the loader requires
`min <= fixed <= max`. Duration is an integer number of seconds, from 1 through
3600, and defaults to 30. `--sample-ms 250` records frequent JSONL snapshots;
`--verbose` enables verifier/libbpf diagnostics. The model's runtime acceptance
and learning outcomes were checked separately for `shadow` and `online` on the
recorded kernel; repeat those checks on another kernel.

A fixed-policy example, inside the guest:

```sh
mkdir -p results/local
sudo build/scx_lertick --mode fixed --duration 15 --sample-ms 250 \
  --stats-json results/local/fixed.jsonl &
LERTICK_LOADER_PID=$!
sleep 1
cat /sys/kernel/sched_ext/state
cat /sys/kernel/sched_ext/root/ops
build/burst_workload --duration 10 --role burst --cpu 0 \
  --burst-us 1000 --sleep-us 2000 --phase-burst-us 6000 \
  > results/local/fixed-workload.jsonl
wait "$LERTICK_LOADER_PID"
cat /sys/kernel/sched_ext/state
```

Run the same experiment with `--mode shadow` or `--mode online` and distinct
output names. The workload names its thread
`lertick_burst`; the scheduler tracks tasks whose `comm` begins with `lertick`, or
a specified `--tracked-tgid PID`. Workload output measures actual thread CPU
time, wake response, completion time, and completed work units.

Scheduler snapshots expose per-task runtime, valid/censored burst labels,
training update counts, five integer model coefficients, predictions, selected
slices, and the slices observed when the kernel actually starts the task.
`matching_dispatch_slices` connects the dispatch budget to the `running`
callback; `adaptive_runs` and bounded adaptive slice extrema provide evidence
that an online prediction reached real execution. Live snapshots are
approximate. Use the final snapshot after the workload exits and scheduler
detaches for the experiment summary.

Labels accumulate `sum_exec_runtime` across preempted execution segments until
a verified wake-to-sleep episode completes. Initial/unverified episodes, exit,
accounting errors, and policy transitions are censored. Continuously runnable
CPU controls must not produce completed training labels. Scheduling counters
alone do not prove the model trained, and updated coefficients alone do not
prove that predictions changed a kernel dispatch budget.

## Run the full Linux validation and small comparison

Fetch and build the pinned official `scx_simple` control inside the guest:

```sh
scripts/fetch_reference.sh
make -C build/reference/scx-c-examples BPF_CLANG=clang-18 BPFTOOL=bpftool scx_simple
sudo python3 tests/validate_linux.py \
  --output results/local/validation \
  --reference build/reference/scx-c-examples/build/scheds/c/scx_simple \
  --repetitions 3 --seconds 4
```

The runner requires `state=disabled` at entry, owns only its child processes,
and saves commands, logs, workload data, scheduler JSONL, and `validation.json`.
It checks preflight; fixed/shadow/online verifier acceptance and registration;
real task runtime; coefficient updates; predictions reaching bounded dispatch
slices; finite/signal/process-death recovery; SysRq abort; and an intentionally
stalled task's real kernel watchdog fallback. Any failed assertion prevents a
complete-validation claim.

The short comparison randomizes three repetitions of the default Linux
scheduler, official non-ML `scx_simple`, fixed Lertick, and online Lertick.
Each run includes two CPU-only controls and one blocking CPU-burst workload,
all pinned to guest CPU 0. It reports measured completion percentiles and work
throughput. These short VM measurements establish a baseline; they do not
justify a performance improvement claim or a production scheduling guarantee.

## Phase 2 correctness and benchmark deployment

Phase 2 source remains on the local `phase2-correctness-benchmarks` branch until
the owner approves publication. Follow [Phase 2 correctness](PHASE2_CORRECTNESS.md)
and the [benchmark guide](../benchmarks/README.md), preserving fresh output
directories for each experiment.

Additional tools used in the recorded guest:

```sh
sudo apt-get install bpftrace stress-ng linux-tools-generic-hwe-24.04 \
  flex bison bc libssl-dev dwarves rustup protobuf-compiler libseccomp-dev
make test-model test-accounting
cat /sys/kernel/debug/sched/preempt
printf full | sudo tee /sys/kernel/debug/sched/preempt
sudo python3 tests/validate_phase2.py --output results/local/phase2a
printf lazy | sudo tee /sys/kernel/debug/sched/preempt
```

These mode commands belong inside the disposable guest. The recorded kernel
originally selects `lazy`; record and restore the selected mode if another guest
differs. The concurrent-init regression requires actual selected `full`, while
the final performance comparison uses the original `lazy` mode.

Rust is needed only for the external upstream comparator. Lertick's model and
scheduler remain C/eBPF. The compatible comparison uses `sched-ext/scx`
`v1.0.16`, commit `e25cc6e5920f33d5bbe2bd62b2e7a5854de88a19`:

```sh
rustup toolchain install 1.91.1 --profile minimal
git clone --depth 1 --branch v1.0.16 https://github.com/sched-ext/scx.git \
  build/reference/scx-v1.0.16
mkdir -p build/reference/tool-bin
ln -s /usr/bin/clang-18 build/reference/tool-bin/clang
cd build/reference/scx-v1.0.16
PATH="$PWD/../tool-bin:$PATH" cargo +1.91.1 build --release --locked -p scx_bpfland -j2
cd ../../..
```

The newer `scx_lavd` v1.1.2 built but failed actual verifier acceptance in
`arena_init` on this guest. It is not included as a successfully deployed
comparator. Preserve its diagnostic rather than changing the test kernel to
improve a benchmark result.

The compile workload uses official Linux 7.0 source, archive SHA256
`bb7f6d80b387c757b7d14bb93028fcb90f793c5c0d367736ee815a100b3891f0`:

```sh
curl -fL https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.0.tar.xz \
  -o build/reference/linux-7.0.tar.xz
printf '%s  %s\n' \
  bb7f6d80b387c757b7d14bb93028fcb90f793c5c0d367736ee815a100b3891f0 \
  build/reference/linux-7.0.tar.xz | sha256sum -c -
tar -xJf build/reference/linux-7.0.tar.xz -C build/reference
make -C build/reference/linux-7.0 O="$PWD/build/kernel-benchmark" defconfig
make -C build/reference/linux-7.0 O="$PWD/build/kernel-benchmark" -j4 kernel/sched/built-in.a
```

The memory-pressure interference workload uses 3,600 MiB of anonymous pages in
the four-GiB VM and a temporary two-GiB swap file. No memory controller is added.
Prepare swap only inside the disposable guest, using a fresh file:

```sh
sudo fallocate -l 2G build/phase2-benchmark.swap
sudo chmod 600 build/phase2-benchmark.swap
sudo mkswap build/phase2-benchmark.swap
sudo swapon build/phase2-benchmark.swap
swapon --show
```

Record actual PSI, reclaim scans and swap counters. Do not infer pressure from
allocation size alone. The [benchmark commands](../benchmarks/README.md) run the
primary repeated suite and separate profiled/observer-control studies. Stop
workloads before removing temporary swap; the recorded VM is shut down after
experiments. Never deploy these fault tests on the host kernel.

## Recovery and watchdog test

The loader owns an unpinned struct_ops link. Normal duration expiry, SIGINT,
SIGTERM, and SIGHUP detach it and verify `state=disabled`. Process death closes
the link's file descriptor; the suite must verify SIGKILL recovery independently
because that path cannot emit a normal final userspace snapshot.

An independent guest admin console can request the kernel abort path:

```sh
printf S | sudo tee /proc/sysrq-trigger >/dev/null
cat /sys/kernel/sched_ext/state
```

Use uppercase `S`. The loader reports an unexpected external/kernel fallback
with a nonzero exit status. The validation runner explicitly expects SysRq's
abort and records its reason.

Watchdog injection deliberately withholds only an identified TGID from
scheduling. It requires all of `--fault-stall`, `--stall-tgid PID`, `--partial`,
and `--dangerous-vm-test`, plus a detected QEMU/KVM guest. The target explicitly
opts into Linux `SCHED_EXT`; SSH and the supervisor remain on the default
scheduling class. The configured watchdog timeout is 1000 ms; observed recovery
latency is measured rather than assumed to equal that timeout. The optional
`--expect-watchdog` accepts only the live kernel's `SCX_EXIT_ERROR_STALL` exit
kind, and fails if that event is absent. Use the runner to reproduce this test
with its stopped-child handshake and cleanup.

After tests, check guest responsiveness and `state=disabled`, preserve results
outside the overlay, and request a guest shutdown from the host:

```sh
python3 scripts/qemu_vm.py stop
```

If ordinary guest recovery is unavailable, stop this project's QEMU instance
through its recorded monitor and retain its serial log before restoring a fresh
overlay from the read-only base.

## Known boundaries

This is a minimal Phase 1 scheduler. It learns per-task CPU-burst estimates and
changes bounded slice budgets; CPU placement, NUMA tuning, advanced ML,
and production service installation remain outside Phase 1. Memory allocation,
reclaim, DAMON/DAMOS, NUMA memory placement, and joint CPU/memory management are
outside the project scope; archived material is background research only.
Root privileges are required in the guest. The official
reference and derived scheduler/helpers retain their GPL notices; see
[THIRD_PARTY.md](../THIRD_PARTY.md). Only outcomes backed by the committed result
artifacts belong in the tested-support claim.
