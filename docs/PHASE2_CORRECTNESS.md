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

Task callbacks operate under the kernel's task/runqueue serialization. The
continuation path reads only the previous task on its locked runqueue.
`init_task` initializes private association state; successful init/exit counts
must balance after detach. Per-task coefficients are never updated from a
separate userspace trainer. The global virtual-time race tolerance remains
that of the official weighted `scx_simple` example; finite tests do not prove
universal starvation freedom or fairness.

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
sudo python3 tests/validate_phase2.py --output results/local/phase2-correctness
cat /sys/kernel/sched_ext/state
```

Expected final state is `disabled`. The runner refuses an existing sched_ext
scheduler and terminates only children it owns. Allocation failure injection,
PID-wrap reuse, other kernel versions, x86 and bare metal remain separate work.
