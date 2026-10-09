# Lertick decisions

## 2026-10-09: Phase 2 approval and local validation

The owner approved Phase 2A correctness and safety followed by Phase 2B
reproducible CPU scheduling benchmarks. Phase 2C optimization is conditional on
correctness passing and reliable measured baselines. Work uses the dedicated
`phase2-correctness-benchmarks` branch from published main `fba7ee4`.

The existing ARM64 Ubuntu QEMU guest remains the deployment environment.
Historical Phase 1 evidence is immutable. Memory pressure is a CPU interference
workload, not a memory-management development direction. The project name and
open-source architecture remain Lertick.

Validated changes may be committed locally. **GitHub push requires renewed
approval of these results. No release or tag is created.** See
[Phase 2 results](PHASE2_RESULTS.md) for the completed scope and limitations.

## 2026-10-09: CPU-only scope, final name, and publication approval

The owner approved publishing Phase 1, requested a narrower CPU-only scope, and
then asked for independent research into a stronger name instead of SchedMind.
The [12-name comparison](NAMING.md) selected **Lertick**, pronounced **LER-tick**,
from learning + tick. The repository is `Denny-Lin/lertick`, and the executable
and sched_ext registration are `scx_lertick`.

Active scope is online machine learning for Linux CPU scheduling using sched_ext
and eBPF: CPU-burst prediction, in-kernel training/inference, adaptive slices,
CPU scheduling decisions, fairness, starvation prevention, low overhead, and
reproducible benchmarks. Memory allocation optimization, memory reclaim,
DAMON/DAMOS controllers, NUMA memory placement, and joint CPU/memory management
are background research only, neither active nor backup directions.

The rebranding changes identifiers, workload opt-in names, build/CLI names,
documentation, and metadata. It preserves the scheduling policy, model,
arithmetic, accounting, bounds, recovery behavior, and known limitations.
Original Phase 1 commit `1d2c39e` and all `results/phase1/` files are preserved.
The [rebranding report](REBRAND_VALIDATION.md) records regression checks.

Publication authorization includes renaming the GitHub repository, updating the
local remote, and pushing validated commits to `origin/main`. It supersedes the
earlier instruction to keep Phase 1 local. **No Phase 2 implementation, release,
or tag is authorized or created as part of this task.**

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

At this earlier decision, the user explicitly authorized a local commit on the existing project branch
and withheld permission to push until reviewing the results. Phase 1 changes
therefore remained local until the publication approval recorded above. See [actual results](PHASE1_RESULTS.md),
[deployment commands](DEPLOYMENT.md), and [license provenance](../THIRD_PARTY.md).

The documents under `docs/planning/` retain their original Phase 0 reasoning
with a historical banner. Their earlier approval gates, status statements,
estimates, and proposed license boundaries are not current implementation
claims.
