# Burst-model confirmation

240 valid long-window trials completed on one eight-vCPU ARM64 QEMU guest.
The model/scheduler implementation is frozen at `6beaba2`; the workload and
measurement implementation is `0b23633`. Source/binary hashes match across
every complete job. Parameters, model defaults, slice bounds and watchdog
thresholds were not retuned. See [protocol](PROTOCOL.md),
[design](design.json), [provenance](provenance.json) and [archive hashes](archives.json).

**Original bias/trunc defaults remain.** EWMA/carry improves many accuracy
estimates but is not a universal replacement. The alternating family favors
the original model. Online wake latency can worsen despite lower prediction
error, and a natural lazy-mode watchdog fallback remains unresolved.

## Completed matrix

Each workload/policy pair has five randomized repetition blocks, 30 seconds
measured after five seconds warmup. All primary sample buffers are explicitly
prefaulted. Only complete sessions enter the five-repetition comparisons.
The candidate selects EWMA initialization with fractional carry; the original
selects bias initialization with truncation. Shadow trials retain fixed 2 ms
slice budgets; online trials exercise their prediction-selected budgets.

| Job | Planned trials | Status |
| --- | ---: | --- |
| stable-full | 40 | complete |
| stable-lazy-repeat | 40 | complete |
| online-short | 40 | complete |
| online-contended | 40 | complete |
| heldout-full-a | 20 | complete |
| heldout-full-b | 20 | complete |
| heldout-lazy-a | 20 | complete |
| heldout-lazy-b | 20 | complete |
| observer-lazy | 40 | not run |

The initial lazy session is separate: 16 trials completed, the seventeenth
failed, and its remaining 23 planned trials were not run. That successful
prefix is retained but excluded from the paired confirmation tables. One
complete fresh 40-trial retry passed with the same binary, flags, watchdog
and input seeds. It does not resolve or erase the original failure.

## Stable workload prediction

MAE is prequential error against verified, clipped CPU-service labels. Ratios
compare model and EWMA on the same episodes inside each trial; below one is
better. Across-policy comparisons use separate live runs with their own labels.
Percent changes are medians of matched repetition changes, not ratios of the
absolute medians. Counter-window boundaries use one-second live snapshots and
are approximate. Five paired trials yield coarse exploratory bootstrap
intervals; episodes are correlated and are not independent repetitions.

### Full

| Workload | Original MAE us | Candidate MAE us | Paired MAE change, coarse interval | Original / EWMA | Candidate / EWMA |
| --- | ---: | ---: | --- | ---: | ---: |
| mixed | 18.439 | 7.707 | -58.20% [-77.27, +93.20] | 2.8801 | 1.0142 |
| multicore | 31.881 | 16.381 | -31.88% [-51.01, +5.61] | 1.6824 | 1.0121 |
| shortburst | 15.824 | 5.503 | -61.16% [-65.22, -3.23] | 1.9765 | 1.0031 |
| wakeheavy | 20.086 | 4.005 | -80.06% [-82.74, -40.73] | 2.9776 | 0.9983 |

### Lazy, complete retry

| Workload | Original MAE us | Candidate MAE us | Paired MAE change, coarse interval | Original / EWMA | Candidate / EWMA |
| --- | ---: | ---: | --- | ---: | ---: |
| mixed | 18.451 | 4.471 | -73.18% [-83.03, -67.68] | 4.0905 | 1.0051 |
| multicore | 21.713 | 8.988 | -59.01% [-64.03, -51.26] | 2.2610 | 1.0048 |
| shortburst | 11.888 | 4.638 | -64.43% [-72.03, -58.75] | 3.0321 | 1.0001 |
| wakeheavy | 15.524 | 3.399 | -78.52% [-82.77, -70.17] | 3.7141 | 0.9956 |

Mixed/multicore full-session intervals cross zero and individual repetitions
regress. Many candidate ratios remain near one, indicating that the model
mostly catches up to the same-episode EWMA on these stable families.

## Fresh input patterns

These four generator families and seeds were fixed before their first model
scores. Requested CPU targets and phase/iteration indices are logged afterward,
never supplied as scheduler features or training labels. The kernel still
learns actual completed CPU service. These are custom inputs, not applications.

### Full

| Workload | Original MAE us | Candidate MAE us | Paired MAE change, coarse interval | Original / EWMA | Candidate / EWMA |
| --- | ---: | ---: | --- | ---: | ---: |
| alternatingburst | 368.314 | 429.093 | +17.82% [+14.22, +20.85] | 0.3844 | 0.4480 |
| rampburst | 189.662 | 24.740 | -86.93% [-88.33, -85.94] | 8.7512 | 1.0613 |
| randomstepburst | 349.874 | 181.217 | -48.21% [-49.04, -47.46] | 1.8390 | 0.9511 |
| stepburst | 60.245 | 10.950 | -81.82% [-82.29, -79.92] | 6.7119 | 1.1939 |

### Lazy

| Workload | Original MAE us | Candidate MAE us | Paired MAE change, coarse interval | Original / EWMA | Candidate / EWMA |
| --- | ---: | ---: | --- | ---: | ---: |
| alternatingburst | 370.933 | 432.680 | +16.59% [+14.01, +19.32] | 0.3868 | 0.4518 |
| rampburst | 189.691 | 22.219 | -88.27% [-88.45, -87.80] | 9.1210 | 1.0625 |
| randomstepburst | 349.453 | 180.882 | -48.32% [-48.87, -47.48] | 1.8363 | 0.9508 |
| stepburst | 59.829 | 10.280 | -82.93% [-83.05, -82.61] | 7.0507 | 1.2088 |

In the full alternating case the candidate regresses 17.82% MAE in all five
pairs. The original model/EWMA ratio is 0.3844, versus 0.4480 for the candidate:
both learn useful structure on this family, while the original remains better.
The full step case improves 81.82% versus the original but still trails EWMA.
Neither a universal accuracy win nor an online scheduling win follows.

## Online accuracy and utility

The optional configuration is compared with original online mode and both
fixed/EWMA controls, on all four declared workloads and all three utility
metrics. Positive throughput change is better; positive latency change is
worse. Do not combine different workloads' throughput units.

| Workload | Original MAE us | Candidate MAE us | Paired MAE change, coarse interval | Original / EWMA | Candidate / EWMA |
| --- | ---: | ---: | --- | ---: | ---: |
| mixed | 11.931 | 4.750 | -62.37% [-65.45, -56.65] | 2.7054 | 1.0086 |
| multicore | 20.488 | 8.411 | -60.96% [-68.40, -38.19] | 2.3391 | 1.0051 |
| shortburst | 14.225 | 3.908 | -67.74% [-78.04, -58.52] | 3.9237 | 1.0006 |
| wakeheavy | 16.687 | 2.954 | -81.65% [-87.04, -78.21] | 5.2714 | 0.9931 |

| Workload | Control | Throughput change, interval | Wake p99 change, interval | Completion p99 change, interval |
| --- | --- | --- | --- | --- |
| shortburst | fixed | +0.04% [-1.26, +1.63] | +3.69% [+0.60, +56.22] | +0.05% [-10.31, +0.64] |
| shortburst | ewma | -0.04% [-1.35, +1.00] | +1.66% [-13.67, +34.70] | -0.42% [-0.69, +12.54] |
| shortburst | online | -0.95% [-1.59, +0.46] | +35.65% [+18.63, +39.36] | +0.68% [-9.94, +12.08] |
| wakeheavy | fixed | -0.01% [-0.47, +3.53] | -0.39% [-1.69, +0.55] | -0.02% [-5.52, +5.35] |
| wakeheavy | ewma | +0.05% [-0.22, +1.61] | -1.76% [-3.26, -0.67] | -3.53% [-5.91, +5.88] |
| wakeheavy | online | -0.32% [-0.81, +2.85] | -2.23% [-3.79, -0.32] | -3.79% [-5.96, +1.68] |
| mixed | fixed | +0.15% [-0.37, +1.88] | +186.56% [+147.76, +227.00] | -8.04% [-11.15, +0.02] |
| mixed | ewma | -0.02% [-0.65, +1.14] | +0.06% [-4.69, +14.32] | +0.03% [-2.33, +12.46] |
| mixed | online | +0.11% [-0.10, +0.71] | +13.00% [+0.02, +14.36] | +10.12% [-0.12, +12.52] |
| multicore | fixed | -0.09% [-1.63, +0.18] | +207.29% [+110.43, +226.35] | +6.68% [-12.43, +12.26] |
| multicore | ewma | -0.08% [-2.25, +0.40] | +0.15% [-1.03, +0.62] | +2.64% [-16.31, +23.33] |
| multicore | online | +0.07% [-2.18, +1.06] | +0.21% [-0.32, +0.71] | -0.51% [-24.93, +3.08] |

There is no consistent throughput advantage. Against original online mode,
shortburst wake p99 rises from across-trial medians 0.995 to 1.364 ms, with
paired median change +35.65% [18.63, 39.36]. Mixed wake p99 rises 20.936 to
22.807 ms, with paired median change +13.00% [0.02, 14.36]. Absolute medians
and paired percentage medians are different summaries. Lower model MAE does
not by itself justify changing scheduler defaults. Every control and metric
remains in the [analysis files](analysis/online-short-controls.json) and
[contended analysis](analysis/online-contended-controls.json).

## Unresolved fallback and omissions

The initial lazy session's original bias/trunc shadow mixed trial hit a kernel
runnable-task stall: `lertick_c07` had not run for 1.113 seconds. Linux scheduling
and original lazy preemption were restored. Its failed state and complete raw
archive remain intact. There is temporal proximity to the host display turning
on, but no matching system-sleep event and no proven cause. Five extra read-only
traced diagnostic runs did not reproduce it. They are marked smoke/diagnostic
and never enter these performance tables. A successful complete retry cannot
fix or erase that reliability result.

[Failure](failures/stable-lazy-initial/failure.json),
[kernel dump](failures/stable-lazy-initial/scheduler.log),
[confirmation state](failures/stable-lazy-initial/confirmation.json),
[diagnosis](failures/stable-lazy-initial/diagnosis.json),
[diagnostic run](failures/stable-lazy-initial/diagnostic-run.json),
and [diagnostic archive](failures/stable-lazy-initial/diagnostic-archive.json)
retain the record. Guest failure/diagnostic copies were cleared only after
complete archives and all 168 live file hashes/sets matched; durable host
archives remain outside Git. No user-preexisting data were removed.

Omitted/incomplete declared jobs: observer-lazy. The 05:35 UTC research reserve stops new sessions before the eight-hour deadline. The observer ablation is not established if it was not run.

## Scope and reproduction

Full and lazy are separately randomized sessions in grouped time order, not a
randomized per-trial preemption intervention. Do not claim a causal preemption
effect or pool earlier pilot/factorial data: binary, windows, sample-buffer
controls and generator logging differ. No real-board, energy or general Linux
superiority is claimed. Model defaults remain bias/trunc and all new choices
remain optional.

```sh
# Isolated Linux QEMU/KVM guest; fresh output, root, no other scheduler/controller:
sudo python3 benchmarks/run_model_confirmation.py --stage heldout-shadow --modes full --output /var/tmp/lertick-confirmation-fresh
# Portable score reproduction from the published compact evidence:
python3 benchmarks/analyze_model_init.py --input results/model-confirmation/compact/stable-full--full-prefault --mode shadow --baseline shadow --candidate shadow-ewma-carry --output /tmp/lertick-full-analysis.json
python3 benchmarks/analyze_online_utility.py --input results/model-confirmation/compact/online-short--lazy-prefault --output /tmp/lertick-utility.json
```

[Linux build and eight verifier-smoke trials](linux-build-and-smoke.log) passed.
Full raw archives preserve every job/latency trace locally; public compact
records preserve all trials, verifier/exit records, counter boundaries and
archive hashes. Recomputing per-job latency percentiles requires the raw logs.
