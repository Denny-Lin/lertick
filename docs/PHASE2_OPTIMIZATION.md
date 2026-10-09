# Phase 2C optimization gate

No Phase 2C policy or model optimization is implemented in this branch.

The approved order is correctness, credible baseline measurements, then
controlled optimization if the evidence justifies it. The accounting fix,
bounded telemetry retention, EWMA comparator and optional diagnostic clocks
serve correctness and measurement. They do not constitute benchmark tuning.

The initial five-feature Q16 model, learning rate, coefficient projection,
prediction cap, eight-label warm-up, 0.5–5 ms bounds and fixed overrun fallback
remain unchanged. Phase 1 results are preserved.

The [results report](PHASE2_RESULTS.md) will record whether the measurements
justify a subsequent optimization experiment. A useful next experiment must
change one major factor, preserve fairness/recovery regressions, record failed
outcomes, and compare against fixed and EWMA. Better prediction error alone is
insufficient: it must translate into a useful scheduling tradeoff.

Unsuccessful validation/setup attempts are recorded with the final evidence:
late comm opt-in exposed missing tracked telemetry; a threaded fixture initially
mistook a genuine barrier/setup sleep for a false label; a newer upstream
comparator encountered an actual verifier compatibility failure. These are
correctness/tooling findings, not discarded performance results.

Neural networks, reinforcement learning, GPUs, Python ML runtimes and external
AI services are outside this phase. No release, tag or GitHub push is authorized
by this local validation work.
