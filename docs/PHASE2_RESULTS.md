# Phase 2 results

Phase 2A and Phase 2B completed on actual Linux. The service-accounting fix,
in-kernel learning, bounded adaptive dispatch, finite fairness tests and recovery
checks passed. All **280 randomized primary trials and 80 overhead/control trials**
passed. The linear model did **not** demonstrate useful throughput improvement over
fixed slices and produced substantial wake-latency regressions in mixed and
multicore contention. Its same-episode MAE exceeded EWMA in all six categories
with eligible labels. **Phase 2C optimization is deferred.**

These are experimental findings from one VM, not production-safety or general
Linux-scheduler superiority claims. The branch is local and reviewable; no push,
release or tag was made. Original Phase 1 evidence is unchanged.

## Environment, compilation and actual execution

The final scheduler/workload implementation is commit
`109ce711bfad52c9f2ddfe2ba760b29258d58c25`, built from a fresh Git-bundle clone.
The [environment record](../results/phase2/environment.json),
[build/verifier record](../results/phase2/build-verifier.json),
[benchmark manifest](../results/phase2/benchmark-environment.json) and
[raw archives](../results/phase2/README.md) preserve provenance.

- Ubuntu 24.04.5 ARM64, Linux `7.0.0-38-generic`, HZ=1000.
- QEMU 10.2.1 on macOS Apple Silicon, HVF acceleration, four vCPUs, 4 GiB RAM,
  20 GiB overlay, temporary 2 GiB swap for pressure trials.
- `CONFIG_SCHED_CLASS_EXT`, BPF syscall/JIT, BTF, PSI, schedstats and SysRq enabled.
  Correctness ran with selected full preemption; benchmarks used original lazy
  preemption. Original mode was restored afterward.
- Clang 18.1.3, BPF ISA v3, GCC 13.3.0, static libbpf 1.7.0, bpftool 7.7.0,
  bpftrace 0.20.2, perf 7.0.14. Compatible bpfland used Rust 1.91.1.
- `PKG_CONFIG_PATH=/home/ubuntu/deps/install/lib/pkgconfig make all test-model
  test-accounting` compiled the actual BPF object, running-kernel BTF header,
  skeleton, C loader, three workload binaries and two independent arithmetic
  tests. The model's 200,121 and accounting's 202,000 oracle cases passed.
- Actual verbose verifier acceptance recorded 13 program summaries, maximum
  3,031 processed instructions. Registration as `lertick`, real task execution,
  eBPF training and adaptive budgets were checked separately.
- Generated BTF declarations and vendored signed comparisons emitted nonfatal
  warnings. Full build and verifier logs are retained in `diagnostics.tar.gz`.

Official `scx_simple` is pinned at
`370b1cea95c6bb668d5b9261f069d433142b9986`. The strong compatible comparator is
`scx_bpfland` v1.0.16, `e25cc6e5920f33d5bbe2bd62b2e7a5854de88a19`.
Its binary resides in a shared directory named `scx-v1.1.2/target`; that path does
not indicate the compiled version. Newer `scx_lavd` v1.1.2 built but failed actual
verifier acceptance in `arena_init` with `R6 invalid mem access 'scalar'`. It was
excluded from performance comparisons, with failure logs preserved.

## Correctness and recovery results

The [raw correctness summary](../results/phase2/correctness.json),
[independent metric audit](../results/phase2/correctness-metrics.json) and
[correctness report](PHASE2_CORRECTNESS.md) record all 19 lifecycle cases,
156 independent service checks, 4,200-task churn, 8,000 children from concurrent
fork parents, three fairness trials and 15 recovery cases. All passed.

| Mode | Actual runs | Completed labels | eBPF updates | Adaptive runs |
| --- | ---: | ---: | ---: | ---: |
| fixed | 1,361 | 1,346 | 0 | 0 |
| shadow | 1,363 | 1,350 | 1,350 | 0 |
| online | 1,363 | 1,348 | 1,348 | 1,351 |

A captured continuation included 1,033,125 ns pending service against a
1,021,728 ns prediction and selected the existing 2,000,000 ns overrun fallback.
The legacy decision would have reused 1,021,728 ns. Nineteen sampled witness
snapshots contain 14 distinct tuples, 13 of which change the old decision.
Dispatch reads pending service; stopping alone commits actual service/fairness.
Online selected budgets span the configured 0.5–5 ms bounds.

All 156 service comparisons met max(20 ms, 2%) tolerance; largest absolute BPF
error was 1.894 ms. Forty-eight long controls had maximum 1.718 ms/0.0859% error.
Tiny/exec cases can have larger relative startup-window differences, up to
12.326%, so a universal sub-percent claim is unwarranted.

Eight equal competitors on four CPUs for nine seconds produced Jain indices
0.99999995 fixed, 0.99999995 shadow and 0.99999952 online, with observed maximum
progress gaps 7.047, 30.659 and 74.702 ms. These finite checks establish observed
progress, not universal starvation freedom.

Eight overlapping fork parents completed/reaped 8,000 distinct children under
full preemption. New init associations reached the known 8,008 lower bound;
final init=exit=8,160 and live=0. The LRU telemetry map retained 3,753 entries
within its 4,096 bound; owned maps disappeared within 172.477 ms across all cases.
Map footprint and logical cleanup boundaries are documented separately.

All three repetitions of each shutdown mechanism passed: signals restored default
scheduling in 31.408–64.645 ms, SysRq-S in 50.630–52.668 ms, and deliberate VM-only
watchdog stalls in 2.490–2.511 s. The one-second configured timeout is not an
exact recovery deadline. Final state was disabled, BPF runtime statistics
disabled, original lazy preemption restored, temporary swap removed, and the VM
powered down gracefully. See [cleanup evidence](../results/phase2/cleanup.json).

## Benchmark method and evidence exclusions

Five randomized repetition blocks use seed 20261009, two seconds of warm-up
and ten measured seconds per custom trial. Kernel-target builds run to completion
until at least ten measured seconds. All policies use their recorded defaults;
Lertick model coefficients, learning rate, eight-label warm-up and 0.5–5 ms bounds
were not tuned. The official simple scheduler's longer default quantum affects
its latency comparison.

Numbers below derive from [host analysis](../results/phase2/analysis.json),
[full metric tables](../results/phase2/analysis.md) and
[360 trial rows](../results/phase2/trials.csv). Full tables include p50/p95/p99,
utilization, context switches, migrations, fairness, callback breakdowns, ranges,
IQRs and seeded 10,000-resample uncertainty. See
[protocol](PHASE2_BENCHMARKS.md) and [reproduction commands](../benchmarks/README.md).

An earlier `8257281` run was intentionally stopped after 149 successful trials
when static review found an unlocked cold-counter update race. Atomic cold
counter updates and explicit MONOTONIC wake timestamps were committed in
`109ce71`. The earlier raw data is preserved and **excluded** from final analysis;
no results from different implementations are combined. Failed setup and
validation attempts are also preserved, described in the evidence archive.

## Median throughput — all 8 workloads × 7 policies

Custom rows: **million compute iterations/s**, excluding memory-access work. `kernelcompile`: **kernel/sched/built-in.a target rebuilds/s**. Do not aggregate the kernel row with custom throughput. Each cell is the median of five independent trials.

| Workload | default | scx_simple | fixed | shadow | ewma | online | scx_bpfland |
| --- | --- | --- | --- | --- | --- | --- | --- |
| cpu | 716.146 | 731.729 | 739.520 | 725.228 | 729.503 | 737.591 | 720.834 |
| shortburst | 728.498 | 729.019 | 716.516 | 707.680 | 733.304 | 715.489 | 727.219 |
| wakeheavy | 681.195 | 711.770 | 714.429 | 706.692 | 713.335 | 693.612 | 699.999 |
| mixed | 705.343 | 728.704 | 698.367 | 715.014 | 709.852 | 699.198 | 712.622 |
| multicore | 2612.477 | 2636.003 | 2521.187 | 2607.297 | 2598.716 | 2646.286 | 2639.562 |
| affinity | 1982.331 | 2119.013 | 2624.505 | 2569.413 | 2606.119 | 2601.447 | 886.932 |
| kernelcompile | 0.552 | 0.537 | 0.534 | 0.519 | 0.558 | 0.555 | 0.560 |
| memory | 1992.282 | 2066.634 | 2332.865 | 2025.581 | 2071.160 | 2536.586 | 2413.467 |

## Median completion p99 — all policies

All cells are **milliseconds**, medians of per-trial p99 values. Custom rows measure timer deadline to completed burst; kernel compilation measures actual completed target-build duration. The CPU and affinity fixtures contain no sleeping burst cohort, so completion p99 is unavailable. Completion includes CPU service; it is not wake latency.

| Workload | default | scx_simple | fixed | shadow | ewma | online | scx_bpfland |
| --- | --- | --- | --- | --- | --- | --- | --- |
| cpu | — | — | — | — | — | — | — |
| shortburst | 3.693 | 19.273 | 1.843 | 1.830 | 1.413 | 1.834 | 3.289 |
| wakeheavy | 3.754 | 20.617 | 2.728 | 2.741 | 2.751 | 2.745 | 3.587 |
| mixed | 25.411 | 47.759 | 23.429 | 23.690 | 24.006 | 24.019 | 9.476 |
| multicore | 9.591 | 15.320 | 8.271 | 8.029 | 7.289 | 7.310 | 6.285 |
| affinity | — | — | — | — | — | — | — |
| kernelcompile | 1870.526 | 2055.490 | 2019.720 | 2105.611 | 1892.499 | 1891.560 | 1865.263 |
| memory | 2.309 | 18.547 | 1.525 | 1.683 | 1.609 | 1.468 | 6.407 |

## Online versus fixed slices — paired outcomes

Changes are medians of five **paired per-repetition percentage changes**, not ratios of the displayed group medians. Positive throughput is better; positive latency is worse. Every online/fixed throughput interval includes zero. The mixed and multicore sampled wake p99 regressions have intervals entirely above zero.

| Workload | Throughput change % [95% CI] | Completion p99 change % [95% CI] | Fixed kernel wake p99 ms | Online kernel wake p99 ms | EWMA kernel wake p99 ms | Online/fixed wake p99 change % [95% CI] |
| --- | --- | --- | --- | --- | --- | --- |
| cpu | -0.39 [-1.40, +0.41] | — | — | — | — | — |
| shortburst | +0.14 [-6.92, +7.48] | -0.52 [-20.32, +2.35] | 1.552 | 1.010 | 1.085 | -17.24 [-49.62, +34.34] |
| wakeheavy | -0.53 [-3.01, +2.42] | +1.09 [-1.41, +1.59] | 2.530 | 2.532 | 2.505 | -0.07 [-4.69, +3.43] |
| mixed | +0.30 [-2.60, +1.44] | +2.42 [-10.04, +14.33] | 6.703 | 20.962 | 20.946 | +213.64 [+186.40, +248.42] |
| multicore | +4.58 [-27.86, +6.66] | -9.11 [-13.26, +30.01] | 1.604 | 3.946 | 3.952 | +147.34 [+113.53, +190.33] |
| affinity | -0.47 [-1.23, +11.25] | — | — | — | — | — |
| kernelcompile | +5.09 [-4.91, +7.38] | -9.05 [-13.77, +6.27] | 1.064 | 0.971 | 0.789 | -30.35 [-46.30, +58.13] |
| memory | +2.19 [-20.51, +20.34] | -2.02 [-14.86, +52.40] | 2.588 | 2.583 | 1.871 | -0.07 [-24.80, +0.37] |

The kernel observer samples every sixteenth custom-task wake and every matching compiler wake. It covers matching CPU, burst, and memory workers, so its cohort differs from burst-completion samples. Online median wake sample counts are 835 shortburst, 1,249 wakeheavy, 442 mixed, 577 multicore, 2,731 kernelcompile, and 974 memory per trial; p99 can therefore reflect only a handful of sampled tail events. No kernel wake samples exist in the continuously runnable CPU/affinity fixtures.

## Selected gains and regressions — measured tradeoffs

| Workload | Online compared with | Metric | Change % [95% CI] | Interpretation |
| --- | --- | --- | --- | --- |
| shortburst | default | completion_p99_ns | -50.34 [-50.79, -43.79] | Observed burst tail reduction versus EEVDF |
| wakeheavy | default | completion_p99_ns | -27.88 [-29.37, -15.15] | Observed burst tail reduction versus EEVDF |
| mixed | default | wake_to_run_ns.p99 | +157.54 [+130.13, +163.02] | Sampled wake tail regression versus EEVDF |
| mixed | scx_bpfland | completion_p99_ns | +155.40 [+152.29, +187.41] | Burst tail regression versus the strong comparator |
| multicore | scx_bpfland | completion_p99_ns | +20.03 [+15.66, +68.72] | Burst tail regression versus the strong comparator |
| shortburst | ewma | completion_p99_ns | +29.01 [+0.15, +50.42] | Linear online worse than EWMA scheduling |
| affinity | default | throughput | +31.62 [+14.95, +36.91] | Policy behavior without active model adaptation |

These are fixture-specific policy tradeoffs, not a claim of general Linux or EEVDF superiority. Fixed Lertick also reduces shortburst/wakeheavy completion tails; the improvements do not isolate a benefit from learning. The mixed bpfland background CPU progress gap is larger than Lertick's despite lower burst tails, which illustrates a responsiveness/service tradeoff. CPU and affinity have zero adaptive-slice selections; an affinity throughput gain cannot be credited to ML.

## Prequential prediction accuracy — online trials

Both predictors are evaluated on the **same subsequent verified CPU episodes**, before that episode trains the model; targets are capped at 10 ms, after eight eligible labels per task. Global MAE uses global sums and sample counts. The percentage column is the median **same-trial** linear/EWMA MAE ratio minus one. Positive means the linear model has higher error. Kernelcompile uses `--track-all`: its accuracy includes all guest tasks, not just compiler children. Raw episode means are retained telemetry, not custom burn-job duration.

| Workload | Global linear MAE µs [95% CI] | Same-trial EWMA MAE µs [95% CI] | Linear/EWMA error increase % [95% CI] | Retained raw episode mean µs |
| --- | --- | --- | --- | --- |
| shortburst | 14.44 [10.82, 17.71] | 5.11 [3.87, 5.96] | +229.50 [+101.27, +257.78] | 213.413 |
| wakeheavy | 22.33 [17.50, 25.79] | 4.86 [2.82, 5.07] | +424.10 [+244.97, +691.74] | 64.022 |
| mixed | 15.22 [12.02, 18.20] | 4.78 [3.83, 8.21] | +182.32 [+85.28, +364.65] | 651.291 |
| multicore | 21.59 [19.63, 94.53] | 5.10 [4.93, 84.55] | +297.89 [+11.80, +355.17] | 1049.875 |
| kernelcompile | 320.47 [300.31, 329.06] | 47.74 [44.59, 57.48] | +541.07 [+469.76, +618.63] | 57.574 |
| memory | 28.62 [9.80, 67.69] | 26.61 [8.25, 59.76] | +13.28 [+7.56, +18.85] | 913.820 |

The result favors EWMA prediction error in all six learning categories; it does not prove that EWMA scheduling always wins. The model's actual use is demonstrated by positive training/adaptive-slice counters below. Its utility beyond fixed or EWMA is not demonstrated. The compiler linear MAE is large relative to its mean raw episode; investigate eligible-task history and prediction bias before increasing model complexity.

| Workload | Global prediction samples median | Training updates median | Adaptive slices median | Clipped targets median [range] |
| --- | --- | --- | --- | --- |
| cpu | 0 | 0 | 0 | 0 [0, 0] |
| shortburst | 13724 | 13724 | 13727 | 0 [0, 0] |
| wakeheavy | 20552 | 20553 | 20152 | 0 [0, 0] |
| mixed | 7307 | 7307 | 7307 | 0 [0, 0] |
| multicore | 9488 | 9488 | 9489 | 0 [0, 5] |
| affinity | 0 | 0 | 0 | 0 [0, 0] |
| kernelcompile | 7270 | 9322 | 6467 | 6 [6, 7] |
| memory | 16149 | 16151 | 16153 | 4 [0, 16] |

## Retained RMSE bounds and completeness

RMSE is computed from final retained task entries; squared errors are quantized to microseconds before accumulation. Cells below are **median [observed min, max] µs**. For these five-trial groups, bootstrap median intervals equal the reported extrema, so the same bounds are also the 95% bootstrap intervals. Do not call an incomplete retained statistic a complete execution-cohort RMSE.

| Workload | Linear RMSE µs [range/95% CI] | Same-episode EWMA RMSE µs [range/95% CI] | Complete retained trials |
| --- | --- | --- | --- |
| shortburst | 20.84 [15.29, 34.14] | 10.19 [6.66, 30.81] | 5/5 |
| wakeheavy | 33.41 [29.52, 39.72] | 8.79 [4.96, 32.75] | 5/5 |
| mixed | 21.58 [18.65, 58.52] | 9.30 [5.17, 56.36] | 5/5 |
| multicore | 31.02 [26.39, 375.89] | 20.55 [11.85, 389.71] | 5/5 |
| kernelcompile | 656.67 [639.87, 672.23] | 363.93 [350.66, 398.82] | 5/5 |
| memory | 168.74 [18.59, 464.21] | 171.99 [18.47, 477.46] | 3/5 |

The primary run has **11 retained/global mismatches** among 160 Lertick-policy trials, with deficits of 1–3 samples. Several occur in fixtures with only seven opted-in tasks, so eviction is not an adequate explanation for every mismatch. Global counters and per-task entries are read non-atomically; the one-second live baseline can cross updates. This is a boundary/completeness limitation, not evidence of a specific dropped episode. Global MAE avoids missing retained entries but still has approximate snapshot boundaries. Online memory has only 3/5 complete retained trials; the other five online learning groups have 5/5.

| Workload | Policy | Repetition | Global samples | Retained samples | Deficit |
| --- | --- | --- | --- | --- | --- |
| kernelcompile | ewma | 0 | 7695 | 7694 | 1 |
| memory | online | 0 | 13645 | 13644 | 1 |
| memory | shadow | 0 | 7512 | 7511 | 1 |
| mixed | shadow | 0 | 7235 | 7234 | 1 |
| memory | ewma | 1 | 15481 | 15480 | 1 |
| memory | shadow | 1 | 7517 | 7515 | 2 |
| multicore | ewma | 1 | 9471 | 9470 | 1 |
| memory | shadow | 2 | 15704 | 15703 | 1 |
| kernelcompile | ewma | 4 | 7840 | 7839 | 1 |
| memory | ewma | 4 | 7320 | 7319 | 1 |
| memory | online | 4 | 16920 | 16917 | 3 |

## Accuracy/latency covariance — descriptive only

Pearson correlations across **five online trials** do not establish prediction-to-latency causality. The multicore outlier raises errors for both predictors and tail latency together; shared VM load or policy behavior can cause that covariance. Keep the outlier; no post hoc trial removal is justified.

| Workload | Linear MAE / completion p99 r | EWMA MAE / completion p99 r | Linear MAE / wake p99 r | EWMA MAE / wake p99 r |
| --- | --- | --- | --- | --- |
| shortburst | -0.870 | +0.270 | -0.843 | +0.471 |
| wakeheavy | +0.621 | -0.055 | +0.358 | +0.161 |
| mixed | -0.555 | +0.314 | +0.178 | -0.522 |
| multicore | +0.971 | +0.960 | +0.990 | +0.982 |
| kernelcompile | +0.728 | +0.652 | +0.146 | +0.563 |
| memory | +0.774 | +0.756 | -0.569 | -0.585 |

## Whole BPF callback means — primary trials

Nanoseconds/call, **median [95% CI]** of per-trial total BPF runtime divided by callback count. These windows include calibration, warmup, and final raw output. They exclude kernel C scheduler work and loader CPU time. Callback mix differs by policy; a change is not automatically SGD cost.

| Workload | Fixed ns/call [95% CI] | Shadow ns/call [95% CI] | Online ns/call [95% CI] | Shadow/fixed mean change % [95% CI] |
| --- | --- | --- | --- | --- |
| cpu | 164.01 [159.42, 175.12] | 196.21 [148.79, 715.56] | 223.00 [160.20, 299.17] | +16.27 [-9.28, +308.60] |
| shortburst | 116.78 [85.56, 169.52] | 122.54 [90.72, 143.24] | 112.75 [90.86, 158.94] | -7.71 [-29.37, +47.44] |
| wakeheavy | 100.08 [86.39, 136.30] | 92.72 [91.26, 167.93] | 123.60 [94.20, 146.62] | +5.64 [-15.84, +23.20] |
| mixed | 124.40 [115.14, 175.24] | 127.99 [109.04, 189.96] | 121.16 [105.87, 205.92] | +8.40 [-21.59, +46.60] |
| multicore | 473.08 [424.78, 493.96] | 403.43 [370.78, 635.47] | 351.00 [341.62, 1507.18] | -12.70 [-23.28, +30.04] |
| affinity | 264.89 [241.32, 612.44] | 313.87 [286.46, 397.54] | 291.97 [245.24, 333.77] | +18.71 [-48.75, +48.60] |
| kernelcompile | 360.45 [342.81, 457.85] | 437.42 [315.18, 460.88] | 341.05 [333.14, 435.06] | +2.29 [-8.06, +27.86] |
| memory | 394.34 [327.68, 633.34] | 455.01 [359.13, 871.37] | 330.69 [299.83, 879.64] | +23.71 [+9.60, +37.58] |

All shadow/fixed primary throughput intervals include zero. The memory whole-callback mean increase is observed, but variable paging interference and callback mix prevent attribution entirely to the arithmetic training step. The run does not establish a precise uninstrumented ML overhead.

## Profile study — all 40 trials

Two workloads × four policies × five trials. All values are **median [observed min, max] ns**; bootstrap median 95% intervals equal these extrema for these groups. Fixed has no training/inference arithmetic, EWMA has inference only. Training/inference timers cover the arithmetic region; they exclude feature preparation, prediction conversion, map/stat publication, and other callback work. Timer calls and BPF runtime accounting add measurement cost. These means are not total ML overhead.

The guest's `arch_sys_counter` runs at 24.00 MHz, approximately **41.7 ns per counter tick**. Reported 14–26 ns arithmetic means average many quantized timing calls and can fall below a single tick. They do not establish nanosecond precision for an individual operation or an uninstrumented cycle count.

| Workload | Policy | Training mean ns [range/95% CI] | Inference mean ns [range/95% CI] | Whole callback mean ns [range/95% CI] |
| --- | --- | --- | --- | --- |
| wakeheavy | fixed | — | — | 101.11 [91.27, 120.31] |
| wakeheavy | shadow | 25.09 [22.84, 27.82] | 14.58 [14.30, 17.03] | 106.16 [96.79, 139.94] |
| wakeheavy | ewma | — | 11.38 [10.34, 11.65] | 100.64 [91.38, 131.55] |
| wakeheavy | online | 24.36 [23.57, 26.71] | 15.01 [14.39, 17.95] | 100.13 [95.90, 135.40] |
| mixed | fixed | — | — | 109.18 [94.25, 192.51] |
| mixed | shadow | 25.77 [24.71, 29.51] | 14.95 [13.95, 15.92] | 132.58 [109.79, 209.76] |
| mixed | ewma | — | 11.10 [10.66, 11.37] | 106.47 [104.27, 132.41] |
| mixed | online | 26.27 [25.27, 27.71] | 14.45 [13.59, 16.08] | 119.46 [111.11, 167.31] |

## Observer controls — throughput changes

Each control cell has five trials. These are **unpaired, separate-session** percent changes of medians with independent bootstrap 95% CIs. Repeating the same index does not create a matched block across sessions. The contrasts isolate a configuration flag, but temporal drift still limits causal attribution.

| Workload | Policy | Remove wake trace vs primary % [95% CI] | Remove wake trace + BPF stats vs primary % [95% CI] | Enable BPF stats, wake trace off % [95% CI] | Enable arithmetic timers vs primary % [95% CI] |
| --- | --- | --- | --- | --- | --- |
| wakeheavy | fixed | +3.10 [+0.57, +7.75] | +2.09 [-0.37, +6.69] | +0.99 [-1.48, +3.38] | +1.26 [-1.78, +5.82] |
| wakeheavy | online | +1.55 [-4.34, +6.34] | +6.25 [+0.48, +7.97] | -4.41 [-8.50, +2.35] | +5.16 [-0.32, +6.86] |
| mixed | fixed | +5.75 [-0.27, +8.22] | +6.52 [+1.83, +9.02] | -0.73 [-5.38, -0.05] | +6.00 [-2.17, +8.48] |
| mixed | online | +5.40 [-0.56, +7.16] | +5.86 [-0.13, +7.63] | -0.43 [-2.35, +0.57] | +5.98 [-0.01, +7.76] |

Removing both observers changes two flags and cannot isolate BPF accounting. The observer-control/no-observers contrast changes only BPF accounting. Several profile sessions are faster despite adding timers, which makes negative-cost interpretations invalid and illustrates session drift. Some observer-removal intervals exclude zero, so instrumentation impact cannot be assumed negligible. Use an interleaved randomized observer study before assigning a numeric overhead budget.

## Additional validity limits

All 35 primary memory trials show positive memory PSI and swap-in deltas. Those deltas include prefaults/warmup; measured thread major faults occur in 33/35 trials and strengthen evidence of actual measured interference. They do not prove pressure during every measured second. Custom throughput excludes memory scanning; memory results are CPU scheduling under interference, not a memory-management feature.

Custom completion distributions omit burst jobs that cross the final measurement deadline; this is a censored-end tail limitation, especially for short 10 s windows. Progress gaps include the final interval through the deadline, all custom workers make progress, and equal continuously runnable CPU workers have high measured Jain fairness. Those observations do not prove a formal starvation or latency bound. Sampled migrations may miss intermediate movement. Global CPU utilization and BPF runtime cover broader windows than throughput. ARM64 QEMU/HVF evidence needs independent bare-metal confirmation before broad performance claims. See [the evidence archive](../results/phase2/README.md).

## Recommendation for a separately approved Phase 3

| Priority | Proposed next experiment | Evidence / acceptance criterion |
| --- | --- | --- |
| 1 | Reproduce mixed/multicore wake tails with interleaved fixed, EWMA, and online; record per-task predictions, selected slices, and wake/run causality | Online/fixed wake p99 rises +214% / +147%; both EWMA and online share high tails. Identify the responsible scheduling behavior before tuning. |
| 2 | Evaluate one bounded slice-mapping or responsiveness-guard change at a time | Keep fixed and EWMA controls, fairness/progress recovery tests, and predefined latency/throughput criteria. Do not assume a more accurate model alone fixes tails. |
| 3 | Audit task eligibility, cold starts, feature ablations, and bias against EWMA | Linear MAE is higher in all six categories. Require held-out repeatable utility, not only nonzero updates or lower training loss. |
| 4 | Improve metric boundaries/cohorts and randomize observer settings within the same sessions | Snapshot mismatches, censored end jobs, sampled wake cohorts, and session drift currently limit attribution. |
| 5 | Repeat with longer windows, 10–20+ independent blocks, and bare metal | Five VM trials produce coarse exploratory CIs; preserve outliers and publish all raw artifacts. |

Do not begin an additional optimization phase automatically. Phase 2 supports publishing a validated experimental scheduler and its tradeoffs; it does not justify a claim that the linear model outperforms EWMA, fixed slices, EEVDF, or established sched_ext policies. No Phase 2C code change or advanced ML feature is recommended from these measurements alone.
