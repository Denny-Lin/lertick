# Phase 2C optimization gate

No Phase 2C policy or model optimization was implemented. Final Phase 2A and
Phase 2B evidence does not justify tuning the linear model for benchmark scores.
The approved work remains correctness and credible measurement on the dedicated
local branch.

The accounting fix, bounded telemetry retention, EWMA comparator and optional
diagnostic clocks serve correctness and measurement. The initial five-feature
Q16 model, learning rate, coefficient projection, 10 ms prediction cap,
eight-label warm-up, 0.5–5 ms slice bounds and fixed overrun fallback are unchanged.
Phase 1 evidence is preserved byte-for-byte.

## Decision from actual measurements

The [final results](PHASE2_RESULTS.md) include 280 primary trials and 80 overhead
and observer-control trials. All online/fixed throughput confidence intervals
include zero change. Mixed and multicore sampled wake p99 increased about 214%
and 147% versus fixed. Linear same-episode MAE exceeded EWMA in every category
with eligible labels. EWMA scheduling also exhibits the mixed/multicore wake
tails, so lower prediction error alone is not a sufficient response.

Training and inference execute inside eBPF, and predictions change actual
bounded slices. This proves functionality, not useful learned scheduling.
Per-call arithmetic timer means are small but quantized by the guest's 24 MHz
counter and include observer cost. Whole-callback means include diagnostics,
accounting and different callback mixes; they do not isolate pure arithmetic.
Separate observer sessions show enough drift and observer effects to preclude
an exact uninstrumented ML overhead claim.

## Recommended separately approved next work

Continue as a targeted research redesign, not a production or performance
release. First reproduce the mixed/multicore responsiveness regression with
interleaved fixed, EWMA and online controls. Record per-task predictions,
remaining service, selected budgets, queue state and wake/run behavior. Then
evaluate one bounded slice-mapping or responsiveness-guard change at a time.
Keep independent service, fairness, lifecycle and recovery regressions, and
predeclare acceptable latency/throughput tradeoffs.

Before increasing model complexity, inspect eligible-task histories, cold starts,
bias and feature usefulness against EWMA. Use longer independent windows,
10–20 or more repetition blocks, randomized observer configurations and eventual
bare-metal confirmation. Preserve failed experiments and outliers. Better
prediction error must translate into a useful scheduling outcome beyond the
fixed and EWMA controls.

If a simple bounded policy continues to match or beat the linear model under
credible independent trials, retain it as the preferred baseline and reconsider
whether learning belongs in the scheduling path. No neural network, reinforcement
learning, GPU, Python ML runtime or external AI dependency is justified here.

## Preserved unsuccessful attempts

The [evidence archive](../results/phase2/README.md) preserves the aborted 149-trial
run at `8257281`, failed initial lifecycle/map/tooling checks, and a newer upstream
comparator's real verifier rejection. They are documented correctness and
compatibility findings, not discarded performance scores. Final tables use only
the fresh `109ce71` implementation run.

No further optimization phase begins automatically. Phase 2 commits remain local
for review, with no GitHub push, release or tag.
