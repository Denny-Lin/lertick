# Phase 2B benchmark protocol

The [benchmark guide](../benchmarks/README.md) defines commands, workload
parameters and raw artifacts. The final measured values and limitations are in
[PHASE2_RESULTS.md](PHASE2_RESULTS.md). Phase 1's four-second comparisons remain
historical functional evidence and are not used to establish Phase 2 benefits.

The primary suite uses five randomized repetition blocks, at least two seconds
of warm-up and ten seconds of measured time for controlled workloads. Each
block randomizes workload and policy order. Policies are default Linux EEVDF,
pinned official `scx_simple`, Lertick fixed, shadow, EWMA and online, plus a
compatible upstream comparator if it actually loads on the guest.

EWMA is a comparison policy with the same ordering, service accounting,
warm-up and slice bounds. It predicts from prior completed episode history;
it does not train the linear model. Shadow trains/predicts with fixed budgets,
isolating learning cost from its scheduling effect. No model coefficient,
feature, learning rate or slice bound is optimized for these benchmarks.

The suite covers CPU-bound, short-burst, wake-heavy, mixed, multicore,
affinity/migration, real Linux scheduler-object compilation and memory-pressure
CPU interference. Kernel compilation is a subset of a kernel build, not a full
distribution kernel build. Compiler descendants opt into learning through
`--track-all`. Memory experiments record PSI and swap activity; allocated bytes
alone are not evidence of reclaim pressure.

Metrics include throughput, job completion percentiles, kernel wake-to-switch
latency, timer-deadline lateness, independent CPU service, utilization, context
switches, observed migrations, fairness and progress gaps. These have distinct
meanings: timer lateness includes timer delivery and userspace return; the BPF
trace observer measures actual sched_wakeup-to-sched_switch latency. Migration
sampling can miss intermediate moves. Kernel compilation uses software perf
counters when available, preserving their raw output.

Prediction accuracy is prequential: predictions are captured at wake, evaluated
against the completed service before updating weights/history, after eight
completed warm-up labels. Linear and EWMA errors are compared on the same
episodes in shadow/online. The target is capped at 10 ms; clipping counts and
raw duration sums identify the boundary of this accuracy claim. In EWMA mode,
legacy `linear_*` telemetry fields contain the active EWMA predictor error;
the report labels them as policy prediction error, never linear-model accuracy.

Kernel BPF runtime accounting records callback execution cost, excluding the
separate wake observer's program IDs. A separate `--profile` study uses optional
in-kernel clocks around inference/training. Those clocks perturb the measured
operation; paired unprofiled trials are needed to assess observer cost. Callback
runtime is not complete scheduler overhead, and virtualization excludes claims
about bare-metal cycles or hardware cache behavior.

Shadow-minus-fixed callback cost includes prediction-error diagnostics and
counters as well as model inference/training. It is learning plus observation
overhead, not a pure arithmetic cost. The internal timers measure a narrower
operation and still include timing-helper cost.

Reports use trial-level medians, min/max, IQR and seeded 10,000-resample bootstrap
intervals. Paired online/comparator changes retain repetition as the independent
unit. Five trials give coarse uncertainty. Many workload contrasts are
exploratory and require confirmation; an interval alone is not a universal
performance claim. Prediction-error/p99 correlations are descriptive and do
not establish causality. Raw trial data, plans, commands and manifests are kept.
