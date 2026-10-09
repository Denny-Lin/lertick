# Lertick CPU scheduling benchmarks

Phase 2 uses `phase2_workload.c`, `run_phase2.py`, and `wake_latency.bt` for
repeatable comparisons on the real Linux guest kernel. The original Phase 1
workload and evidence remain unchanged. The suite measures CPU scheduling;
the memory working set is an interference workload, not a memory controller.

## Phase 2 protocol

Build the workload on Linux with `make workload`, or independently:

```sh
cc -O2 -g -std=gnu11 -Wall -Wextra benchmarks/phase2_workload.c \
  -o build/phase2_workload -pthread -lm
build/phase2_workload --workload shortburst --warmup 2 --duration 10 \
  --cpus 0,1,2,3 > workload.jsonl
```

Each worker calibrates CPU work and allocates its sample buffer before a common
warmup/measurement window. The memory worker faults its anonymous pages before
the window. Workers buffer all samples and print after finishing, avoiding log
I/O inside the measured loop. CPU work is measured with the thread CPU clock,
rather than assuming a loop iteration has constant duration. Buffers contain at
most 100,000 samples per worker; any overflow fails the runner.

The window record includes the effective selected CPU list; single-CPU cases
use the first requested CPU. Each worker records its actual initial Linux
`sched_getaffinity` mask. The runner checks these masks and all sampled execution
CPUs against the configured workload before accepting a trial. Portable smoke
runs on other systems use an explicit unknown-mask marker.

| Category | Workers and contention |
| --- | --- |
| `cpu` | Eight continuously runnable equal workers on one CPU |
| `shortburst` | Four 200us CPU/1800us sleep workers plus two CPU workers, one CPU |
| `wakeheavy` | Six 50us CPU/500us sleep workers plus two CPU workers, one CPU |
| `mixed` | Two 300us/1700us, two 3000us/2000us and four CPU workers, one CPU |
| `multicore` | The same burst workers plus eight CPU workers, all selected CPUs |
| `affinity` | Eight CPU workers switch pinned CPU every 50 completed work chunks |
| `kernelcompile` | Real parallel rebuilds of Linux `kernel/sched/built-in.a` |
| `memory` | Four CPU and two 300us/1700us workers plus an anonymous page walker |

Burst sleeps begin after each completed CPU burst; missed deadlines do not cause
catch-up jobs. The standalone workload defaults to 1GiB; the runner defaults to
3600MiB for the 4GiB test VM with a temporary 2GiB swap file. Memory size is
configurable with `--memory-mib`. Record memory PSI totals, swap-in/out counters,
direct/kswapd scans and page faults before interpreting the last category as
reclaim pressure: a large working set does not itself prove that reclaim occurred.

Run correctness validation before benchmarking. Inside the isolated Linux VM,
install `bpftrace` and `bpftool`, build Lertick and the pinned official reference,
then run:

```sh
sudo python3 benchmarks/run_phase2.py --output build/phase2-bench \
  --reference /home/ubuntu/reference/scx-c-examples/build/scheds/c/scx_simple \
  --policies default,scx_simple,fixed,shadow,ewma,online \
  --workloads cpu,shortburst,wakeheavy,mixed,multicore,affinity,memory \
  --repetitions 5 --duration 10 --warmup 2 --cpus 0,1,2,3 --bpf-runtime
```

The full suite adds `kernelcompile` to the workload list and requires a real
Linux source tree and a separate configured build tree:

```sh
make -C /home/ubuntu/reference/linux O=/home/ubuntu/kernel-benchmark defconfig
sudo python3 benchmarks/run_phase2.py --output build/phase2-full \
  --reference /home/ubuntu/reference/scx-c-examples/build/scheds/c/scx_simple \
  --extra-scheduler scx_bpfland=/absolute/path/to/scx_bpfland \
  --policies default,scx_simple,fixed,shadow,ewma,online,scx_bpfland \
  --kernel-source /home/ubuntu/reference/linux \
  --kernel-build /home/ubuntu/kernel-benchmark --bpf-runtime
```

Record the source commit, kernel configuration, and compiler. The compile
category builds the target once as warmup, deletes scheduler objects and the
archive, and completes forced real target rebuilds until at least the measured
duration has elapsed. It reports complete build count, actual duration and build
completion times. Each measured build runs under `perf stat` with software
`task-clock`, `context-switches`, `cpu-migrations`, and `page-faults` counters
inherited by actual `make` descendants; raw CSV is retained. Use `--perf PATH` if
the distribution's default wrapper does not support the running kernel.
It does not extrapolate a whole-kernel compilation. Lertick
uses explicit `--track-all` for this category so compiler descendants can learn;
other categories use the usual `lertick` task-name prefix.

The runner randomizes workload and policy order within each repetition with
seed `20261009`. Its default five repetitions use ten measured seconds and two
warmup seconds. Shorter runs require `--smoke` and are labeled smoke-only in the
environment manifest. `--resume` skips completed successful trials while keeping
their raw files. The runner refuses an already active sched_ext scheduler and
terminates only processes that it created.

## Metrics and observer cost

The custom workload reports work throughput, completed burst jobs per second,
CPU service, timer-deadline-to-first-userspace-run latency, deadline-to-burst
completion latency, voluntary/involuntary context switches, observed CPU
migrations, affinity changes, per-worker progress gaps and CPU utilization in
core equivalents. Observed migrations compare CPUs at chunk boundaries and can
miss migrations between samples. Fairness is Jain's index of measured CPU
service among equal continuously runnable workers. Burst workers have different
offered load and are not included in that equal-worker fairness calculation.
No-progress workers fail the trial; the largest observed progress gap is also
reported, since a completed trial alone is not proof of a starvation bound.

The read-only bpftrace observer records true `sched_wakeup`/`sched_wakeup_new` to
the next `sched_switch` for named workload threads and matching compiler tasks.
It samples every 16th wake per `lertick` TID and every wake for `make`, `cc1`,
`cc1plus`, `as`, and `gcc`, preserving exact nanosecond observations. Compiler
descendants are often too short lived to reach sixteen wakes. Preemption is not a wake.
All primary policies use the same observer and any reported lost events fail the
trial. Kernel compilation reports build completion plus compiler wake latency.
The timer deadline metric remains separately
labeled because timer delivery time is not the scheduler's wake timestamp.
The observer uses documented bpftrace tracepoints and
[v0.20.2 timestamp/string helpers](https://github.com/bpftrace/bpftrace/blob/v0.20.2/docs/reference_guide.md).
It explicitly selects `nsecs(monotonic)`, matching the workload's measurement
window even after guest suspend. The earlier bare-`nsecs` observer used BOOTTIME;
its partial experiment is preserved and excluded from the final comparison.

`--bpf-runtime` temporarily enables `kernel.bpf_stats_enabled` and restores its
original value. It snapshots `bpftool -j prog show` for scheduler program IDs
created before observer attachment, excluding observer programs. The recorded
runtime and call counts include calibration, warmup and raw-output time;
per-callback means are more useful than unnormalized totals. This accounting
also has cost. `--profile` enables additional in-kernel training/inference
timers; it is for a separate overhead study, and should not silently replace the
primary performance run. Separate `--without-wake-observer` controls assess
probe perturbation; omit `--bpf-runtime` as well to assess runtime-accounting
effects, retaining the cross-session caveat below.

Run the separate studies sequentially, after the primary experiment, with the
same committed source and binaries:

```sh
sudo python3 benchmarks/run_phase2.py --output build/phase2-overhead-profile \
  --policies fixed,shadow,ewma,online --workloads mixed,wakeheavy \
  --repetitions 5 --duration 10 --warmup 2 --profile --bpf-runtime
sudo python3 benchmarks/run_phase2.py --output build/phase2-observer-control \
  --policies fixed,online --workloads mixed,wakeheavy \
  --repetitions 5 --duration 10 --warmup 2 \
  --without-wake-observer --bpf-runtime
sudo python3 benchmarks/run_phase2.py --output build/phase2-no-observers \
  --policies fixed,online --workloads mixed,wakeheavy \
  --repetitions 5 --duration 10 --warmup 2 --without-wake-observer
```

These use the same repetition seed, but separate experiments occur at different
times. Cross-session contrasts are unpaired and use independent trial resampling;
they do not remove host drift or establish an exact instrumentation tax.

Prediction error is prequential: the wake-time prediction is compared with the
subsequent verified sleep-delimited CPU episode before that label changes the
model. After the initial eight labels, both linear and EWMA errors use the same
10ms-capped CPU-service target. The clipped-target count is retained. Report
MAE/RMSE separately from scheduling latency. EWMA mode's selected prediction is
EWMA, so its policy prediction error is not mislabeled as linear-model accuracy.

Lertick counter/error deltas use the last one-second telemetry snapshot before
the measurement window and the final unload snapshot. These boundaries are
approximate and include some warmup and output overhead. CPU burst truth and
application timings use the workload's exact common window. Optional internal
timers report mean training/inference cost per operation; BPF program runtime
reports whole callbacks including dispatch, accounting and model work.
The recorded guest uses a 24 MHz architectural counter (about 41.7 ns per tick).
A mean below one tick averages quantized intervals across many calls and does
not provide that precision for an individual operation. Timers include helper
cost and do not measure uninstrumented arithmetic or bare-metal cycles.

## Raw results and interpretation

Each trial directory retains `workload.jsonl`, `wake.jsonl`, scheduler telemetry,
stdout/stderr, commands, and `trial.json`. Compile trials retain actual `make`
logs. Top-level `environment.json`, `plan.json`, `commands.json`, `progress.json`
and `summary.json` retain binaries' SHA256, compiler, OS, CPU/memory environment,
randomized order and analysis. Preserve these files, preferably compressed,
with the published result description.

The summary reports medians of per-trial percentiles, ranges, IQRs and seeded
10,000-resample bootstrap intervals. It resamples trials rather than treating
thousands of jobs from one trial as independent repetitions. Online comparisons
are paired by repetition. Positive percentage changes mean higher values, which
has opposite meaning for throughput and latency. Error-versus-p99 correlations
are descriptive within one workload and policy; five repetitions cannot show
causation. Small trial counts, multiple comparisons and QEMU timing limit any
performance conclusion. Confirm promising effects with longer independent and
bare-metal runs. No Linux EEVDF or other scheduler superiority is assumed.

The [Phase 2 evidence archive](../results/phase2/README.md) includes extraction
and integrity-check commands. Once extracted, reproduce the host analysis:

```sh
python3 benchmarks/analyze_phase2.py \
  --primary build/phase2-artifacts/phase2-benchmark-final \
  --study profile=build/phase2-artifacts/phase2-overhead-profile \
  --study observer-control=build/phase2-artifacts/phase2-observer-control \
  --study no-observers=build/phase2-artifacts/phase2-no-observers \
  --output build/phase2-analysis
```

The analyzer rejects incomplete primary experiments and mismatched kernel,
preemption mode, benchmark sources or workload/loader binary fingerprints.
It reports global MAE alongside retained RMSE, cross-checks snapshot coverage,
and keeps separate-session observer contrasts distinct from randomized-block
policy contrasts. It executes no Linux workload and needs only Python's standard
library. The deliberately aborted earlier run is excluded from final analysis.

## Preserved Phase 1 workload

`burst_workload.c` provides controlled CPU bursts with real sleeps and a separate
continuously runnable CPU role. It calibrates work batches with
`CLOCK_THREAD_CPUTIME_ID`, then measures each burst's actual CPU time. Burst mode
initially sleeps for 1ms after calibration to establish a verified wake boundary.
On Linux the task comm is `lertick_burst` or `lertick_cpu`; JSONL includes Linux PID
and TID.

```sh
make workload
build/burst_workload --duration 3 --burst-us 2000 --sleep-us 8000 \
  --phase-burst-us 500 > burst.jsonl
build/burst_workload --duration 3 --role cpu > cpu.jsonl
# Run separate instances concurrently on an explicitly selected Linux CPU:
build/burst_workload --duration 3 --burst-us 300 --sleep-us 700 --cpu 0
```

`--phase-burst-us` changes the target halfway through wall time. `--policy-ext`
explicitly requests Linux scheduler policy 7 for partial-switch tests; successful
policy selection alone does not prove an active BPF scheduler. `--cpu` applies
Linux affinity. Defaults require neither flag.

Burst samples contain `target_cpu_ns`, measured `cpu_ns`, `wall_ns`,
`response_ns` (actual wake lateness after the sleep deadline), `completion_ns`
(deadline-to-burst-completion), and work `units`. Summaries report measured CPU
and wall time and `throughput_units_per_s`. These are workload measurements;
response latency is a wakeup lateness metric, not an application request SLO.
CPU role emits only calibration and final records so logging does not add sleep
episodes during its measured run. Burst-mode logging may affect measurements.

Use identical contention and duration for default Linux, fixed sched_ext and
online sched_ext trials. Retain raw JSONL, scheduler telemetry, environment
manifest and failure logs. A small VM comparison is a deployment sanity check.
The CPU sections of the [historical protocol](../docs/planning/BENCHMARK_PLAN.md)
provide background for broader experiments;
no performance improvement follows from compilation or synthetic model tests.

Bare-metal tests are required before broader CPU-performance conclusions.
Fairness, starvation, task churn, affinity, and scheduling overhead remain CPU
validation topics. Memory-management experiments are outside Lertick's scope.
The archived memory-controller protocol is background research only.
