# Dispatch order from predicted bursts

Status: **first run done, 2026-11-16. The planned change (A) failed. Variant B passed three of four workloads and needs a confirmation run.**

## Diagnosis from Phase 2

The Phase 2 raw trials (`results/phase2/raw/primary-r0*.tar.gz`), split by burst class, show where `mixed` loses. All eight workers share one CPU: two 300 µs bursts, two 3 ms bursts and four CPU-bound workers.

| p99, median of 5 repetitions | EEVDF | fixed | online | scx_bpfland |
| --- | --- | --- | --- | --- |
| 300 µs burst completion | 9.03 ms | 2.33 ms | 2.25 ms | 2.60 ms |
| 3 ms burst completion | 27.7 ms | 29.4 ms | 29.4 ms | 9.75 ms |
| 3 ms burst wake to run | 10.1 ms | 9.7 ms | 24.0 ms | 10.0 ms |
| CPU worker max progress gap | 20 ms | 39 ms | 58 ms | 499 ms |

- **The online wake regression moves waiting rather than adding it.** Online gives a 3 ms burst one 3 ms slice instead of 2 ms pieces, so the burst no longer waits mid-way. It then waits about 17 ms at the next wakeup, because it has spent its vtime credit. The completion time is unchanged.
- **scx_bpfland's lower 3 ms completion comes from starving the CPU-bound workers.** Their longest progress gap is 499 ms.
- **Lertick is about 6% slower than EEVDF on 3 ms completion in both fixed and online modes.**

## Hypothesis

A slice longer than the fixed slice only takes effect while other tasks wait, because `dispatch()` already renews an uncontended task. A prediction should therefore decide the dispatch order, not the length of the run.

The change keeps the charged vtime in the task context and inserts each task at the deadline `vtime + request × 100 / weight`, as EEVDF does. `request` is the predicted remaining burst, bounded to the slice range, or the fixed slice when there is no prediction. Adaptive slices are capped at the fixed slice. Fixed mode keeps its order, because every request is the fixed slice.

| Build | Order by request | Cap adaptive slice |
| --- | --- | --- |
| `base` (`ccfe5b3`) | no | no |
| `A` | yes | yes |
| `B` | no | yes |
| `C` | yes | no |

`B` and `C` isolate the two parts. Only the decided form is kept; the compile-time switches are removed afterwards.

## Success criteria

Online mode of build `A` against EEVDF (`default`) wins a workload only if all of these hold for medians of paired repetitions:

1. For every burst class, completion p99 and wake-to-run p99 are no more than 5% worse, and at least one of them is 10% or more better.
2. The CPU workers' max progress gap is no more than 1.5× EEVDF's.
3. Throughput is no more than 2% worse.

Against `base` online, no burst class may get more than 5% worse.

## Run

ARM64 QEMU/HVF, Linux `7.0.0-38-generic`, preemption `lazy`. The guest has 8 vCPUs, so runs use `--cpus 0,1,2,3` to match Phase 2. Workloads are `mixed`, `multicore`, `shortburst` and `wakeheavy`, with five repetitions. The runner refuses fewer than five repetitions per call, so each arm runs as a single call of `benchmarks/run_phase2.py` from its own build, and the order of the arms is shuffled. The arms therefore ran at different times, about four minutes apart, and are not interleaved by repetition:

```sh
python3 <build>/benchmarks/run_phase2.py --output runs/<build>-<policy> --policies <policy> \
  --workloads mixed,multicore,shortburst,wakeheavy --repetitions 5 \
  --seed 20261115 --cpus 0,1,2,3 --bpf-runtime
```

Builds `A`, `B` and `C` come from one source tree, compiled with `CLANG="clang-18 -D<switch>=0"`.

## Results, 2026-11-16

There are 120 trials, all of which passed the runner's checks. `trials.tar.gz` holds each trial's `trial.json`, the commands and the run log. The per-sample logs stay in the guest under `~/lertick-exp/runs`. Each value below is the median over 5 repetitions of the per-trial p99, in ms. "Worker gap" is the CPU workers' longest progress gap, and throughput is in M units/s.

| mixed | 300 µs completion | 3 ms completion | 300 µs wake | 3 ms wake | Worker gap | Throughput |
| --- | --- | --- | --- | --- | --- | --- |
| EEVDF | 12.12 | 30.99 | 11.30 | 14.22 | 27.8 | 681.8 |
| base fixed | 2.38 | 29.37 | 2.24 | 10.31 | 29.7 | 679.0 |
| base online | 2.26 | 27.00 | 1.91 | 23.94 | 26.6 | 680.1 |
| A | 2.61 | 30.41 | 2.03 | 15.61 | 44.9 | 666.2 |
| **B** | **2.35** | **27.25** | **1.97** | **9.26** | **29.6** | **682.6** |
| C | 2.26 | 27.00 | 1.89 | 23.97 | 24.6 | 684.5 |

| multicore | 300 µs completion | 3 ms completion | 300 µs wake | 3 ms wake | Worker gap | Throughput |
| --- | --- | --- | --- | --- | --- | --- |
| EEVDF | 5.28 | 15.10 | 4.92 | 4.50 | 23.9 | 2446.5 |
| base fixed | 1.64 | 9.47 | 1.25 | 1.90 | 12.2 | 2524.1 |
| base online | 1.83 | 11.98 | 1.55 | 4.22 | 12.5 | 2509.4 |
| A | 1.96 | 11.79 | 1.50 | 3.79 | 43.8 | 2466.8 |
| **B** | **1.60** | **9.49** | **1.23** | **2.00** | **39.3** | **2528.9** |
| C | 2.02 | 10.04 | 1.46 | 6.11 | 57.9 | 2115.2 |

| shortburst / wakeheavy | Burst completion | Burst wake | Worker gap | Throughput |
| --- | --- | --- | --- | --- |
| EEVDF | 4.32 / 4.69 | 4.12 / 4.47 | 11.7 / 14.3 | 672.5 / 620.7 |
| base fixed | 1.84 / 2.76 | 1.09 / 2.51 | 8.9 / 8.4 | 681.0 / 671.6 |
| base online | 2.44 / 2.76 | 1.82 / 2.55 | 13.0 / 16.5 | 666.2 / 666.2 |
| A | 2.01 / 2.75 | 1.71 / 2.55 | 22.9 / 8.3 | 672.4 / 670.7 |
| **B** | **1.84 / 2.74** | **1.22 / 2.50** | **8.9 / 9.2** | **680.6 / 672.1** |
| C | 2.03 / 2.77 | 1.84 / 2.65 | 22.7 / 29.2 | 664.9 / 598.7 |

### Against the criteria

- **A, the planned change, fails.** Its worker gap is 1.6× EEVDF's in `mixed` and 1.8× in `multicore`, and its 3 ms wake p99 in `mixed` is 10% worse than EEVDF's. Ordering by request lets short predicted bursts pass the CPU workers again and again. C, which only orders, fails the same way.
- **B only caps adaptive slices at the fixed slice.** It passes `mixed`, `shortburst` and `wakeheavy` against EEVDF:
  - Every burst class is at least 12% better, and up to 83% better.
  - The worker gap is within 1.06× of EEVDF's.
  - Throughput is not lower.
  - In `mixed`, the 3 ms wake p99 drops from 23.94 ms (base online) to 9.26 ms, which removes the regression in the README.
- **B fails `multicore` on the worker gap**: 39.3 ms against EEVDF's 23.9 ms, or 1.64×. The per-trial gaps spread widely (EEVDF 11.8–97.1 ms, B 10.0–52.6 ms), so five repetitions do not settle this.
- **B is no more than 5% worse than base online in any burst class.**

### What this does and does not show

- **B is close to the fixed slice.** Against base fixed, B's 3 ms completion in `mixed` is 7% lower and everything else is within noise. Most of the gain over EEVDF comes from Lertick's dispatch with short fixed slices, not from learning.
- **B was not the arm the criteria named.** It needs a confirmation run with fresh seeds and more repetitions before it can be called a win, including a check of the `multicore` worker gap.
- **The run had no `scx_bpfland` arm**, so the README's `mixed` regression against scx_bpfland is not retested here.
- **EEVDF's 300 µs completion p99 in `mixed` was 12.1 ms here against 9.0 ms in Phase 2.** The guest has changed since Phase 2, so compare arms within one run only.

## Confirmation run for B, criteria fixed 2026-11-16 before it starts

The question is whether B (adaptive slices capped at the fixed slice, no ordering by request) beats EEVDF, using fresh seeds and twice the repetitions. The criteria are the ones above, now applied to B. The arms are EEVDF, base fixed, base online, B and `scx_bpfland`. The `scx_bpfland` arm is for information only and is not part of the criteria.

- **Repetitions:** 10 per arm, in two blocks of 5 with seeds 20261116 and 20261117. The arms run in a shuffled order in the first block and in reverse order in the second, so slow drift in the guest does not favor one arm.
- **Win per workload:** the medians over the 10 repetitions meet criteria 1 to 3 against EEVDF.
- **Confirmed:** B wins `mixed`, `shortburst` and `wakeheavy` again, and is no more than 5% worse than base online in any burst class. `multicore` is reported either way.

## Confirmation result, 2026-11-16: B is not confirmed

There are 200 trials (5 arms × 4 workloads × 10 repetitions), all of which passed the runner's checks. `confirm-summary.jsonl` holds one row per trial; `judge.py confirm-summary.jsonl` reproduces the verdicts. The raw archives stay in the guest under `~/lertick-exp/confirm`.

| B against EEVDF | mixed | multicore | shortburst | wakeheavy |
| --- | --- | --- | --- | --- |
| Burst classes | -3% to -83%, except 3 ms completion +3% | -35% to -72% | -52% to -55% | -39% to -41% |
| Worker gap | **1.98×** | 0.71× | **1.51×** | 1.12× |
| Throughput | **-2.5%** | -0.1% | -0.8% | +6.2% |
| Verdict | no win | win | no win | win |

- **B fails the confirmation.** It also gets worse than base online: in `mixed`, 300 µs completion is 15% higher, 3 ms completion 20% higher and the worker gap 52.7 ms against 25.6 ms. In `shortburst`, wake p99 is 63% higher. B stays an experiment and does not replace base online.
- **The likely cause is adaptive slices shorter than the fixed slice.** Under B, online can give a burst less than 2 ms when the prediction is low. A 3 ms burst then runs in shorter pieces and waits more often. This was not measured directly.

### Exploratory finding: fixed mode beats EEVDF in all four workloads

This was not the hypothesis under test. Applying the same criteria to the other arms shows:

| Against EEVDF, criteria 1–3 | mixed | multicore | shortburst | wakeheavy |
| --- | --- | --- | --- | --- |
| base fixed | **win** (gap 1.17×) | **win** | **win** | **win** (throughput +6.8%) |
| base online | no: 3 ms wake +74% | no: 3 ms wake +8% | win | win |

Fixed mode also met the criteria in all four workloads in the first run. In both runs, then, Lertick's dispatch with a fixed 2 ms slice beats EEVDF on these four workloads, and online's adaptive slices are what lose `mixed` and `multicore`. `cpu`, `affinity`, `memory` and `kernelcompile` have not been rerun against EEVDF.
