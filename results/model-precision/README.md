# Optional burst-model priors and fractional updates

Recorded 2026-11-01, model/runner implementation `6beaba2`, ARM64 Linux
`7.0.0-38-generic`, eight vCPUs, workload affinity `0,1,2,3`. Defaults remain
`--model-init bias --model-update trunc`. This is an accuracy experiment,
not evidence of improved scheduling or general Linux superiority.

## What changed

`--model-init ewma` starts the existing linear model at the causal EWMA
feature instead of a fixed 2.5 ms bias. All features, the 10 ms target cap,
learning rate, coefficient bounds, eight-label warm-up and slice bounds
remain unchanged. `--model-update carry` retains 16 fractional update bits
that otherwise round to zero. No floating-point operation enters eBPF.

With the original bias update, residuals below 64 Q16 units (about 9.77 us
at the 10 ms scale) cannot move any coefficient. An independent test shows
a 30-unit numerical residual persisting after 1,000 original updates, while
the carry variant converges. This proves an arithmetic limitation, not a
general workload win. The model state is 48 bytes versus the original 32,
a 16-byte increase per task even when carry is not selected.

## Frozen factorial comparison

Four existing workloads, five randomized repetition blocks, four model
configurations: 80 complete trials. Each uses 2 s warm-up and 10 s measured
time. Shadow mode retains fixed 2 ms slices, so prediction changes do not
change budget decisions. The kernel wake observer is enabled throughout.
Kernel preemption was temporarily selected as **full**, then restored to
**lazy**. All trials passed.

Global prequential absolute-error deltas are divided by global eligible-
label deltas, avoiding retained-map eviction bias. The one-second live
snapshot baseline makes window boundaries approximate. Repetition is the
resampling unit; five trials give coarse exploratory bootstrap intervals.
Within-trial episodes are correlated and are not independent repetitions.

The following compares bias/trunc with ewma/carry. Raw MAEs are medians;
percentage changes are the median of paired trial changes, so they need
not equal a ratio of the two displayed medians.

| Workload | Bias/trunc MAE us | EWMA/carry MAE us | Paired change [bootstrap 95% interval] | Candidate model/EWMA ratio |
| --- | ---: | ---: | --- | ---: |
| mixed | 21.739 | 6.190 | -71.53% [-72.99, -68.75] | 1.086 |
| multicore | 25.217 | 11.706 | -51.81% [-54.57, -37.77] | 1.010 |
| shortburst | 16.350 | 5.523 | -67.12% [-72.15, -60.56] | 1.002 |
| wakeheavy | 21.850 | 3.560 | -82.17% [-86.08, -69.29] | 0.999 |

**The combined variant mostly catches up with EWMA; it does not establish
a learned advantage over that simpler same-episode baseline.** Lower model
error does not by itself establish better wake latency or throughput. These
are stable custom workloads, not held-out real applications.

The two-factor breakdown matters: carrying fractional updates with the old
bias prior is not a general win. Its multicore model/EWMA ratio increases
8.72% (paired interval +5.90% to +28.18%). Starting from EWMA before adding
carry consistently improves that ratio in this dataset. Full per-factor,
latency and throughput results are retained in the four JSON reports.

## Earlier pilot and environment limits

A separate 40-trial initializer-only pilot used the earlier `aca70d4`
binary with lazy preemption. It did not establish a consistent MAE benefit.
Do not pool it with the newer full-preemption factorial or attribute their
different absolute errors to one cause. A matched same-binary lazy/full
confirmation is still required before considering a default change. No
parameters were tuned after the factorial scores.

## Correctness and preserved failures

All four init/update combinations passed the complete Linux Phase 2 suite:
learning and pending-service witnesses, 19 lifecycle/churn cases, 8,000
children from concurrent fork parents, three fairness modes, map release,
and three full recovery repetitions (signals, SysRq-S and real watchdog).
Host model/accounting oracles and ASAN/UBSAN passed; carry has an additional
100,000 adversarial signed-128-bit oracle cases. Linux CPU-performance and
Lerwatt plug-in units also passed.

The earlier failures remain in the raw archives:

- The first full run stopped because the guest selected lazy preemption,
  while the concurrent-fork protocol explicitly requires full. It was not
  counted as a complete pass; temporary changes were restored.
- A runnable negative control emitted a completed long burst. Ten repeated
  fairness runs reproduced the pattern once in fixed mode: the exact TID
  blocked in an ext4 journal wait while writing its final result. The kernel
  trace and task snapshot agree. Direct workers now report through a separate
  unmeasured parent, retaining the zero-sleep-label assertion.
- A later fork/exec check compared a timed 20 ms interval with a 47.5 ms
  lifetime BPF total. The old run did not capture an independent lifetime
  total, so its excess cannot be attributed retrospectively. The comparison
  scope was inconsistent regardless. Fork/exec now records post-exit wait4
  user+system CPU and uses a tighter max(5 ms, 2%) lifetime comparison. In
  384 new fork/exec checks, the largest BPF/wait4 difference is 0.180 ms.
  The original outlier is retained, not recast as a proven accounting pass.

## Reproduction and files

```sh
make test-model test-accounting test-cpuperf test-lerwatt  # Linux
sudo python3 tests/validate_phase2.py --output /var/tmp/lertick-precision-new \
  --model-init ewma --model-update carry
sudo python3 benchmarks/run_phase2.py --output /var/tmp/lertick-factorial-new \
  --policies shadow,shadow-ewma-prior,shadow-carry,shadow-ewma-carry \
  --workloads shortburst,wakeheavy,mixed,multicore --repetitions 5 \
  --duration 10 --warmup 2 --cpus 0,1,2,3 --seed 2026110102
python3 benchmarks/analyze_model_init.py --input /var/tmp/lertick-factorial-new \
  --baseline shadow --candidate shadow-ewma-carry --output comparison.json
```

Select full preemption explicitly before reproducing the original validation
and factorial; record and restore the selected mode afterward. Do not change
it on a machine with an active scheduler. macOS can run model/accounting tests;
CPU-performance/plug-in tests and the kernel suite were run on Linux.

- [Protocol](PROTOCOL.md), [environment](environment.json), [plan](plan.json),
  [session/restoration](session.json), [validation summary](validation-summary.json).
- [Combined](combined.json), [initializer](ewma-init.json), [carry with EWMA prior](ewma-carry.json),
  [carry with bias prior](bias-carry.json), [earlier lazy pilot](initializer-lazy-pilot.json).
- Full raw archives are in `raw/`; [sizes and SHA256](archives.json) identify them.
  They include every trial, smoke evidence, failed attempts and kernel traces.
