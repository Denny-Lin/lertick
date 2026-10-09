# ScxSprout decisions

## 2026-10-09: approve and validate Phase 1

The repository owner approved the proposed ScxSprout direction and requested a
minimal C/eBPF sched_ext CPU scheduler, actual Ubuntu QEMU deployment, online
training and inference inside eBPF where feasible, bounded prediction-driven
slices, safe recovery, and small measured baseline comparisons.

The implementation uses the official C examples at
`sched-ext/scx-c-examples@370b1cea95c6bb668d5b9261f069d433142b9986` as its
reference. The initial policy is a shared weighted virtual-time dispatch queue.
A five-feature fixed-point linear model learns per-task wake-to-sleep CPU
service. It changes slice budgets after sufficient labels; dispatch order and
fairness accounting remain independent of predictions.

Phase 1 prioritizes proof of execution and recovery. It does not require a
neural network, userspace trainer, GPU, persistent daemon, Docker-only runtime,
or performance improvement. The accepted experiment uses QEMU HVF on ARM64;
Docker was not used.

The user explicitly authorized a local commit on the existing project branch
and withheld permission to push until reviewing the results. Phase 1 changes
therefore remain local pending approval. See [actual results](PHASE1_RESULTS.md),
[deployment commands](DEPLOYMENT.md), and [license provenance](../THIRD_PARTY.md).

The documents under `docs/planning/` retain their original Phase 0 reasoning
with a historical banner. Their earlier approval gates, status statements,
estimates, and proposed license boundaries are not current implementation
claims.
