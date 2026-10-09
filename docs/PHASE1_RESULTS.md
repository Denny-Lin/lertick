# Lertick Phase 1: original Linux deployment results

**Recorded 2026-10-09. Phase 1 passed actual Linux compilation, BPF verifier
acceptance, sched_ext registration, real task execution, in-eBPF online learning,
bounded prediction-driven dispatch, and recovery tests.** These are functional
results from one disposable VM. They do not establish a performance improvement
or production suitability.

The project is now called **Lertick**. These original measurements were made
under its earlier **ScxSprout** name. Every `results/phase1/` file is preserved
byte-for-byte, including `scx_sprout`, `sprout_*`, original paths and hashes.
Current source links use the renamed files; use the [deployment guide](DEPLOYMENT.md)
for current commands. This rebrand does not alter the Phase 1 policy.

## Implementation

- [C/eBPF scheduler](../src/bpf/scx_lertick.bpf.c): official `scx_simple` weighted
  virtual-time queue and default CPU-selection patterns, per-task storage,
  execution-service accounting, wake-to-sleep labels, three policy modes,
  bounded dispatch slices, counters, and retained snapshots.
- [Online model](../src/shared/model.h): five-feature Q16 linear regression with
  projected, error-clipped LMS updates. Features are bias, previous completed
  burst, burst EWMA, sleep interval, and shared-queue occupancy. Training and
  inference run in the BPF callbacks; userspace only reads telemetry.
- [Loader](../src/userspace/scx_lertick.c): live-kernel preflight, libbpf struct_ops
  loading, registration verification, finite duration, unpinned link ownership,
  signal shutdown, JSONL snapshots, final exit classification, and guarded
  one-task watchdog fault injection in a VM.
- [Real workload](../benchmarks/burst_workload.c) and
  [Linux runner](../tests/validate_linux.py): CPU-clock calibrated work,
  phase-changing bursts, continuously runnable controls, recovery trials, and
  randomized baseline order.
- [QEMU helper](../scripts/qemu_vm.py), pinned build dependencies, source
  provenance, arithmetic tests, and [reproducible commands](DEPLOYMENT.md).

The official reference is
[`sched-ext/scx-c-examples@370b1cea95c6bb668d5b9261f069d433142b9986`](https://github.com/sched-ext/scx-c-examples/tree/370b1cea95c6bb668d5b9261f069d433142b9986).
Required headers are copied unchanged. GPL-derived scheduler/loader code and
MIT original model/workload material have explicit [license boundaries](../THIRD_PARTY.md).

Default slices are fixed at 2 ms. Online mode can request 0.5–5 ms after eight
valid labels, using predicted remaining CPU service. Predictions are capped at
10 ms and coefficients at ±4 in Q16. Model output does not choose dispatch
order or modify actual-service fairness charging. Shadow mode trains and
predicts while retaining fixed slices.

Labels accumulate `se.sum_exec_runtime` across preemptions and slice expirations
until a verified wake-to-sleep episode ends. Initial attachment, exit, and
unverified episodes are censored. Slice expiration is not a completed training
label. A continuously runnable task does not train until it has a genuine
completed sleeping episode.

## Actual build and environment

| Item | Observed value |
| --- | --- |
| Host | Apple Silicon ARM64, macOS 26.3.1 (a) |
| Virtualization | QEMU 10.2.1, `-accel hvf -cpu host`, ARM64 `virt` |
| Guest | Ubuntu 24.04.5 LTS ARM64, 4 vCPUs, 4 GiB RAM, 20 GiB overlay |
| Running kernel | `7.0.0-38-generic`, package `7.0.0-38.38~24.04.4` |
| Compiler/tools | Clang 18.1.3 BPF ISA v3, GCC/cc userspace, bpftool 7.7.0 |
| Loader library | Isolated static libbpf 1.7.0, commit `f5dcbae736e5d7f83a35718e01be1a8e3010fa39` |
| Kernel checks | `CONFIG_SCHED_CLASS_EXT`, `BPF`, `BPF_SYSCALL`, `BPF_JIT`, `DEBUG_INFO_BTF`, `MAGIC_SYSRQ` all `y` |
| Live BTF | Readable `/sys/kernel/btf/vmlinux`; hash retained in manifest |

The pinned Ubuntu cloud image initially booted kernel 6.8, which could not
provide sched_ext. Installing the HWE kernel and rebooting resolved this.
Hardware acceleration was actually used and verified in the live QEMU command;
Docker was not used as the runtime.

Inside this Linux guest, the build actually generated `vmlinux.h` from live BTF,
compiled the BPF object, generated its skeleton, compiled the userspace loader,
compiled the workload, and ran the model test executable. The pinned official
`scx_simple` was also compiled and loaded for comparison. The
[build transcript](../results/phase1/final-build.log),
[environment manifest](../results/phase1/environment.txt), and
[tested hashes](../results/phase1/source-and-binary-sha256.txt) preserve evidence.
Remaining compiler diagnostics come from unchanged upstream helper/generated
skeleton code; the build succeeded.

Ubuntu's system libbpf 1.3 headers did not match bpftool's newer skeleton format.
An isolated libbpf 1.7 prefix resolved this without replacing system libraries.
BPF ISA v3 does not support signed division; the model uses tested arithmetic
shift truncation. An initial fully unrolled training program exceeded the
verifier's one-million processed-instruction limit. Bounded five-feature loops
and branchless truncation resolved verifier complexity. The final successful
online-mode verifier log is [retained compressed](../results/phase1/online-verifier.log.gz).
All 13 programs were accepted; the largest processed-instruction count was
5,301, below the one-million limit. The documented VM helper's prepare, HVF
start, status, SSH, and graceful stop commands were also exercised. After its
restart, the same actual Linux kernel passed preflight and a further online
load/unload [smoke test](../results/phase1/qemu-helper-smoke.jsonl).

## Execution and learning

All three modes were loaded into the actual guest kernel. Each passed the
verifier and produced `state=enabled` with `root/ops=scx_sprout`. A pinned burst
worker ran for six seconds, changing from approximately 1 ms CPU service to
6 ms halfway through, with 2 ms sleeps. Final snapshots retain its execution
and completed labels after task exit.

| Mode | Task runs | Completed labels | BPF training updates | Adaptive runs | Newly assigned slice range |
| --- | ---: | ---: | ---: | ---: | --- |
| fixed | 1,365 | 1,347 | 0 | 0 | 2–2 ms |
| shadow | 1,361 | 1,348 | 1,348 | 0 | 2–2 ms |
| online | 1,367 | 1,345 | 1,345 | 1,355 | 0.5–5 ms |

For online mode, changed coefficients, one update per completed valid label,
and real runtime accounting establish in-eBPF training. An intermediate snapshot
also ties a prediction-derived request to the budget observed by the kernel's
`running` callback:

- Training updates: **77**; coefficients changed from the initial `[16384, 0, 0, 0, 0]` to `[9805, -626, -625, -1306, 0]`.
- Predicted service: **1,435,546 ns**.
- Selected slice: **1,435,546 ns**; observed `p->scx.slice` when the task ran: **1,435,546 ns**.
- Adaptive task runs at that snapshot: **70**.

This shows the model changed actual sched_ext scheduling budgets. It does not
mean the task consumed that entire budget: it may sleep or be preempted early.
Previously partially consumed budgets observed on resumed tasks can be below
0.5 ms. The bounds apply to newly assigned slices.

The two continuously runnable CPU controls in each ScxSprout comparison trial
executed real CPU service and produced zero completed labels and zero training
updates. This checks that slice expiration was not misused as a training label.
No accounting errors, task-storage allocation failures, or snapshot-map failures
were observed in these trials.
Allocation failure was not injected. A task-storage initialization failure can
abort initial attachment or reject a fork; it does not guarantee automatic
default-scheduler restoration for every allocation failure.

## Shutdown and fallback

| Test | Result | Observed recovery |
| --- | --- | --- |
| sigint | PASS; struct_ops link closed; state disabled | 0.063 s |
| sigterm | PASS; struct_ops link closed; state disabled | 0.031 s |
| sigkill | PASS; struct_ops link closed; state disabled | 0.031 s |
| sysrq | PASS; disabled by sysrq-S | 0.055 s |
| watchdog | PASS; runnable task stall | 2.508 s |

Watchdog evidence: exit kind **1026**, reason **runnable task stall**, message `sprout_burst[3859] failed to run for 1.492s`, and `default_restored=true`. The final full suite ended with `sched_ext/state=disabled`.

The watchdog trial deliberately withheld one known SCHED_EXT burst task on a
real wakeup, using partial switching so the runner and SSH stayed on the default
class. The kernel reported `SCX_EXIT_ERROR_STALL`, the fault counter was nonzero,
and the withheld workload completed after fallback. A 1-second configured
timeout is not an exact wall-clock recovery deadline; polling and kernel
watchdog cadence affect the observed delay.

An initial CPU-only watchdog target did not block and was kept running by the
kernel, so that attempt correctly failed the watchdog assertion. The final
test uses a genuine sleeping/waking task and checks the exact kernel exit kind.
The loader also classifies final exit reasons after detach to catch fallback
racing normal termination. SysRq intentionally returns loader status 1 because
it is an external fallback; the test expects and verifies that outcome.

## Small baseline comparison

Three randomized repetitions ran each policy for four seconds with one burst
worker and two continuously runnable CPU competitors, all pinned to guest CPU
0. The burst worker requested approximately 1 ms CPU service and slept 2 ms.
`scx_simple` used the official default weighted policy and default slice.
ScxSprout fixed and online shared the same policy and slice bounds.

The values below are medians of three per-run percentiles, not pooled
percentiles. Completion latency is wall time from an intended burst wake to
work completion, including wake delay and runnable contention. Raw work counts,
CPU durations, trial order, and per-run results are retained.

| Policy | Median p95 completion | Median p99 completion | Total burst samples |
| --- | ---: | ---: | ---: |
| default | 3.259 ms | 3.350 ms | 2,420 |
| scx_simple | 20.016 ms | 20.128 ms | 546 |
| fixed | 2.015 ms | 2.031 ms | 3,006 |
| online | 2.023 ms | 3.996 ms | 2,996 |

The official example's longer default slice is a major confounder. The short
VM trial does not establish general superiority, fairness, throughput benefit,
or statistical significance. Fixed and online ScxSprout had close p95 completion
latency, while online p99 was higher in this final trial. There is no
demonstrated ML performance improvement. Calibration,
short warm-up, host scheduling, workload logging, and virtualization can affect
these results. No performance improvement is claimed.

## Tests, limitations, and next step

The independent numerical oracle passed 200,121 cases on macOS and Linux. It
checks signed rounding, clipping, overflow boundaries, normalization, adaptation,
and saturation recovery. Host AddressSanitizer/UndefinedBehaviorSanitizer checks
also passed. Its synthetic holdout accuracy is an arithmetic/model sanity test,
not a Linux scheduling performance result. Documentation links, shell/Python
syntax, and whitespace checks were run locally. Privileged Linux experiments
are not delegated to ordinary CI runners.

The functional Phase 1 deployment has no remaining QEMU, kernel, verifier,
registration, training, or recovery blocker. Validation covers one ARM64 guest
kernel only. x86, other kernels, bare metal, heavy task churn, PID reuse, fork/
exec/migration corner cases, arbitrary affinity changes, nice/cgroup fairness,
adversarial starvation, model poisoning, long-duration drift, and production
operation remain unvalidated. Queue occupancy is an approximate global feature;
task-local models do not generalize across tasks. Wake-to-sleep labels and the
10 ms target cap deliberately exclude or saturate some workload behavior.

One policy limitation was identified by source review: on a still-running
task's slice-expiry continuation, `dispatch` can run before `stopping`. The
prediction budget then subtracts only service already committed by prior
`stopping` callbacks, omitting the current segment until that callback occurs.
A continuation can consequently request more than the exact remaining
prediction. Every newly assigned slice is still bounded, and the completed
sleep label still includes the final segment exactly once. This does not
invalidate the measured training or changed-slice evidence. The next
correctness change should subtract the pending runtime delta locally when
choosing a continuation slice, without charging or training it twice.

The retained `exited` flag denotes release of a sched_ext task association,
including detach; it does not by itself prove OS process exit. The runner waits
for workload completion separately. Voluntary yield is not separately
censored: a later valid sleep labels the whole wake-to-sleep CPU service.

Next, address continuation service and extend task-lifecycle, affinity,
weighted-fairness, and repeated recovery
tests before experimenting with performance. Then run longer randomized trials
on dedicated Linux hardware with frozen-model and EWMA controls. Keep the MVP
small until those tests support a broader claim.

The original validated implementation is commit `1d2c39e`. Publication was
approved subject to rebranding and CPU-only scope. The separate
[rebranding report](REBRAND_VALIDATION.md) records regression checks and
publication. Complete original records and commands remain unchanged in
[the evidence directory](../results/phase1/README.md).
