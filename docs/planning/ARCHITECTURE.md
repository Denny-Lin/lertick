# Architecture proposal

> Archived Phase 0 background research, superseded by Lertick's CPU-only scope.
> Original names (ScxSprout, scx-sprout, and linux-resource-learning), proposed
> phases, and statements below are preserved as historical reasoning. Memory
> allocation, reclaim, DAMON/DAMOS, NUMA memory placement, and joint CPU/memory
> management are background research only, neither active nor backup development
> directions. See the [archive index](README.md), [current Phase 1 results](../PHASE1_RESULTS.md),
> [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md).
> Phase 2 implementation has not been approved.

Research snapshot: 2026-10-09. Status: design only. No scheduler, loader, model,
memory controller, or Linux runtime validation exists in this repository.

## Decision and hypothesis

The provisional primary project is **scx-sprout**: a small `sched_ext` scheduler
that learns a task's capped CPU burst from completed workload episodes and uses
the prediction to select a bounded execution slice. The project retains a
weighted virtual-time scheduling policy. It does not let model scores become
unbounded priorities or change task weights.

**Untested hypothesis:** for some repeatedly waking workloads, online predictions
may choose useful slices with less tuning than a fixed slice or EWMA. A useful
prediction need not improve scheduling: the policy benefit must be established
separately. This is not a claim of being the first learned scheduler.

CPU scheduling is preferred provisionally because completion labels can be
audited against known synthetic bursts and `sched_ext` has kernel fallback
mechanisms. Memory reclaim has a useful alternative scope but slower,
confounded feedback and effects that cannot be instantly reversed. Reject the
CPU direction if reliable labels or measurable policy benefit do not survive
the feasibility gates below.

## Components

| Component | Proposed responsibility | Runtime dependency |
| --- | --- | --- |
| C userspace loader | Check compatibility, load/attach BPF, export counters, own attachment lifetime, handle signals | libbpf and its link dependencies |
| C/eBPF scheduler | Default CPU selection, weighted virtual-time DSQ, runtime accounting, fixed fallback slice | Compatible `sched_ext` kernel |
| Shared integer model routines | Feature clipping, prediction, incremental update, saturation | C integer arithmetic |
| Per-task state | Episode accounting, recent histories, coefficients, eligibility and confidence counters | BPF task storage, subject to callback/allocator validation |
| Per-CPU telemetry | Aggregate predictions, updates, fallbacks, censoring, sampled cost | Small BPF maps |
| Separate benchmark tooling | Workloads, paired run orchestration, provenance and statistical analysis | Optional development tools |

Kernel-facing sources should follow Linux C conventions. Start with one
topology: one socket/cache domain and a small uniform VM. NUMA, cgroup group
fairness, custom CPU placement, memory coordination, neural networks, and
reinforcement learning are deferred. Task `nice` weights alone do not establish
cgroup-level fairness.

The source of reference is the matching upstream kernel's example scheduler.
Linux v6.12 `scx_simple` implements global weighted virtual-time ordering and
demonstrates an idle-local fast path. Its comments explicitly discuss yield
accounting limitations. Preserve an attribution trail for any later reuse.
[Reference source](https://github.com/torvalds/linux/blob/v6.12/tools/sched_ext/scx_simple.bpf.c).

## What the model learns

An episode starts at a verified wakeup and ends at a verified sleep. The target
is accumulated **scheduler-accounted CPU service** across all execution segments
of that episode, capped at a documented reference duration. It is not wall-clock
request latency, waiting time, or the duration of one slice.

The callback state machine is an implementation gate. Linux v6.12 documents
`runnable`, `running`, `stopping`, and `quiescent`, but migration and attribute
changes also cause state notifications. Candidate sleep completion must check
`SCX_DEQ_SLEEP`; `stopping(..., false)` alone is insufficient. Keep episode state
across ordinary preemption and migration; invalidate it when continuity cannot
be established. Exits, explicit yields, attach-in-the-middle, detach,
unsupported policy transitions, counter anomalies, and open episodes at the
measurement boundary receive explicit censored reasons.
[Callback contract and ordering](https://github.com/torvalds/linux/blob/v6.12/kernel/sched/ext.c).

Take runtime deltas from an audited, verifier-accessible
`p->se.sum_exec_runtime` path. The v6.12 field documentation specifically warns
that `p->scx.slice` may be cleared by preemption and recommends
`sum_exec_runtime` for accounting. Do not treat `initial_slice - remaining_slice`
as a burst label. Confirm callback timing and CO-RE access on each tested kernel;
fall back or stop if that contract cannot be established.
[Runtime accounting contract](https://github.com/torvalds/linux/blob/v6.12/include/linux/sched/ext.h).

Use only information available at episode start: a bias, the previous completed
burst, an EWMA of completed bursts, the previous sleep interval, and a clipped
queue-load indicator. Record the feature snapshot before the target is known.
Long continuously runnable tasks may provide no completed labels. Report their
coverage separately; never manufacture completion at slice expiry.

Completed-only labels favor tasks that sleep. The first experiment must quantify
this selection bias with always-runnable controls, mixed bursts, and holdout
phases. Report completion, censoring, and cap saturation by workload class.
Prediction claims apply only to the population with reliable labels. A capped
target explicitly learns `min(service / reference_service, 1)`; it is not an
estimate of uncapped long bursts.

## Meaningful incremental learning

Proposed first model: per-task five-feature online linear regression using a
projected, error-clipped LMS update in fixed-point arithmetic. Coefficients change in response
to completed real episodes; this distinguishes the model from a fixed heuristic.
Per-task ownership avoids a shared model lock in the hot path but sacrifices
cross-task transfer and requires enough samples per task.

Let `Q = 65536`, features `x` and target `y` be normalized to `[0, Q]`,
coefficients `w` be bounded signed Q16.16 integers, and learning rate `alpha`
also be Q16.16. Proposed bounds are `W = 4 * Q` and `0 < alpha <= Q`, with
`alpha = Q / 64` as an initial tuning candidate:

```text
raw        = sum(w[j] * x[j]) / Q
prediction = clamp(raw, 0, Q)
error      = clamp(y - raw, -Q, Q)
w[j]       = clamp(w[j] + alpha * error * x[j] / (Q * Q), -W, W)
```

The formula is a specification, not an implemented algorithm. Error clipping
and coefficient projection intentionally modify ordinary squared-error SGD;
the learning residual uses the raw linear output, not the clipped inference
value. For five features with these bounds, the absolute dot-product is at most
`20 * Q * Q` and the update product is at most `Q * Q * Q`, both within signed
64-bit range. Specify integer evaluation order, division rounding and saturation.
Avoid undefined signed overflow. Host tests must compare all boundary cases
against a high-precision reference. Fixed-point and floating-point model quality
must be compared offline before accepting quantization.

Start in a fixed-slice warmup mode. Only reliable completed episodes update the
model. Insufficient samples, state allocation failure, invalid features, missing
accounting, excessive prediction error, and unsupported transitions use the
documented fixed slice. The proposed confidence threshold and adaptation rate
are tuning parameters, not proven constants.

Predict total capped service at wakeup. On a resumed episode, derive remaining
predicted service from accumulated actual service. Clamp the selected slice to
`[slice_min, slice_max]`. If service already exceeds the prediction, use the
fixed fallback slice for the remainder of the episode; otherwise a severe
underprediction could force a CPU-bound task into repeated tiny slices.
The exact slice bounds must come from experiments. A slice cap is not a hard
response-time guarantee.

## Scheduling invariants

1. Charge virtual time from actual accounted execution, scaled by task weight;
   never charge predicted service as if it were actual CPU use.
2. Preserve the fixed weighted virtual-time dispatch order. Model output only
   selects a bounded slice; cap any sleeper credit independently of predictions.
3. Keep a documented idle-CPU path and honor affinity. Validate accounting on
   both local dispatch and the shared DSQ.
4. Do not withhold runnable tasks to obtain labels. Missing model state must
   degrade to the fixed policy.
5. Treat fairness and maximum runnable wait as measured release criteria.
   Weighted virtual-time accounting is a design constraint, not a proof that
   every implementation is fair or starvation-free.

## Verifier and concurrency feasibility

Keep the feature dimension compile-time fixed, loops bounded, model buffers in
maps, and per-callback work independent of task count. The kernel documents
multiple verifier complexity/state limits; its approximately one-million
processed-instruction complexity budget is not a runtime instruction budget or
a promise that this design will load. Older text in the same Q&A is not a
complete description of modern loop support.
[Verifier design limits](https://docs.kernel.org/6.15/bpf/bpf_design_QA.html).

Before implementing all-BPF training, verify task-storage helper availability,
state allocation in appropriate callbacks, migration serialization, map memory
bounds, and synchronization for any shared counters. A task does not run on two
CPUs simultaneously, but this does not excuse auditing every callback/state
access. Do not hold a BPF lock around scheduler kfunc calls. Sample telemetry
rather than writing one ring-buffer record per scheduling event.

If all-BPF updates fail verification or exceed the overhead budget, retain BPF
accounting and inference and move training to a C worker fed by a bounded event
queue. Publish event loss and staleness, use versioned model publication, and
benchmark that hybrid separately. This fallback introduces IPC/copy and
synchronization costs; it is not assumed faster.

## Runtime ownership and recovery

Run a foreground loader with an unpinned attachment and a finite experiment
deadline. Do not install a startup service or persist a scheduler attachment.
The initial compatibility check must refuse to replace an existing active
`sched_ext` scheduler. A normal stop releases the attachment and restores the
kernel's default fair scheduling class. The documented kernel also aborts the
BPF scheduler on detected errors/stalls and the uppercase SysRq `S` request.
These mechanisms do not guarantee immunity from kernel bugs or protect data
from every possible workload failure.
[Kernel fallback and enablement](https://docs.kernel.org/scheduler/sched-ext.html).

Use a QEMU guest with a preserved base image and disposable overlay. Keep an
independent guest console and host control path. Test attachment lifetime,
SIGINT/SIGTERM, process death, SysRq, and watchdog recovery before performance
experiments; restore the overlay between independent trials.
See [deployment proposal](../DEPLOYMENT.md).

## Backup: learned cgroup reclaim controller

If CPU feasibility fails, investigate a user-space C contextual bandit that
chooses among `noop` and small positive `memory.reclaim` requests for one
experiment-owned cgroup. Noop means **no write**. Stop issuing requests on
controller exit; do not install repeating kernel policies. Clamp request size
and rate and keep workload isolation, an independent deadline, and an abort
path. Actual reclaimed bytes and request duration are not strictly bounded by
the requested size: the kernel can over/under reclaim and returns `EAGAIN` for
under-reclaim. An in-progress request may still be executing when the controller
is stopped. Existing eviction, refault cost, and swap I/O cannot be instantly
undone.
[Linux v6.12 cgroup memory interface](https://www.kernel.org/doc/html/v6.12/admin-guide/cgroup-v2.html#memory-interface-files).

Features can include cgroup PSI deltas, recent refaults, file/anonymous mix, and
the workload's independently measured latency. Reward must use controlled
windows and separately logged allocations/frees; a decrease in `memory.current`
does not prove the chosen action reclaimed those bytes. PSI is stall time, not
application p99. Start with simulated/replayed observations and a fixed safe arm
set; never explore unbounded reclaim. An estimated 1,000–3,000 core C lines is
plausible, excluding workloads, packaging, and analysis, but unvalidated.

DAMON may later provide read-only diagnostics. DAMOS actuation is a separate
gate: existing DAMOS already supplies cold-region actions, quotas, watermarks,
and feedback quota tuning. Its running kernel context can outlive a userspace
controller, so an independently supervised `state=off` cleanup is necessary.
Setting both quota time and bytes to zero disables limits; it is **not** a noop.
Per-cgroup filtering in the v6.12 design requires the `paddr` backend, whereas a
small `vaddr` experiment can target a specific PID. Keep this added integration
out of the first CPU MVP.
[DAMOS design](https://www.kernel.org/doc/html/v6.12/mm/damon/design.html),
[sysfs lifecycle](https://www.kernel.org/doc/html/v6.12/admin-guide/mm/damon/usage.html).

## Acceptance gates and effort estimate

| Gate | Evidence required |
| --- | --- |
| Label feasibility | Known bursts agree with accounting; migration/preemption do not create false completions; censoring is reported |
| Fixed-point correctness | Boundary tests, arithmetic bounds, reference comparison, no feature/label leakage |
| Linux integration | Compatible build, verifier acceptance, correct dispatch, finite-run unload and independent recovery |
| Learning | Predict-before-update errors and coefficient changes on heldout workload phases beat or explain failure versus EWMA |
| Policy value | Comparable fixed/EWMA/ML runs with latency, throughput, fairness and overhead results |
| Release readiness | Reproducible artifacts, documented tested versions, failure drills, security review |

Engineering estimate: 1,450–2,400 core C/eBPF lines within a 1,000–3,000-line
scope ceiling, and 180–330 engineering hours (approximately 5–9 full-time
weeks) for feasibility, a narrow MVP, tests, and first credible results.
Kernel compatibility or accounting defects can dominate this estimate. No
implementation approval or performance result is implied by this document.
