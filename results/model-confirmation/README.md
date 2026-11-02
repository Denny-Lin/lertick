# Burst-model confirmation

The model and scheduler implementation remain frozen at `6beaba2`; the
workload/measurement implementation is `0b23633`. Parameters, caps, flags and
watchdog thresholds are unchanged. See [the predeclared protocol](PROTOCOL.md)
and [source hashes](design.json). Original defaults remain bias/trunc.

## First complete environment

Forty full-preemption trials completed: four workloads, five paired
repetitions, 30 seconds measured after five seconds of warmup, sample pages
explicitly prefaulted. Candidate EWMA/carry is compared with original
bias/trunc in fixed-slice shadow mode.

| Workload | Baseline MAE us | Candidate MAE us | Paired MAE change | Coarse bootstrap interval | Candidate / same-episode EWMA |
| --- | ---: | ---: | ---: | --- | ---: |
| mixed | 18.439 | 7.707 | -58.20% | [-77.27%, +93.20%] | 1.0142 |
| multicore | 31.881 | 16.381 | -31.88% | [-51.01%, +5.61%] | 1.0121 |
| shortburst | 15.824 | 5.503 | -61.16% | [-65.22%, -3.23%] | 1.0031 |
| wakeheavy | 20.086 | 4.005 | -80.06% | [-82.74%, -40.73%] | 0.9983 |

Mixed and multicore have individual paired regressions and intervals crossing
zero. The candidate largely catches up to EWMA; these results do not establish
a consistent learned advantage over it. Trial, not episode, is the bootstrap
unit. Each trial has its own observed labels: the same-episode ratio compares
model and EWMA inside that trial, while across-policy changes compare separate
live runs. One-second counter boundaries are approximate.

[Full analysis](analysis/stable-full--full-prefault.json) and all compact
trial/counter-boundary records under `compact/` preserve the data. Full raw
archives remain outside Git under `results/local/model-confirmation-0b23633/`.

## Unresolved lazy-mode fallback

The initial lazy session completed 16 trials and failed the next mixed
bias/trunc shadow trial: the kernel reported `lertick_c07` runnable but not
run for 1.113 seconds. It restored Linux scheduling automatically and the
original lazy mode remained selected. This session is incomplete and does
not enter a five-repetition performance comparison. Its successful prefix
and failed trial remain in the complete raw archive.

The host display turned on close to the guest fallback. There is no matching
system-sleep event, and timing alone does not establish a cause. Five additional
read-only traced diagnostic runs did not reproduce the failure; their smoke
status excludes them from performance evidence. A single complete retry of
the unchanged 40-trial session is in progress. Any later success must not be
reported as fixing or erasing this reliability failure.

[Original failure](failures/stable-lazy-initial/failure.json),
[kernel scheduler dump](failures/stable-lazy-initial/scheduler.log),
[confirmation state](failures/stable-lazy-initial/confirmation.json),
[diagnosis and limits](failures/stable-lazy-initial/diagnosis.json), and
[raw archive hash](failures/stable-lazy-initial/archive.json) retain the record.
The kernel/model source has not been changed in response to this event.
Remaining planned environments and utility measurements are still pending.
