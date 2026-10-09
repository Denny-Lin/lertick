# Phase 2A correctness and safety

Phase 2 is developed on `phase2-correctness-benchmarks`. The original Phase 1
evidence remains unchanged. This report distinguishes portable arithmetic
checks from actual Linux execution; the final execution results are recorded
in [PHASE2_RESULTS.md](PHASE2_RESULTS.md).

## Service ownership

Linux v7.0 `kernel/sched/ext.c::balance_one()` updates `sum_exec_runtime`
before `dispatch(prev)`. It can keep the previous task running without a
`stopping`/`running` transition. The old slice calculation saw only committed
episode service and therefore could reuse service already consumed by that
continuing task.

The [accounting helper](../src/shared/accounting.h) computes a read-only view:
`consumed = committed + (sum_exec_runtime - runtime_start)` when the task is
running. Continuation dispatch subtracts this complete service from the causal
prediction. It does not advance the watermark or charge virtual time.
`stopping` remains the single owner of those charges, using actual service
and the kernel's task weight. Backward counters and overflow censor the episode
and increment an error counter. Prediction exhaustion keeps the existing fixed
fallback; every newly assigned budget remains bounded.

For real sleep, the audited kernel calls `update_curr_scx`, `stopping(false)`,
then `quiescent`. A final sleeping segment is already committed when the label
is constructed. Slice expiration, preemption, migration, and yield retain the
episode. Verified wake-to-sleep observations train exactly once. Initial
attachment, late opt-in during execution, task exit, and policy transitions are
censored. These callback-order assumptions are kernel-specific.

Late task-name opt-in is now refreshed in continuation and stopping callbacks.
A task can call `PR_SET_NAME` after its first running callback and then continue
for a long time. Previously its initial segment was charged to fairness and
episode accounting but omitted from tracked telemetry. The new callbacks
recognize that opt-in without admitting its unobserved features as a label.

## Lifecycle and concurrency

Live scheduling/model state belongs to task storage. The retained telemetry
map is a bounded 4,096-entry LRU hash; eviction affects observation only.
No scheduling decision reconstructs state from that map. This prevents a long
churn experiment from exhausting the old retained hash. Loader enumeration is
bounded independently of successful lookups because concurrent LRU eviction
can invalidate an enumeration key. Periodic snapshots are approximate; final
reads after detach are stable.

An `exited` snapshot means its sched_ext association ended; unload also ends
associations for tasks that are still alive. The `live_tasks` counter is logical
init-minus-exit accounting. It does not measure per-task kernel allocation bytes
before detach. Owned-map disappearance independently verifies the loader's map
lifetime after unload.

Task callbacks operate under the kernel's task/runqueue serialization. The
continuation path reads only the previous task on its locked runqueue.
`init_task` initializes private association state; successful init/exit counts
must balance after detach. Per-task coefficients are never updated from a
separate userspace trainer. The global virtual-time race tolerance remains
that of the official weighted `scx_simple` example; finite tests do not prove
universal starvation freedom or fairness.

`init_task` is an unlocked callback. On preemptible ARM64, migration pinning
does not prevent another fork caller from running on the same CPU between a
diagnostic counter's load and store. Ordinary struct_ops programs here do not
request a private stack/recursion guard. `INIT_TASKS` and `ALLOC_FAILURES` now
use atomic additions; runqueue-serialized hot counters retain their existing
implementation. This is a statically identified lost-update possibility, not a
reproduced scheduler service-accounting failure. See the pinned Linux v7.0
[fork caller](https://github.com/torvalds/linux/blob/v7.0/kernel/sched/core.c),
[trampoline](https://github.com/torvalds/linux/blob/v7.0/kernel/bpf/trampoline.c)
and [recursion selection](https://github.com/torvalds/linux/blob/v7.0/include/linux/bpf_verifier.h).

A concurrent-fork test runs eight parents pinned to one CPU, each creating
1,000 children. It records the actual dynamic preemption mode and requires
independent completion, bounded telemetry, balanced init/exit counts and
released owned maps. Full preemption is selected for this regression; the
guest's original lazy mode is restored for the final performance experiment.

Owned map IDs can remain briefly visible after the loader exits, then disappear.
The suite measures this drain with a five-second bound and preserves the first
and final inventories. This is compatible with deferred kernel/RCU cleanup;
the mechanism is inferred from kernel lifetime rules, while actual disappearance
is independently measured. Immediate map-ID disappearance is not promised.

## Regressions and independent evidence

- `make test-accounting`: 202,000 independent 128-bit oracle cases and audited
  callback-order traces, including overflow and duplicate stopping guards.
- `make test-model`: 200,121 independent fixed-point arithmetic/model cases.
- [Linux suite](../tests/validate_phase2.py): independent thread CPU-clock and
  `/proc/thread-self/schedstat` service compared against final BPF runtime;
  continuation witnesses capture pending service, validity, warm-up count,
  prediction and selected budget at the same decision.
- CPU, yield, late opt-in, forced affinity migration, mixed concurrent wakeups,
  fork/exec, short-lived children and 4,200 distinct-task churn.
- Actual BPF map type/capacity, per-task exit records, balanced association
  counts, and disappearance of loader-owned task map IDs after process exit.
- Eight equal-weight competitors on four CPUs for nine seconds in each mode:
  nonzero service, Jain index greater than 0.9 and observed progress gaps below
  one second are finite acceptance checks.
- Three repetitions each of SIGINT, SIGTERM, SIGKILL, SysRq-S and a genuine
  runnable-task watchdog stall. Fault injection is guarded and VM-only.

The threaded fixture first blocks on a barrier and then performs an explicit
setup sleep before independent measurement. That can produce one genuine tiny
setup label. Thread CPU controls allow only that documented label; standalone
CPU/yield/late-opt-in controls require zero labels. Measured CPU loops perform
no sleeps. A blanket zero-label assertion for barrier workers would incorrectly
classify a real setup sleep as an implementation failure.

## Reproduce

Inside the disposable four-vCPU Ubuntu guest, after the build in
[DEPLOYMENT.md](DEPLOYMENT.md):

```sh
make test-model test-accounting
cat /sys/kernel/debug/sched/preempt
# Recorded guest originally selects lazy; use full for concurrent init stress.
printf full | sudo tee /sys/kernel/debug/sched/preempt
sudo python3 tests/validate_phase2.py --output results/local/phase2-correctness
cat /sys/kernel/sched_ext/state
printf lazy | sudo tee /sys/kernel/debug/sched/preempt
```

Record the previous selected mode and restore that mode on another guest;
`lazy` is the recorded environment's original value. If full preemption cannot
be selected, report that blocker rather than claiming this stress was covered.

Expected final state is `disabled`. The runner refuses an existing sched_ext
scheduler and terminates only children it owns. Allocation failure injection,
PID-wrap reuse, other kernel versions, x86 and bare metal remain separate work.

## Final measured validation

The final Phase 2A run tested source commit
`109ce711bfad52c9f2ddfe2ba760b29258d58c25` in the four-vCPU Ubuntu ARM64 QEMU
guest on `7.0.0-38-generic`. Full preemption was selected: the dynamic interface
reported `(full) lazy`, where parentheses identify the selected mode. The
[raw correctness results](../results/phase2/correctness.json) and
[independently audited metrics](../results/phase2/correctness-metrics.json)
record this run. All 19 original lifecycle cases, three fairness cases, the
concurrent-fork regression and all 15 recovery attempts passed. The suite
completed with sched_ext `disabled`; successful registration and real task
execution required the kernel to accept the loaded BPF programs.

The burst fixture recorded the following per-task counters. A run is a
`running` callback, not necessarily a new burst. Shadow trains and predicts
while retaining the fixed policy; online applies the learned prediction.

| Mode | Runs | Completed labels | Training updates | Adaptive runs | Selected slice range |
| --- | ---: | ---: | ---: | ---: | --- |
| Fixed | 1,361 | 1,346 | 0 | 0 | 2 ms |
| Shadow | 1,363 | 1,350 | 1,350 | 0 | 2 ms |
| Online | 1,363 | 1,348 | 1,348 | 1,351 | 0.5–5 ms |

Online additionally recorded 1,619 adaptive slice assignments and 1,358
dispatch budgets matching the subsequent running observation. Training updates
equal completed labels in both learning modes. These counters and bounded
slice observations demonstrate in-kernel learning and its scheduling effect;
they do not establish a performance improvement.

Continuation sampling captured 19 observations, of which 18 changed the
selected budget relative to the old committed-only calculation. Deduplicating
the recorded tuples leaves 14 observations, 13 changed. Repeated snapshots
can describe the same decision, so the 19 samples are not claimed as distinct
callbacks. In one witness, committed service was zero and pending service was
1,033,125 ns against a 1,021,728 ns prediction. The corrected calculation
selected the existing 2,000,000 ns exhaustion fallback; the old calculation
would have selected 1,021,728 ns. The selected budgets matched the independently
recomputed pending-service calculation.

### Independent service comparison

All 156 comparisons against thread CPU clocks and procfs passed the declared
tolerances. The maximum absolute BPF-versus-CPU-clock difference was
1,894,455 ns (1.894455 ms). The maximum CPU-clock-versus-procfs difference was
844,916 ns (0.844916 ms).

The 48 long CPU, yield, affinity and late-opt-in controls with at least one
second of independently measured CPU service, including the fairness workers,
had a maximum BPF-versus-CPU-clock difference of 1,718,000 ns (1.718 ms) and
maximum relative difference of 0.0859041%. The worst long control was online
late opt-in: CPU clock 1,999,903,787 ns, procfs 2,000,748,703 ns and BPF
2,001,621,787 ns, with zero completed labels or training updates.

Across all 156 comparisons, the maximum relative difference was 12.326%.
Short fork/exec controls magnify startup and teardown boundary differences:
the independent measured interval and complete BPF association do not have
identical boundaries. Passing the absolute/relative tolerance for those
controls does not support a universal sub-percent service-accuracy claim.

### Finite fairness and lifecycle results

Each fairness run used eight equal-weight competitors on four CPUs for nine
seconds. Every worker received service; all Jain indices exceeded 0.9 and all
observed progress gaps stayed below the one-second acceptance bound.

| Mode | Jain index | Maximum observed runnable progress gap |
| --- | ---: | ---: |
| Fixed | 0.999999954534 | 7.046628 ms |
| Shadow | 0.999999945728 | 30.658648 ms |
| Online | 0.999999519596 | 74.701708 ms |

These finite equal-weight observations do not prove universal starvation
freedom, fairness under adversarial workloads, or long-term production safety.
The audited callback ordering is specific to Linux v7.0; this execution does
not establish compatibility with other kernels or architectures.

The original churn case independently completed 4,200 children and retained
3,787 task snapshots, including eight recent children. The added full-preemption
case ran eight overlapping parents pinned to CPU 0, each creating and reaping
1,000 children. It recorded 8,000 distinct child TIDs and 8,000 independent
completion records. The known lower bound of new associations was 8,008
(children plus parents), and the observed initialization delta was exactly
8,008. After detach, global initialization and exit counts both equaled
8,160, with zero live associations and zero counter, allocation or snapshot
failure counters.

The concurrent case retained 3,745 child snapshots and 3,753 total snapshots,
within the 4,096-entry bound. Expected LRU eviction affects telemetry, so these
retained entries cannot enumerate every completed child. The regression proves
the independent completion count, the association-count lower bound and exact
final global balance. It does not prove that every possible lost-update
interleaving occurred, inspect every live task-storage allocation, or test
allocation failure injection.

The [actual kernel map inventory](../results/phase2/map-footprint.json)
reported a 576-byte task-storage value and a 440-byte snapshot value. The
4,096-entry LRU map reported 2,098,048 bytes of
memlock, including map overhead; its raw snapshot values account for
1,802,240 bytes. Live task storage uses one 576-byte value per active
association plus kernel overhead. Its total depends on the number of live
tasks and is not bounded by the retained snapshot capacity. Kernel map
metadata is not a complete measurement of total live task-storage allocation.

All 23 owned-map release checks across the original lifecycle, fairness and
concurrent-fork cases observed disappearance of the loader-owned task-storage
and snapshot map IDs within the five-second bound. The maximum measured drain
was 172.476994 ms. Disappearance was polled, so this is an observed latency
including polling overhead, not the exact instant of kernel deallocation.

### Recovery observations

Each mechanism was tested three times. All 15 attempts restored the default
Linux scheduler, balanced final association counters and left sched_ext
disabled. Recovery timings are observed elapsed seconds, including polling.

| Mechanism | Repetition 1 | Repetition 2 | Repetition 3 |
| --- | ---: | ---: | ---: |
| SIGINT | 0.064645078 | 0.031499459 | 0.031518715 |
| SIGTERM | 0.031603122 | 0.031594877 | 0.031859674 |
| SIGKILL | 0.031407664 | 0.031709961 | 0.031439799 |
| SysRq-S | 0.051843149 | 0.050629529 | 0.052668290 |
| Watchdog | 2.489730008 | 2.505141662 | 2.510805226 |

The watchdog cases produced a real runnable-task stall, a positive injected
stall counter and sched_ext exit kind `1026`; the SysRq cases reported exit
kind `67`. Fault injection was restricted to the disposable guest. Successful
recovery in these attempts does not replace broader fault and sustained-load
testing.
