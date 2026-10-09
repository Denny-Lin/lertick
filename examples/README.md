# Proposed terminal demo

This is a future acceptance scenario. `scx-sprout` and its flags below are proposed interfaces, not installed commands. There is no generated or fabricated demo output.

1. Capture the kernel/config and baseline scheduler state in an Ubuntu QEMU guest.
2. Start the foreground loader in observe mode; verify task telemetry and labeled completed bursts.
3. Display prediction error, model-update counts, skipped/censored labels, and model-cost counters.
4. Enable bounded learned slices for the selected test tasks; show actual slice decisions beside predictions.
5. Run the same mixed workload under fixed, EWMA, frozen-model, and online policies; retain all raw trials.
6. Stop the loader and verify `/sys/kernel/sched_ext/state` reports `disabled`.

The [deployment plan](../docs/DEPLOYMENT.md) defines prerequisites and recovery. The [benchmark protocol](../docs/planning/BENCHMARK_PLAN.md) defines acceptable evidence. A later demo must show policy changes and measured errors without implying an unmeasured speedup.
