# Benchmark plan

> Historical Phase 0 planning snapshot. The user approved ScxSprout on 2026-10-09. Implementation and actual Linux validation now exist; see [Phase 1 results](../PHASE1_RESULTS.md), [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md). Statements below describe the original planning stage.

Research snapshot: 2026-10-09. All procedures below are proposed. No Linux,
QEMU, verifier, training-overhead, or performance result has been collected.

## Questions and release gate

Separate three questions: does the model predict capped completed bursts; does
using its predictions change the intended policy; and does that policy improve
useful workload outcomes at acceptable overhead? A lower prediction error alone
does not answer the last question. The project may publish negative results.

Before opening a run matrix, predeclare workload parameters, tuning budget,
slice bounds, warmup, measurement durations, seeds, repetitions, stopping
criteria, and practically meaningful regression limits. Proposed first release
budgets are under 1% of total guest CPU capacity for incremental model work,
under 2% throughput regression on the CPU-bound controls, and no unexplained
OOM, scheduler abort, or starvation event. These are engineering targets to
review before testing, not measured properties or universal safety bounds.

## Comparable policies

| Policy | Purpose | Controls |
| --- | --- | --- |
| Kernel default fair scheduler | User-relevant Linux baseline | Record exact kernel, knobs, cgroups, affinity, topology |
| Matching upstream `scx_simple` | Small non-ML `sched_ext` reference | Pin source matching target kernel; default weighted-vtime mode |
| Pinned `scx_rusty` | Mature `sched_ext` reference | Record compatible commit, build and options; defaults plus equal-budget tuning |
| scx-sprout fixed slice | Isolate scaffolding/accounting cost | Same dispatch, accounting, bounds and telemetry as ML |
| scx-sprout EWMA | Non-ML adaptive baseline | Same labels and slice mapping; tune decay within equal budget |
| scx-sprout frozen regression | Separate inference from training | Train on disjoint phases; freeze coefficients |
| scx-sprout online regression | Full proposed policy | Predict before update; reset state at independent run boundary |
| scx-sprout shadow learner | Separate prediction from policy | Fixed slices, model still predicts/updates |

Current C examples also live in `sched-ext/scx-c-examples`; do not assume
`scx_simple` is still built from the current Rust `scx` tree.
[C examples](https://github.com/sched-ext/scx-c-examples),
[kernel v6.12 example](https://github.com/torvalds/linux/blob/v6.12/tools/sched_ext/scx_simple.bpf.c).

The `scx_rusty` maintainers describe a multidomain BPF/Rust scheduler and say
it can be production-ready when appropriately tuned, while noting topology and
weight limitations. That is a reason to include it, not evidence of its result
on this project. Its larger userspace dependency set belongs to the benchmark
environment, not the proposed minimal C runtime.
[Maintainer description](https://github.com/sched-ext/scx/blob/main/scheds/rust/scx_rusty/README.md).

Apply all policies to the same workload/scheduling scope. Partial-switch mode
is useful for safety bringup, but changes priority-class interactions; do not
compare a partial-mode candidate against a whole-system baseline and claim
equivalence. Document whether machine services participate. Keep each tuned
baseline's full settings and separate tuning data from report data.

## Workload matrix

| Workload | Proposed setup | Main question |
| --- | --- | --- |
| Known bursts | Repeated CPU work followed by sleep, with known generated service targets | Label accuracy, leakage, changing burst predictability |
| Pure CPU saturation | Independent always-runnable workers at 1x and 2x vCPU count | Missing labels, overhead, fallback behavior |
| Latency with interference | Open-loop periodic CPU/sleep worker plus saturating background workers | Wake-to-run p95/p99 and application deadline misses |
| Abrupt phase changes | Short/long/bimodal bursts with randomized phase order | Adaptation lag and censoring bias |
| Mixed weights | Equal-affinity runnable workers with documented task weights | Weighted shares and maximum runnable wait |
| Yield/migration stress | Explicit yields, affinity changes, forks/exits, policy transitions | Accounting validity and state cleanup |
| Real workload | A pinned kernel build or another reproducible CPU workload plus latency probe | Whether microbenchmark gains transfer |

Workload generators are future work. Known bursts must use a calibrated CPU
service criterion rather than an unvalidated wall-clock busy-wait, particularly
inside a VM. Repeat with one and four vCPUs; larger or NUMA topologies are
outside the initial support claim. Include workloads with weak/no burst
predictability to expose overfitting. Do not choose only workloads that sleep
frequently enough to train the proposed model.

For application tail latency, the load generator must maintain a documented
offered load and report drops/backlog. Closed-loop clients can hide long stalls
through coordinated omission. Use enough requests for p99, report sample count,
and keep raw distributions; do not infer a reliable p99 from a few hundred
observations.

## Reproducible run procedure

1. Freeze source commits and guest image checksum. Record kernel config/BTF,
   kernel command line, compiler/libbpf versions, scheduler options, model
   parameters, workload binaries and host/QEMU configuration.
2. Use an idle, dedicated host where feasible. Keep host power/frequency policy,
   vCPU affinity, acceleration and guest topology constant; record steal time
   and background load. TCG runs are integration evidence, not hardware
   scheduler-performance evidence.
3. Boot a fresh disposable overlay or restore an equivalent stopped-VM image.
   Clear model state at each independent trial. Keep caches/dataset preparation
   identical; never drop caches only for one policy.
4. Confirm default scheduling, load exactly one selected scheduler, and record
   observed state/name. Verify the finite experiment deadline and independent
   recovery channel before starting pressure.
5. Use an initial proposal of 30 seconds warmup followed by 120 seconds measured
   time for microbenchmarks, with at least 10 independently reset runs per
   policy. Validate whether these durations/sample counts suffice; increase
   them when a predefined precision requirement is unmet.
6. Randomize policy order in paired blocks and use the same workload seeds per
   block. Capture full-system metrics and application output. Separate high
   tracing overhead diagnostic runs from primary performance runs.
7. Unload, verify return to default fair scheduling, and preserve logs,
   failures and raw samples. Mark failed runs; do not silently discard them.

No benchmark script or command interface is implemented. A future runner
should encode this sequence, provide `--dry-run`, refuse an existing scheduler,
and always attempt owned-resource cleanup.

## Metrics and interpretation

| Metric | Proposed observation | Interpretation caveat |
| --- | --- | --- |
| Throughput | Completed application operations per second | Report offered load and work size |
| Application p50/p95/p99 | Request/deadline timestamps | Different from scheduler waiting time |
| Wake-to-run delay | Correlated scheduler wakeup/switch events | Validate task identity and clock domain |
| Queue/runtime time | Scheduler trace and per-task counters | CPU migration/exit can terminate intervals |
| Context switches and migrations | Available `perf stat` events | Verify PMU/events in VM; mark unavailable |
| CPU consumption | Process and system counters | Include loader and BPF work, not just model instructions |
| Fairness | CPU service relative to runnable demand and weights | Restrict weighted-share calculation to simultaneously backlogged tasks |
| Starvation | Maximum continuous runnable wait, misses, watchdog events | No watchdog event does not prove absence of harmful waiting |
| Model quality | Predict-before-update MAE and relative error | Break out capped targets, eligible tasks and censored episodes |
| Adaptation | Error and policy trajectories after phase changes | Include reset and initial-state policy |
| Resource footprint | Map memory, loader RSS, events/losses | Scale across task count and fork churn |

For continuously backlogged tasks, let `s_i` be measured CPU service and `w_i`
be the configured weight, and calculate Jain's index on `z_i = s_i / w_i`:
`J = (sum z_i)^2 / (n * sum z_i^2)`. Publish per-task shares and maximum waits
too; a single aggregate fairness score can hide starvation. Nice/task weight
tests do not establish cgroup group fairness. Latency and throughput must be
reported together, because shorter slices can trade one for the other.

The kernel's `perf sched` documentation describes record/latency/timehist
analysis. Use matched `perf` tooling and preserve raw trace data; check missing
events and overhead before treating it as ground truth.
[perf sched documentation](https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/perf-sched.txt).

## Training and inference overhead

Measure both per-operation costs and full-system impact:

- Fixed policy with all model paths disabled.
- Shadow inference enabled, updates disabled.
- Shadow inference plus online updates at explicit sampling rates.
- Model used for slices, same training rate.
- Telemetry disabled versus sampled telemetry to quantify instrumentation cost.

Add sampled entry/exit timestamps to prediction and update helpers with a
documented sampling rate. Include an empty timing path to estimate timestamp
cost; report that subtraction is approximate. Preserve call counts so sampled
ns/op can be converted to estimated CPU time per second. Report p50/p95/p99,
worst sampled value, train/infer counts and sampling uncertainty. Disable
instrumentation for main performance runs. Timing the full callback does not
isolate the model unless the dispatch/map/accounting components are held equal.

Inspect translated/JIT instruction sizes and verifier logs, and record map
accesses and update rates. These explain results but do not replace elapsed
time/CPU measurements. BPF profiler availability and PMU accuracy depend on
kernel and VM capabilities; unavailable observations must remain unavailable.
Measure allocation failures, migration-related state misses and fixed-policy
fallback frequency. If overhead scales unexpectedly with task count, stop
expanding the matrix until the design is corrected.

## Statistics and artifacts

Report every independently reset run, paired median effects and bootstrap
confidence intervals across runs; do not treat millions of requests within one
run as millions of independent experiments. Preserve per-run tail distributions
and sample counts. Predeclare one primary latency workload and a throughput
regression control; label the broader matrix exploratory. Negative or mixed
results are valid evidence.

Each submitted result should contain environment JSON, policy/workload config,
seed, commits, guest image checksum, kernel config, raw application samples,
system counters, model/censoring counters, failure log, summary CSV and the exact
analysis version. Include a provenance manifest and command transcript. Keep
addresses, usernames and unrelated workload data out of public submissions.

## Recovery drills

Before performance claims, demonstrate normal stop, SIGTERM, abrupt loader
death, explicit uppercase SysRq `S`, and deliberate stalled-task watchdog
recovery in a disposable guest. Deliberate broken scheduler tests require
separate reviewed test code after implementation approval. Record measured
recovery time, exit reason, `/sys/kernel/sched_ext/state`, service responsiveness
and absence/presence of guest errors. The kernel defines these fallback paths;
successful project integration is still untested.
[Kernel fallback contract](https://www.kernel.org/doc/html/v6.12/scheduler/sched-ext.html).

## Backup memory-controller experiment

Hold the default CPU scheduler and exact MGLRU state constant. Compare no
controller, observer-only telemetry, fixed cgroup reclaim requests, a non-ML
EWMA/threshold controller, and frozen/online contextual action selection using
identical bounded arm sets. Add native DAMOS goal tuning only in a later matrix
with identical DAMON target/action scope; systemwide DAMON_RECLAIM is not an
equivalent baseline for single-cgroup actions.

Use phase-controlled hot/cold file mappings and mixed anonymous/file working
sets, with pinned swap/I/O settings, allocations/frees, and application latency.
Collect cgroup PSI total deltas, refaults, `memory.events`, memory usage, swap and
I/O, controller cost, request durations/return codes, and completion rates.
PSI reports stall time and its totals permit interval deltas; it is not an
application latency measurement.
[PSI semantics](https://docs.kernel.org/accounting/psi.html).

Noop must perform no reclaim write. Log requested bytes separately from
estimated realized reclaim; `memory.reclaim` can reclaim more/less than requested
and `EAGAIN` is not automatically a fatal error. Memory-usage deltas include
workload allocation/free activity, so treat learned rewards as experimental,
confounded estimates unless independently instrumented. Eviction effects
persist after stopping: start independent trials from equivalent cold/warm
states rather than asserting instant rollback.
[cgroup reclaim semantics](https://www.kernel.org/doc/html/v6.12/admin-guide/cgroup-v2.html#memory-interface-files).

If DAMOS actions are introduced, also compare native goal-based feedback,
measure action statistics rather than memory usage alone, and drill supervisor
loss with `state=off` cleanup. This backup matrix does not establish a CPU
scheduler result or a memory-performance improvement.
