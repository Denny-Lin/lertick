# Burst-model precision experiment

This protocol was written before measuring the fractional-update variant.
The initializer-only pilot is exploratory evidence, not a selection test.

## Frozen comparison

- One Ubuntu ARM64 guest, eight vCPUs, workload affinity `0,1,2,3`.
- Shadow mode: all variants keep fixed 2 ms slices. CPU service accounting,
  feature definitions, clipping, learning rate and task eligibility stay fixed.
- A factorial comparison separates initialization (`bias` or `ewma`) from
  update arithmetic (`trunc` or `carry`). The first primary comparison is
  `shadow` versus `shadow-carry`; the second is `shadow-ewma-prior` versus
  `shadow-ewma-carry`. The initializer comparison is secondary.
- Four existing workloads: shortburst, wakeheavy, mixed, multicore. Five
  complete randomized repetition blocks; 10 s measurement and 2 s warm-up.
  Seed `2026110102`. Retain all 80 trials, including outliers and failures.
- Record the actual kernel preemption mode, binary/source hashes and plan.
  Do not pool the earlier lazy-preemption pilot with a full-preemption run.
- Retain the kernel wake observer in every trial. Run no other VM workload
  experiments concurrently. Stop and diagnose scheduler or accounting errors.

## Scores and acceptance

Primary accuracy is the global prequential absolute-error delta divided by
the global eligible-label delta. This avoids bias from evicted task snapshots.
Also report model MAE divided by the EWMA MAE on the same episodes within each
trial. Repetition is the resampling unit; use paired percentage changes with
trial-level bootstrap median intervals. Five repetitions provide coarse,
exploratory intervals, not a general Linux performance claim.

Report throughput, kernel wake p99 and burst-completion p99 alongside accuracy.
The one-second scheduler snapshots give approximate measurement boundaries.
Do not use better training loss as evidence of better held-out prediction.
Do not change the default based on a narrow or inconsistent result. A later
online comparison must include fixed and EWMA controls before claiming that
more accurate predictions improve scheduling.

The carry arithmetic must first pass an independent signed-128-bit oracle,
sanitizers, Linux compilation and verifier acceptance. Validate lifecycle,
service accounting, runnable controls, fairness and recovery separately.
Its model state uses 48 bytes, versus 32 bytes for the original model; this
cost must be reported even if accuracy does not improve.

## Initializer-only pilot, before carry evaluation

The pilot used seed `2026110101`, the same four workload categories and
five repetitions, shadow versus shadow-ewma-prior, with lazy preemption.
All 40 trials completed. EWMA initialization alone did not consistently
improve MAE across categories, so it does not justify changing the default.
The raw archive and paired analysis will accompany the final report.

The first full validation stopped at its full-preemption requirement. A
subsequent full-preemption run passed lifecycle, concurrent fork and two
fairness modes, but an online CPU control produced a long completed label.
Its workload had no voluntary sleep inside the measured interval. Preserve
this failure and diagnose blocking around reporting before accepting the
negative control. Temporary kernel preemption changes were restored.
