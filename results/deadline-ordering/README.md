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

## Fixed mode on the other four workloads, criteria fixed 2026-11-16 before the run

Base fixed is now the candidate. This run tests the four Phase 2 workloads not yet rerun, with EEVDF, base fixed and base online as arms. There are 10 repetitions per arm, in two blocks of 5 with seeds 20261118 and 20261119: the arms are shuffled in the first block and reversed in the second. The options are `--cpus 0,1,2,3 --bpf-runtime`, and for `kernelcompile` also `--kernel-source ~/reference/linux-7.0 --kernel-build ~/reference/linux7 --perf /usr/bin/perf`.

**Difference from Phase 2:** `memory` uses `--memory-mib 3000` instead of 3600. The guest has 3.9 GiB and no swap now (the Phase 2 swap file was removed to free disk space), so 3600 MiB risks the OOM killer. This workload is therefore lighter memory pressure than in Phase 2.

Against EEVDF, using medians over 10 repetitions:

| Workload | Win | No loss |
| --- | --- | --- |
| `cpu`, `affinity` (no sleeping bursts) | Throughput +2% or more, and worker gap ≤ 1.5× | Throughput within ±2%, and worker gap ≤ 1.5× |
| `memory` | Criteria 1–3 above, with the 300 µs burst class | — |
| `kernelcompile` | Build completion p99 and compiler wake p99 each no more than 5% worse, one of them 10% or more better, and rebuilds/s no more than 2% worse | Both p99 within ±5%, and rebuilds/s within ±2% |

Anything else is a loss. "No loss" is reported separately and is not counted as a win.

## schbench and hackbench, criteria fixed 2026-11-16 before the run

These are community benchmarks rather than this project's own fixtures. The arms are EEVDF, base fixed and base online. Both Lertick arms run with `--track-all`, as the service does. There are 10 repetitions; the arm order is shuffled per repetition, and each arm runs every configuration under one attach. Benchmarks are pinned to CPUs 0–3 with `taskset`. The driver is `bench.sh`.

- **schbench** is pinned to kernel.org commit `6300b8f`, with 30 s runs after a 5 s warm-up, in three configurations:
  - light: `-m 1 -t 2`
  - full: `-m 1 -t 4`
  - overload: `-m 2 -t 6`
- **hackbench** is built from the rt-tests source at tag `v2.5`, the same version as Ubuntu's `rt-tests` 2.5-1 package. The package could not be installed because the guest's apt is blocked by a half-installed `linux-image-6.8.0-149-generic`, which was left alone. hackbench runs in two configurations: `-g 8 -l 2000` (sockets) and `-p -g 8 -l 2000` (pipes).

Against EEVDF, using medians over 10 repetitions, per configuration:

| Benchmark | Win | No loss |
| --- | --- | --- |
| schbench | Wakeup p99 10% or more better, request p99 no more than 5% worse, and RPS p50 no more than 2% worse | Wakeup and request p99 within ±5%, and RPS within ±2% |
| hackbench | Time 10% or more lower | Time within ±2% |

Anything else is a loss.

## Result: fixed mode on the other four workloads, 2026-11-16

There are 120 trials, all of which passed the runner's checks. `rest-summary.jsonl` holds one row per trial. `rest-kernelcompile-memory.jsonl` holds the build completion and compiler wake p99 values, which are nested in `trial.json`. Values are medians over 10 repetitions.

| Workload | EEVDF | base fixed | base online | fixed against EEVDF |
| --- | --- | --- | --- | --- |
| `cpu`: throughput (M/s), worker gap (ms) | 613.3, 110.5 | 666.5, 45.7 | 654.8, 51.4 | **win**: +8.7%, gap 0.41× |
| `affinity`: throughput, worker gap | 1561.7, 71.8 | 2222.8, 30.0 | 2135.8, 45.8 | **win**: +42.3%, gap 0.42× |
| `memory`: 300 µs completion and wake p99 (ms), worker gap, throughput | 3.82, 2.91, 27.1, 1554.5 | 2.19, 1.33, 12.9, 1563.5 | 2.46, 1.21, 17.7, 1635.3 | **win**: -43%, -54%, gap 0.48×, +0.6% |
| `kernelcompile`: build p99 (s), compiler wake p99 (ms), rebuilds/s | 3.433, 0.548, 0.321 | 3.042, 0.763, 0.382 | 2.799, 0.691, 0.390 | **loss**: build -11.4%, rebuilds/s +19.0%, but wake p99 **+39.1%** |

Combined with the confirmation run, fixed mode beats EEVDF in seven of the eight Phase 2 workloads. It loses `kernelcompile` on the compiler's wake p99, 0.55 ms against 0.76 ms, even though the builds finish faster. Online loses the same way, at +26%.

**Next target:** compiler wake latency. Lertick has no wakeup preemption, so a woken compiler waits for the current 2 ms slice to end, whereas EEVDF can preempt on wakeup. This explanation has not been measured.

## Result: schbench and hackbench, 2026-11-16

There are 30 runs (3 arms × 10 repetitions), each covering all five configurations. `bench.tar.gz` holds the raw outputs and `bench_judge.py bench` reproduces the table. Values are medians over 10 repetitions: schbench wakeup and request p99 in µs and RPS p50, and hackbench time in seconds.

| Configuration | EEVDF | base fixed | base online | fixed against EEVDF |
| --- | --- | --- | --- | --- |
| schbench light | 189.5 / 19744 / 187.5 | 170.5 / 16800 / 188.0 | 175.0 / 17536 / 188.0 | **win**: wake -10.0%, request -14.9% |
| schbench full | 240.5 / 27552 / 269.5 | 72.0 / 30560 / 262.5 | 73.0 / 27840 / 261.5 | **loss**: wake -70.1%, but request +10.9% and RPS -2.6% |
| schbench overload | 7560 / 408576 / 110.5 | 7136 / 538624 / 94.5 | 7632 / 564224 / 95.5 | **loss**: request +31.8%, RPS -14.5% |
| hackbench socket | 2.137 | 2.797 | 3.173 | **loss**: +30.9% |
| hackbench pipe | 1.047 | 1.286 | 1.370 | **loss**: +22.9% |

- **The Phase 2 wins do not carry over to throughput under heavy load.** Fixed wins only schbench light. Wakeup latency is much lower with a full machine, but requests take longer and throughput falls. Under overload and in both hackbench modes, Lertick is 15–31% slower than EEVDF.
- **Reliability:** in repetition 8, online fell back to EEVDF during hackbench with `runnable task stall (hackbench[405314] failed to run for 1.364s)`. The kernel restored its default scheduler. The fixed arm had no fallback. That online repetition ran partly under EEVDF; its values remain in the medians, and its loader log is in the archive.
- **Likely cause, not yet measured:** Lertick dispatches every task from one global vtime queue (`SHARED_DSQ`). It has no per-CPU queues and no wake-affine placement, so tasks that wake each other at a high rate lose cache locality and contend on one queue. EEVDF keeps per-CPU run queues. Before changing the policy, the next step is to measure where hackbench loses time: queue contention, migrations or wake-to-run.

## Where hackbench loses time, 2026-11-16

The measurement uses `hackbench -g 8 -l 2000` (sockets, 321 tasks on CPUs 0–3), 5 runs per arm. `hackbench-wait.bt` records each hackbench task's runnable wait, from wakeup or preemption to the next switch-in.

| Median of 5 runs | EEVDF | Lertick fixed |
| --- | --- | --- |
| hackbench time (s) | 1.46 | 2.00 (+37%) |
| Waits over 100 ms | 94 | 657 |
| Longest wait (ms) | 187 | 456 |
| Preemptions of a running hackbench task | 87,805 | 51 |

- **The loss is long runnable waits, not CPU overhead or migration.** Total task-clock, context switches and CPU migrations under `perf stat` are similar between the arms. The top kernel symbols under `perf record` are the same socket and slab paths in both. Lertick, however, leaves about 7× as many waits above 100 ms, and its longest wait is about 2.5× EEVDF's. A hackbench group finishes only when its slowest task does, so the tail sets the time.
- **The same tail produces the stalls.** During this diagnosis, fixed mode was once removed by the kernel watchdog with `runnable task stall (hackbench[412745] failed to run for 1.009s)`. Online fell back the same way during the schbench/hackbench run. The `perf stat`/`perf record` profile above was taken around that fallback, so only the wait measurements, which had no fallback, are used here.
- **Lertick almost never preempts.** EEVDF preempted running hackbench tasks about 88,000 times per run, against about 50 for Lertick. A task woken while all four CPUs are busy waits for a running task to sleep or use up its 2 ms slice.

### Hypothesis for the next change

`vtime_now` only moves when a task with a higher vtime starts running. A woken task is placed at `vtime_now − max_slice` or later. Short sleepers that keep waking therefore stay near `vtime_now` and keep sorting ahead of tasks that have already run, while `vtime_now` itself barely advances. A task that has run waits until the stream of wakers pauses, which matches the hundreds of milliseconds observed and the stalls of 1 s or more. Advancing the queue's virtual clock with the service actually delivered, as EEVDF does with its average vruntime, should bound these waits. A wakeup preemption bound would address the second gap. The change goes into the shared dispatch path, so both online and fixed modes get it, and online must still beat fixed.

## Held-out and scaling check, criteria fixed 2026-11-16 before the run

Every configuration in the screening rounds came from the same schbench and hackbench settings, so a win there may be fitted to them. This check uses only tests and settings that no screening round used. It adds two sched_ext schedulers from the community repository as references, and repeats everything on 8 CPUs to see how the single shared queue scales.

- **Arms:** EEVDF, release online (AI), release fixed, `scx_bpfland` and `scx_lavd` (sched-ext/scx 1.1.2 builds, default options).
  - **Change before any result:** `scx_lavd` 1.1.2 failed the BPF verifier on this kernel (`arena_init`: `R6 invalid mem access 'scalar'`, load `-EACCES`) and never attached, so it is dropped. The other arms are unchanged.
- **Tests:**
  - `perf bench sched messaging`, processes and threads
  - `perf bench sched pipe`
  - schbench `-m 4 -t 2` and `-m 1 -t 1`
  - hackbench threads `-T -g 4 -l 2000` and processes `-g 16 -l 1000`
  - kernel compile: rebuilding `kernel/sched/` objects with `make -j<CPUs>` in a private copy of the build tree
- **Repetitions:** 5 per arm. Arm order is shuffled per repetition. Runs on CPUs 0–3, then on 0–7.
- **Per test against EEVDF:** a win is the median better by 5% or more (lower time or latency p99, higher RPS), a tie within ±5%, and a loss anything worse. Results are reported per test and CPU count, wins and losses alike.

## Release confirmation, 2026-11-16

This run fixes the README numbers. It compares the release candidate (`69a67ff`, plus a later change to comments only) with EEVDF and the previous release (`ccfe5b3`), using `SBTIME=30 SBWARM=5 eval.sh release 10 default main-on=base:online rel-on=release:online rel-fx=release:fixed`. There are 40 runs, each covering all five configurations. `release-confirmation.tar.gz` holds every raw output; `eval_judge.py eval/release main-on` reproduces the ratios.

Values are medians of 10 repetitions:

| Configuration | EEVDF | rel-on | rel-fx | main-on |
| --- | ---: | ---: | ---: | ---: |
| hackbench pipe (s) | 0.8255 | 0.6990 | 0.6845 | 1.0885 |
| hackbench socket (s) | 1.6250 | 1.4990 | 1.5425 | 2.1040 |
| schbench full wakeup p99 (µs) | 120.5 | 47 | 47 | 47 |
| schbench full request p99 (µs) | 18,240 | 17,152 | 17,408 | 17,280 |
| schbench full RPS | 302 | 303 | 303 | 301.5 |
| schbench light wakeup p99 (µs) | 90.5 | 91 | 96 | 97.5 |
| schbench light request p99 (µs) | 11,760 | 11,712 | 11,904 | 12,352 |
| schbench light RPS | 214.5 | 215 | 214.5 | 214 |
| schbench overload wakeup p99 (µs) | 8,944 | 8,128 | 8,008 | 7,752 |
| schbench overload request p99 (µs) | 379,392 | 371,712 | 347,648 | 482,816 |
| schbench overload RPS | 124.5 | 119 | 118.5 | 106 |

- **Against EEVDF, the geometric mean of the ratios is 0.887 for rel-on and 0.888 for rel-fx** (below 1 is better). The only metric worse than EEVDF by more than 2% is overload RPS, at 4.4% fewer requests per second for rel-on. Online and fixed are equal within noise.
- **Fallbacks:** main-on fell back once in repetition 6, with `runnable task stall (kworker/7:1[752044] failed to run for 1.001s)`. rel-on and rel-fx had none.
- **Correctness of the release build** (see the commit history):
  - The model, accounting, concurrent-fork, fairness and recovery checks pass.
  - Cgroup weight 200:100 gives a 2.00 CPU-time ratio, equal-weight groups with 2:1 tasks give 1.00, and `nice` 0 against 5 gives 3.07; EEVDF gives 2.00, 1.00 and 3.05.
  - The setup-label check in the lifecycle suite fails intermittently on this 8-vCPU guest, for `ccfe5b3` as well.

### Held-out result on 4 CPUs, 2026-11-16

There are 20 runs (4 arms × 5 repetitions) with no fallbacks. `heldout-cpu4.tar.gz` holds the raw outputs; `heldout_judge.py heldout/cpu4` reproduces the table. Values are medians; the percentage is the change against EEVDF, where negative is better, and the verdict uses the 5% threshold.

| Test | EEVDF | Lertick online | Lertick fixed | scx_bpfland |
| --- | ---: | ---: | ---: | ---: |
| perf bench messaging, processes (s) | 0.619 | 0.616 (same) | 0.676 (+9%, loss) | 0.735 (+19%, loss) |
| perf bench messaging, threads (s) | 0.479 | 0.475 (same) | 0.455 (−5%, win) | 0.601 (+25%, loss) |
| perf bench pipe (µs/op) | 20.38 | 1.606 (−92%, win) | 1.598 (−92%, win) | 1.854 (−91%, win) |
| schbench `-m 4 -t 2` wakeup p99 (µs) | 3,780 | 5,384 (+42%, loss) | 4,984 (+32%, loss) | 5,336 (+41%, loss) |
| schbench `-m 4 -t 2` request p99 (µs) | 70,270 | 161,500 (+130%, loss) | 145,200 (+107%, loss) | 385,500 (+449%, loss) |
| schbench `-m 4 -t 2` RPS | 165 | 162 (same) | 167 (same) | 155 (+6%, loss) |
| schbench `-m 1 -t 1` wakeup p99 (µs) | 92 | 86 (−7%, win) | 73 (−21%, win) | 71 (−23%, win) |
| schbench `-m 1 -t 1` request p99 (µs) | 15,220 | 11,380 (−25%, win) | 9,744 (−36%, win) | 11,280 (−26%, win) |
| hackbench threads `-T -g 4` (s) | 1.150 | 0.770 (−33%, win) | 0.768 (−33%, win) | 0.908 (−21%, win) |
| hackbench `-g 16` (s) | 1.731 | 1.718 (same) | 1.891 (+9%, loss) | 2.381 (+38%, loss) |
| kernel/sched rebuild (s) | 2.434 | 2.660 (+9%, loss) | 2.541 (+4%, same) | 2.875 (+18%, loss) |

- **Lertick online wins 4, ties 4 and loses 3** of the 11 held-out measures. Fixed wins 5, ties 2 and loses 4. scx_bpfland wins 4 and loses 7.
- **The tuned win partly carries over.** hackbench in thread mode, `perf bench pipe` and the light schbench case are clear wins, and messaging and `hackbench -g 16` are ties.
- **schbench with 4 message threads is a large loss.** Request p99 is 2.3× EEVDF's and wakeup p99 is 42% higher. EEVDF also beats scx_bpfland there, so this load pattern, many message threads waking groups of workers, favours EEVDF's per-CPU wakeup handling.
- **Kernel compilation is 9% slower in online mode.**
- **Online against fixed:** online has fewer losses (3 against 4) but fewer wins. Neither mode dominates.
- **perf bench pipe takes 20 µs per round trip under EEVDF in this guest, against about 1.6 µs for all three sched_ext schedulers.** This is probably an artefact of the guest that has not been investigated, for example EEVDF placing the two ends on different vCPUs and paying a cross-vCPU wakeup. It should not be read as a general result.

### Held-out result on 8 CPUs, 2026-11-16

The same 11 measures, rerun with all 8 vCPUs (`heldout.sh cpu8 0,1,2,3,4,5,6,7 5 …`). There are 20 runs with no fallbacks. `heldout-cpu8.tar.gz` holds the raw outputs; `heldout_judge.py heldout/cpu8` reproduces the verdicts, and `heldout-cpu8-judge.txt` is its output. Values are medians; the percentage is the change against EEVDF, where negative is better for times and latencies and positive is better for RPS.

| Test | EEVDF | Lertick online | Lertick fixed | scx_bpfland |
| --- | ---: | ---: | ---: | ---: |
| perf bench messaging, processes (s) | 0.532 | 0.562 (+6%, loss) | 0.543 (+2%, same) | 0.691 (+30%, loss) |
| perf bench messaging, threads (s) | 0.432 | 0.473 (+9%, loss) | 0.410 (−5%, win) | 0.628 (+45%, loss) |
| perf bench pipe (µs/op) | 21.96 | 2.347 (−89%, win) | 3.544 (−84%, win) | 2.858 (−87%, win) |
| schbench `-m 4 -t 2` wakeup p99 (µs) | 171 | 217 (+27%, loss) | 275 (+61%, loss) | 498 (+191%, loss) |
| schbench `-m 4 -t 2` request p99 (µs) | 37,056 | 35,904 (−3%, same) | 39,744 (+7%, loss) | 41,664 (+12%, loss) |
| schbench `-m 4 -t 2` RPS | 354 | 373 (+5%, win) | 366 (+3%, same) | 375 (+6%, win) |
| schbench `-m 1 -t 1` wakeup p99 (µs) | 104 | 71 (−32%, win) | 76 (−27%, win) | 89 (−14%, win) |
| schbench `-m 1 -t 1` request p99 (µs) | 13,104 | 10,256 (−22%, win) | 10,864 (−17%, win) | 12,208 (−7%, win) |
| hackbench threads `-T -g 4` (s) | 1.178 | 0.835 (−29%, win) | 0.875 (−26%, win) | 0.940 (−20%, win) |
| hackbench `-g 16` (s) | 1.535 | 1.271 (−17%, win) | 1.489 (−3%, same) | 2.057 (+34%, loss) |
| kernel/sched rebuild (s) | 2.815 | 2.853 (+1%, same) | 2.903 (+3%, same) | 2.772 (−2%, same) |

- **Lertick online wins 6, ties 2 and loses 3.** Fixed wins 5, ties 4 and loses 2. scx_bpfland wins 5, ties 1 and loses 5.
- **The online mode scales better than it did on 4 CPUs.** `hackbench -g 16` turns from a tie into a 17% win, the schbench `-m 4 -t 2` request p99 loss (2.3× on 4 CPUs) disappears, and the kernel/sched rebuild is a tie.
- **The remaining losses are wakeup-heavy.** schbench `-m 4 -t 2` wakeup p99 is still 27% higher, and both `perf bench messaging` cases lose (6% and 9%) where they tied on 4 CPUs. Fixed mode ties or wins messaging, so the learned slice is the likely cause there and is the next thing to look at.
- **EEVDF's pipe time is again about 22 µs per round trip in this guest,** so the pipe win is still treated as a likely VM artefact.

## Longer kernel compile, criteria fixed 2026-11-16 before the run

The held-out kernel/sched rebuild takes under 3 s and its spread across runs (2.2 to 3.2 s) is larger than the differences between schedulers. This test rebuilds every object under `kernel/` from a clean state instead.

- **Script:** `kbuild-long.sh LABEL CPUS REPS ARM…`. Each run deletes the `.o` and `.a` files under the private build tree's `kernel/`, then times `make -j<CPUs> kernel/` pinned to the CPU set. One untimed warm-up build fills the page cache first. Arm order is shuffled per repetition with a fixed seed.
- **Arms:** EEVDF (sched_ext off), Lertick online (release build) and scx_bpfland 1.1.2.
- **Sets:** 4 CPUs (0–3) and 8 CPUs (0–7), 5 repetitions each.
- **Verdict:** the median wall time of each arm against EEVDF. Better or worse if the median differs by more than 3%, otherwise the same. The min and max of every arm are reported next to the median. Any fallback or failed build voids that run.

## Longer kernel compile result, 2026-11-16

`kbuild-long.tar.gz` holds every run, including the discarded first 8-CPU set. Each run rebuilds all of `kernel/` (about 30 s); values are wall-clock seconds.

| CPUs | EEVDF median (min–max) | Lertick online | scx_bpfland |
| --- | --- | --- | --- |
| 4 | 30.12 (28.93–31.50) | 28.41 (27.20–31.77), −5.7%, better | 28.95 (26.60–30.74), −3.9%, better |
| 8 | 25.69 (23.80–28.73) | 25.68 (24.87–33.43), −0.1%, same | 26.44 (23.59–28.20), +2.9%, same |

- **The short kernel/sched rebuild's 9% loss on 4 CPUs does not hold up.** With ten times more work per run, Lertick is 5.7% faster than EEVDF on 4 CPUs and level on 8.
- **The first 8-CPU set was discarded before judging.** Between 19:10 and 19:16 guest time, both sched_ext arms took 51 to 151 s and EEVDF 33 s, while the host was cloning the repository. With all 8 vCPUs busy, host load reaches the guest directly. The set was moved to `kbuild-long/cpu8-disturbed` and rerun with the host idle; the table uses the rerun.
- **The README uses the held-out 5% threshold for every row,** so scx_bpfland's −3.9% on 4 CPUs is shown there as the same.

## scx_lavd as a second reference, criteria fixed 2026-11-16 before the run

scx_lavd 1.1.2 and 1.1.4 failed to load when built with the guest's clang 18: the verifier rejected `arena_init` with `R6 invalid mem access 'scalar'`. lavd keeps data in a BPF arena, and arena pointers need address-space casts that clang 18 does not emit, so the verifier saw a plain integer used as a pointer. The same lavd 1.1.4 source built with the official LLVM 20.1.8 release (SHA-256 checked against the release attestation) loads and runs on this kernel. The fault was the compiler, not lavd or the kernel.

- **Arms:** EEVDF, Lertick online (release build) and scx_lavd 1.1.4 with default options, in fresh sessions so that each set has its own EEVDF baseline.
- **Held-out:** `heldout.sh` on 4 CPUs and on 8 CPUs, 5 repetitions each, judged as before (5% threshold).
- **Kernel compile:** `kbuild-long.sh` on 4 and 8 CPUs, 5 repetitions each, judged as before (3% threshold).
- No other work runs on the host during the 8-CPU sets.

## scx_lavd result, 2026-11-16

`lavd-sets.tar.gz` holds the four sets (`heldout/cpu4-lavd`, `heldout/cpu8-lavd`, `kbuild-long/cpu4-lavd`, `kbuild-long/cpu8-lavd`); `heldout_judge.py` reproduces the held-out verdicts. All 60 runs completed with no fallbacks. Lertick against lavd, from the same sessions:

| Measure | 4 CPUs | 8 CPUs |
| --- | --- | --- |
| perf messaging, processes | 8% faster | 8% faster |
| perf messaging, threads | 17% faster | same (+5%) |
| perf pipe | 93% faster | 87% faster |
| hackbench threads | 18% faster | 9% slower |
| hackbench `-g 16` | 14% faster | 8% faster |
| schbench `-m 1 -t 1` request p99 | same | 21% higher |
| schbench `-m 1 -t 1` wakeup p99 | 38% lower | 34% lower |
| schbench `-m 4 -t 2` request p99 | 27% lower | 16% lower |
| schbench `-m 4 -t 2` wakeup p99 | 84× higher (5,544 against 66 µs) | 21% lower |
| schbench `-m 4 -t 2` RPS | same | same |
| Kernel compile (`kernel/`) | same | same |
| **Total** | **7 better, 3 same, 1 worse** | **6 better, 3 same, 2 worse** |

- **lavd's 4-CPU wakeup p99 of 66 µs is the motivation for the wakeup preemption screening below.** On 8 CPUs, where there are spare CPUs, the gap disappears.
- **EEVDF and lavd both take about 20 µs per `perf bench pipe` round trip, against 1.5 to 2.9 µs for Lertick and scx_bpfland.** A sched_ext scheduler is not fast here by itself, so the pipe gap is better explained by Lertick and bpfland keeping both ends on one CPU than by a VM artefact. The README footnote now states the measurement instead of calling it an artefact.
- **Session-to-session variation is large.** In these sessions, Lertick against EEVDF on 8 CPUs won `perf messaging` processes (−9%) and tied threads, where the earlier 8-CPU session showed losses of 6% and 9%; EEVDF's schbench `-m 4 -t 2` wakeup p99 was 89 µs here against 171 µs earlier. The README keeps the earlier sessions for EEVDF and bpfland and these sessions for lavd, each compared within its own session.

## scx_p2dq and scx_flash as further references, criteria fixed 2026-11-16 before the run

- **Arms:** EEVDF, Lertick online (release build), scx_p2dq 1.1.4 (Meta, server-oriented) and scx_flash 1.1.4 (latency-oriented), both built with LLVM 20.1.8 and run with default options.
- **Sets and verdicts:** the same as for scx_lavd: `heldout.sh` and `kbuild-long.sh` on 4 and 8 CPUs, 5 repetitions each, 5% and 3% thresholds. `more-run.sh` starts them after the lavd sets finish and stops if either scheduler fails a 10-second load test.
- **README comparison:** each scheduler is compared with Lertick within the same session.

## Wakeup preemption and a shorter continuation cap, hypothesis fixed 2026-11-16 before the run

**Why.** schbench with 4 message threads (`-m 4 -t 2`) has 12 threads on 4 CPUs. Its wakeup p99 is 42% (4 CPUs) and 27% (8 CPUs) above EEVDF. scx_lavd 1.1.4 reaches 66 µs against EEVDF's 3,748 µs on 4 CPUs. Reading its source, it orders by a latency-criticality deadline and, on a wakeup, preempts the least critical of the preferred CPU and two random CPUs. Lertick queues a woken task in fair order and never preempts, so it waits for a slice to end (up to 5 ms), and `keep_prev` can let the running task continue for up to 20 ms. Earlier wakeup preemption (r1, r6) only kicked the target CPU and was judged on the tuned benchmarks, which do not include this case. lavd pays elsewhere (request p99 +213%, `hackbench -g 16` +28% on 4 CPUs), so only the preemption idea is tried, inside Lertick's fair order.

**Arms** (all built from the same source, `BPF_DEFS` selects the change):

- `base`: the current algorithm.
- `kickA`: `LERTICK_WAKE_KICK=1`. A woken task that is not ahead of the clock asks one CPU to yield: among its previous CPU and two random CPUs, the one whose current task is furthest ahead in vtime, if that task has run at least 0.5 ms and is more than 1 ms ahead.
- `kickB`: kickA plus `LERTICK_KEEP_MAX_NS=5 ms` (instead of 20 ms).
- `keepC`: the 5 ms cap alone.

**Screening** (`kick-run.sh`): the 11 held-out measures on 4 and on 8 CPUs, 3 repetitions, with EEVDF. A candidate goes on to a 5-repetition confirmation if it lowers schbench `-m 4 -t 2` wakeup p99 against `base` by more than 10% on both CPU counts, and is not worse than `base` by more than 5% on any other measure. It replaces `base` only if the confirmation shows it no worse than `base` on any measure on either CPU count, and the kernel compile (`kbuild-long.sh`) is no worse by more than 3%.

### Screening result: wakeup preemption rejected, 2026-11-16

`kick-sets.tar.gz` holds both sets (`heldout/kick-cpu4`, `heldout/kick-cpu8`, 3 repetitions, 5 arms). All four builds passed a 10-second load test; there were no fallbacks in 30 Lertick runs. schbench `-m 4 -t 2` wakeup p99, the target measure:

| Arm | 4 CPUs | 8 CPUs |
| --- | ---: | ---: |
| EEVDF | 3,788 µs | 142 µs |
| base | 5,064 µs | 200 µs |
| kickA | 5,368 µs (+6%) | 346 µs (+73%) |
| kickB | 5,336 µs (+5%) | 314 µs (+57%) |
| keepC | 5,224 µs (+3%) | 202 µs (+1%) |

- **No candidate meets the screening rule** (more than 10% lower than `base` on both CPU counts). Wakeup preemption makes the 8-CPU tail worse and also slows `perf messaging` processes on 8 CPUs by 14–16%. The 5 ms continuation cap changes nothing measurable.
- **The switches were removed** (revert of the experiment commit), as with earlier rejected switches.
- **Interpretation.** lavd's low tail does not come from preemption alone. Its deadline puts frequently waking, short-running tasks (here the message threads) far ahead of the workers, so they run almost at once; Lertick's fair vtime order treats both kinds of thread alike. Trying that would change the ordering rule itself and needs its own hypothesis.

## Runnable-task stalls in the p2dq and flash sessions, 2026-11-16

In `heldout/cpu4-more`, Lertick (release build) fell back to EEVDF in 2 of 5 repetitions. Both times the kernel watchdog reported a runnable hackbench task that had not run for over 1 s (1.108 s and 1.150 s); the loader sets a 1,000 ms watchdog timeout. The 20 earlier 4-CPU held-out runs, the 15 lavd runs and the 30 runs above had no fallback, so the stall is intermittent.

**Likely mechanism.** `hackbench -g 16` keeps hundreds of tasks runnable on 4 CPUs. The clock advances by service over the runnable weight, so with about 640 runnable tasks it moves about 6 ms per second. A task requeued after a 5 ms slice sits about 5 ms ahead of the clock, and every woken task is placed up to 2 ms behind it, so the requeued task waits until the clock advances about 7 ms: 640 × 7 ms / 4 CPUs ≈ 1.1 s, which matches the watchdog reports. Fair order alone does not bound the wait when the runnable count is large.

The p2dq and flash comparison from these sessions is kept here but not added to the README until the stall is fixed and the comparison is rerun with the fixed build. Lertick against each, from the same sessions, with the two fallback repetitions removed (4 CPUs: 3 repetitions):

| Lertick against | 4 CPUs | 8 CPUs |
| --- | --- | --- |
| scx_p2dq 1.1.4 | 7 better, 2 same, 2 worse | 3 better, 5 same, 3 worse |
| scx_flash 1.1.4 | 8 better, 2 same, 1 worse | 6 better, 3 same, 2 worse |

### Starvation guard, criteria fixed 2026-11-16 before the run

**Change.** Each enqueue into the shared queue records its time. A BPF timer every 50 ms moves up to 8 tasks that have been queued for more than 200 ms straight to their CPU's local queue and kicks that CPU. The new `stall_rescues` counter reports how often it acts. This bounds the wait well inside the loader's 1 s watchdog without changing fair order otherwise.

**Stress test** (`stall-test.sh`): `hackbench -g 16 -l 1000` and `-g 32 -l 1000` on CPUs 0–3, 10 repetitions, arms EEVDF, `base` (current algorithm) and `guard`, order shuffled per repetition. The guard passes if it has no fallback, its largest wake-to-run time stays under 500 ms, and its median hackbench times are no more than 5% slower than `base`. It then goes to the 5-repetition held-out and kernel compile confirmation on 4 and 8 CPUs against `base` and EEVDF, and replaces `base` only if no measure is worse than `base` on either CPU count.

**First stress result (`stall/s1`): the guard alone fails.** `base` and `guard` each fell back once in 10 repetitions (a hackbench task not run for 1.08 s), and the largest wake-to-run time stayed at 450–660 ms with the guard against 510–760 ms without it, although the guard moved 340–660 tasks per run. Both runs put about 100,000 tasks on local queues against about 10,000 on the shared queue: `sync_local` queues a sync wakee on the waker's CPU without looking at that CPU's local queue, which other CPUs cannot take from, so a chain of sync wakeups can pile tasks there out of the guard's reach.

**Second candidate (`guard2`):** the guard plus `sync_local` only onto an empty local queue. Same stress test and criteria (`ARMS="default base=... guard2=..." stall-test.sh s2 10`).

**Second stress result (`stall/s2`): `guard2` fails too.** Each of `base` and `guard2` fell back once in 10 repetitions; the largest wake-to-run time was 470–730 ms for `base` and 460–740 ms for `guard2`, and `guard2` made `hackbench -g 16` and `-g 32` 12% and 18% slower than `base`. Limiting `sync_local` cut local enqueues from about 158,000 to 65,000 per run without shortening the longest wait, so the local queues were not the cause. Both changes were reverted.

**Revised explanation.** With 640 or 1,280 hackbench tasks on 4 CPUs, a fair round alone takes several hundred milliseconds: `base` has a longest wake-to-run time of 470–760 ms in every one of its 20 stress runs, not only in the runs that fell back. The fallbacks are the tail of that distribution crossing Lertick's watchdog timeout, which is 1 s. The other sched_ext schedulers measured here use 5 s (scx_bpfland, scx_flash), 25 s (scx_p2dq) and 30 s (scx_lavd). The 1 s value dates from the project's first commit and has no recorded rationale.

### Watchdog timeout 5 s, criteria fixed 2026-11-16 before the run

The owner chose to raise the runnable-task watchdog timeout from 1 s to 5 s (`t5`), as in scx_bpfland and scx_flash. It changes no scheduling decision, so it can only remove fallbacks; it must still eject a scheduler that really stalls a task.

- **Stall stress** (`stall-test.sh s3 10`, arms EEVDF, `base`, `t5`): `t5` passes with no fallback in 10 repetitions and median hackbench times within 5% of `base`.
- **Watchdog still works:** `tests/validate_watchdog_flags.py` (expected and unexpected injected stalls) passes with `t5` in the VM.
- **No regression:** the held-out suite and `kbuild-long.sh` on 4 and 8 CPUs, 5 repetitions, against `base` and EEVDF; no measure worse than `base` by more than 5% (3% for the kernel compile) on either CPU count. Since scheduling is unchanged, a difference beyond that would point to noise and be rerun rather than accepted.

### Watchdog timeout 5 s: result

`t5-sets.tar.gz` holds the load test, the injected watchdog tests and the held-out and kernel compile sets; `stall-s3-s4.tar.gz` the two stress runs.

- **Load and watchdog:** `t5` loads with `timeout_ms` 5000, and `validate_watchdog_flags.py` passes (normal exit, expected and unexpected injected stalls all eject the scheduler and restore the default).
- **Stress:** `t5` had no fallback in 20 repetitions (`s3`, `s4`); `base` fell back once in `s4`. Median hackbench `t5` against `base`: −2.8% and +7.3% (`s3`), −0.4% and −3.1% (`s4`) for `-g 16` and `-g 32`. The +7.3% exceeded the 5% rule and was rerun as planned; the rerun is within it.
- **Held-out and kernel compile:** no fallback. `t5` against `base` was worse by 6–7% on three 4-CPU measures and better by 12–42% on four 8-CPU measures, with no 8-CPU loss. The two builds make identical scheduling decisions, so these differences measure the noise between two arms of one session, not the change.
- **Decision:** `t5` replaces `base`. It removes the fallbacks without changing scheduling.

## Pooled comparison with EEVDF over six sessions, 2026-11-16

The noise above made single-session verdicts against EEVDF suspect, so `pool.py` combines every held-out session that ran Lertick (the release algorithm) and EEVDF together: the first held-out sessions, the lavd, p2dq/flash, kick and t5 sessions, six per CPU count (the t5 session contributes both `base` and `t5`). Each session gives one percentage per measure; the verdict uses the median over sessions.

| Measure | 4 CPUs | 8 CPUs |
| --- | --- | --- |
| perf messaging, processes | same (−1%) | worse (−7%) |
| perf messaging, threads | same (+1%) | same (−3%) |
| perf pipe | better (+93%) | better (+89%) |
| schbench `-m 4 -t 2` wakeup p99 | worse (−47%) | worse (−32%) |
| schbench `-m 4 -t 2` request p99 | worse (−136%) | worse (−6%) |
| schbench `-m 4 -t 2` RPS | same (+4%) | same (0%) |
| schbench `-m 1 -t 1` wakeup p99 | same (+5%) | same (+5%) |
| schbench `-m 1 -t 1` request p99 | worse (−7%) | worse (−12%) |
| hackbench threads | better (+29%) | better (+11%) |
| hackbench `-g 16` | same (+1%) | same (+4%) |
| kernel/sched rebuild (short) | same (−4%) | same (0%) |
| **Total** | **2 better, 6 same, 3 worse** | **2 better, 5 same, 4 worse** |

- **Only two wins hold in every session:** `perf bench pipe` and hackbench in thread mode. The losses on schbench `-m 4 -t 2` also hold in every session.
- **The README's first held-out session was a favourable one.** It showed 4 wins on 4 CPUs and 6 on 8; pooled, both are 2. The other measures swing by 10–50 percentage points between sessions with no code change.
- **Consequence for reporting:** verdicts against EEVDF should come from pooled sessions, not from one session. The longer kernel compile (`kbuild-long`) was level or better in every session and is not in this table.

## Overnight sessions with an idle host, criteria fixed 2026-11-16 before the run

The six pooled sessions ran while the host Mac was also in use (a video call and video playback at times). Host load takes vCPUs away from the guest, and Lertick's single shared queue may suffer more from a vCPU paused while holding its lock than EEVDF's per-CPU queues, so host activity could bias results against Lertick rather than only add noise.

- **Sessions** (`night-run.sh`): two rounds, each with EEVDF, Lertick (`t5`, the 5 s watchdog build), scx_bpfland, scx_lavd, scx_p2dq and scx_flash in the same sessions: `heldout.sh` on 4 and 8 CPUs and `kbuild-long.sh` on 4 and 8 CPUs, 5 repetitions each. The host is not otherwise used.
- **Host evidence:** `host-load.sh` logs the Mac's load average and busiest processes every 10 s; the guest logs `/proc/stat` every 10 s for steal time.
- **Use:** if both rounds agree with each other and steal time stays low, they replace the pooled daytime sessions as the README's basis, with verdicts from the median of the two rounds and every scheduler compared within the same rounds. If the overnight results are clearly better for Lertick than the daytime ones, host load biased the daytime sessions.

### Evening session result, 2026-11-16

The schedule changed to one round (`night-run.sh 1 eve 0`, 15:29 to 16:47 host time) while the owner was away; another process on the host trained a model under a usage limit. Host load average 2.4 to 7.8 (mean 4.2, including the VM itself), guest steal time 0. No fallbacks in any arm.

Lertick (`t5`) against EEVDF in this session (positive is better for Lertick; pooled daytime value in brackets):

| Test | 4 CPUs | 8 CPUs |
|---|---|---|
| hackbench msg processes | +3 (-1) | +0 (-7) |
| hackbench msg threads | +2 (+1) | -5 (-3) |
| perf bench sched pipe | +92 (+93) | +92 (+88) |
| hackbench pipe threads | +26 (+29) | +16 (+11) |
| hackbench 16 groups | +1 (+1) | +2 (+4) |
| schbench -m1 -t1 request p99 | +1 (-7) | +0 (-12) |
| schbench -m1 -t1 wakeup p99 | +9 (+5) | +7 (+5) |
| schbench -m4 -t2 request p99 | -155 (-136) | -3 (-6) |
| schbench -m4 -t2 wakeup p99 | -40 (-47) | +27 (-32) |
| schbench -m4 -t2 RPS | +0 (+4) | +0 (+0) |
| kernel compile | -1 (+2) | +0 (+1) |

Better/same/worse at a 5% threshold: 3/6/2 on 4 CPUs and 4/7/0 on 8 CPUs. Against the other schedulers in the same session: scx_bpfland 7/4/0 and 6/4/1, scx_lavd 7/3/1 and 2/6/3, scx_p2dq 7/2/2 and 3/6/2, scx_flash 7/2/2 and 7/4/0.

Raw sets: `eve-sets.tar.gz` (held-out, kernel compile, guest `/proc/stat` log).

Verdict: the evening session agrees with the pooled daytime results within the known session-to-session swing. The 4-CPU schbench -m4 -t2 losses are as large as before, so host use did not cause them. The small daytime losses on 8-CPU messaging processes and on schbench -m1 -t1 request p99 did not appear in this session and may have been partly host noise; the 8-CPU -m4 -t2 wakeup win is a single session against a pooled loss and is not taken as a change. One round is not enough to replace the pooled basis, so the README keeps the pooled numbers.

## AI deadline order (`dl`), hypothesis fixed 2026-11-16 before the run

**Why.** The losses that hold in every session are latency with many message threads (schbench `-m 4 -t 2`) and single-thread request latency. EEVDF orders by virtual deadline (vruntime plus the requested slice over weight), and lavd puts short, frequently waking tasks first. Lertick orders by vtime alone; the model's predicted burst only sets the slice, which is why online and fixed modes score alike.

**Change** (`LERTICK_AI_DEADLINE=1`): the shared queue key becomes vtime plus the task's slice over its weight; in online mode the slice is the predicted burst (0.5–5 ms). The offset is removed when the task starts running, so charging is unchanged, and `keep_prev` adds prev's own offset before comparing. r4's earlier deadline variant hurt hackbench pipe (1.72×); it is not identical (it was combined with other changes and judged on the tuned set), so pipe and hackbench are watched closely.

**Screening** (`heldout.sh aidl-cpu4` and `aidl-cpu8`, 3 repetitions, arms EEVDF, `base` and `dl` built from the same source). `dl` against `base` is compared within each session, so host load affects both Lertick arms alike. `dl` goes on if schbench `-m 4 -t 2` wakeup p99 is lower than `base` by more than 10% on both CPU counts and no other measure is worse than `base` by more than 10% (the spread seen between identical builds in one session).

### Screening result: AI deadline order rejected, 2026-11-16

`aidl-sets.tar.gz` holds both sets (3 repetitions, EEVDF, `base`, `dl`). `dl` against `base`, same session:

| Measure | 4 CPUs | 8 CPUs |
| --- | ---: | ---: |
| schbench `-m 4 -t 2` wakeup p99 (target) | +1% | +15% |
| schbench `-m 4 -t 2` request p99 | +1% | +12% |
| perf messaging, processes | +8% | +6% |
| perf messaging, threads | +25% | −9% |
| hackbench `-g 16` | +7% | +11% |
| perf pipe | −45% | −29% |
| schbench `-m 1 -t 1` request p99 | −6% | +6% |

Positive is worse for `dl`. The target measure does not improve and several others get worse, so `dl` is rejected and the switch reverted. With one shared queue, ordering by predicted deadline moves short-burst tasks ahead of fair order without shortening the wakeup tail; this agrees with r4.

**Schedule change.** The idle-host comparison runs as one round (`night-run.sh 1 eve`) from 17:35 to about 20:00 local time on 2026-11-16, while the owner is out, instead of two rounds overnight. `host-load.sh` logs the host from 17:35 to 21:00. One round cannot show round-to-round spread on an idle host; it does show whether the daytime sessions were biased against Lertick.

## Learned continuation (`ek`), hypothesis fixed 2026-11-16 before the run

**Why.** Online and fixed modes score alike because the prediction only sets the slice, and most tasks sleep before any slice ends. The consistent loss is wakeup latency with many message threads, where a woken task waits behind a running one: `keep_prev` lets prev continue while it is ahead in fair order, for up to 20 ms.

**Change** (`LERTICK_AI_KEEP=1`): when prev's slice ends and its current burst has a trained prediction, keep prev if it is predicted to sleep within 0.5 ms, and yield to a queued task if it is predicted to run more than 2 ms longer. Otherwise the fair rule decides. CPU-bound tasks have no valid burst and fixed mode has no prediction, so both are unchanged; only the learned mode can change.

**Screening** (`ek-run.sh`, starts when the evening round ends): held-out on 4 and 8 CPUs, 3 repetitions, arms EEVDF, `base` online, `base` fixed and `ek` online, built from the same source. `ek` goes on if schbench `-m 4 -t 2` wakeup p99 is lower than `base` online by more than 10% on both CPU counts and no other measure is worse than `base` online by more than 10%. The `base` online and fixed arms also measure what the learned slice adds today.

### Screening result: learned continuation rejected, 2026-11-16

`ek-run.sh` ran 16:47 to 17:06 host time with no fallbacks; raw sets in `ek-sets.tar.gz`. Change against `base` online (positive is better for `ek`):

| Test | 4 CPUs | 8 CPUs |
|---|---|---|
| schbench -m4 -t2 wakeup p99 (target) | +2 | -16 |
| schbench -m4 -t2 request p99 | -2 | -5 |
| schbench -m1 -t1 wakeup p99 | -15 | +9 |
| hackbench msg processes | +12 | +6 |
| hackbench 16 groups | +12 | -1 |
| hackbench pipe threads | -9 | -3 |
| perf bench sched pipe | +1 | -7 |

The target moved by +2% and -16%, not by more than 10% on both CPU counts, and schbench -m1 -t1 wakeup p99 was 15% worse on 4 CPUs. `ek` is rejected and the `LERTICK_AI_KEEP` switch is removed from the source; `BPF_DEFS` stays in the Makefile for later screening builds.

What the learned slice adds today (`base` online against `base` fixed, same session): on 4 CPUs perf pipe +24% but schbench -m4 -t2 wakeup p99 -18%, hackbench msg processes -12% and 16 groups -10%; on 8 CPUs schbench -m1 -t1 wakeup p99 +18% and hackbench pipe threads +6%, the rest within 3%. The signs disagree between CPU counts and with earlier sessions, so with 3 repetitions this is still noise rather than a measured gain or loss from learning.

## Pooled comparison with every scheduler, 2026-11-16

`pool_all.py` adds the evening session to the pool and pools the other schedulers the same way: per session, Lertick's median against the other scheduler's median from the same session (fallback repetitions dropped, kernel compile from `kbuild-long`), then the median over sessions. EEVDF has 7 sessions (6 for kernel compile); scx_bpfland, scx_lavd, scx_p2dq and scx_flash have 2 each (the first bpfland or lavd or p2dq/flash session, and the evening session). The README charts use these numbers (`make_chart.py`).

Better/same/worse at 5%, 4 CPUs then 8 CPUs: EEVDF 3/6/2 and 3/5/3, scx_bpfland 7/3/1 and 8/2/1, scx_lavd 7/3/1 and 5/2/4, scx_p2dq 7/2/2 and 3/6/2, scx_flash 7/2/2 and 6/5/0. Against EEVDF the losses that remain are schbench -m 4 -t 2 (request p99 2.4 times higher and wakeup p99 47% higher on 4 CPUs, wakeup p99 31% higher on 8 CPUs) and, on 8 CPUs, perf messaging processes (6%) and schbench -m 1 -t 1 request p99 (10%).

## One queue per last-level cache (`llc`), hypothesis fixed 2026-11-16 before the run

**Why.** Every CPU takes work from one shared queue, so its lock is taken on every enqueue and dispatch, and a vCPU that the host pauses while holding it stalls the rest. The consistent loss, schbench `-m 4 -t 2` wakeup and request p99, is the test with the most wakeups per second across CPUs. Most sched_ext schedulers (scx_bpfland, scx_lavd, scx_p2dq) keep a queue per cache domain, and real machines with several LLCs need one anyway.

**Change.** One vtime-ordered queue per LLC domain, read from `/sys/devices/system/cpu/cpu*/cache` by the loader; all queues keep one virtual clock. A task is queued in the domain of its CPU; a CPU takes from its own domain and steals the head of another when that is empty; `keep_prev` also compares prev with the head of the other domains. With one domain the behaviour is the previous one.

**VM limit.** The test VM reports every cache, L1 included, as shared by all 8 CPUs, so the real topology gives one domain there and no difference. Screening therefore uses builds with `-DLERTICK_DOMAIN_SPLIT=N`, which splits CPUs into consecutive groups of N: N=2 for the 4-CPU run (two domains on CPUs 0-3) and N=4 for the 8-CPU run (two domains).

**Screening** (`llc-run.sh`): held-out on 4 and 8 CPUs, 3 repetitions, arms EEVDF, `base` (the current build), `one` (the new code with the VM's one domain) and `split`. `split` goes on to the full confirmation if schbench `-m 4 -t 2` wakeup p99 is lower than `base` by more than 10% on both CPU counts and no other measure is worse than `base` by more than 10%. `one` must stay within 10% of `base` on every measure, since it should behave the same.

### Screening result: forced split rejected, 2026-11-16

`llc-run.sh` ran 01:51 to 02:10 UTC with no fallbacks; raw sets in `llc-sets.tar.gz`. Against `base` (positive is better):

| Test | `split` 4 CPUs | `split` 8 CPUs | `one` 4 CPUs | `one` 8 CPUs |
|---|---|---|---|---|
| schbench -m4 -t2 wakeup p99 (target) | -31 | -11 | +0 | -33 |
| schbench -m4 -t2 request p99 | +16 | -1 | -1 | -5 |
| schbench -m1 -t1 wakeup p99 | -2 | +34 | +5 | +34 |
| perf messaging threads | -14 | +5 | +4 | -29 |
| perf pipe | +50 | +51 | +51 | -2 |
| hackbench pipe threads | +23 | -1 | +18 | +2 |
| kernel compile | -9 | -0 | -3 | -1 |

Splitting the queue made the target worse on both CPU counts, so two-way queues do not help this test here; the loss is not contention on the shared queue. `split` is rejected and the `LERTICK_DOMAIN_SPLIT` screening switch is removed.

`one` schedules exactly as `base` on this VM (one domain), yet differs by up to 33% on single measures in 3 repetitions, in both directions; `base` also measured perf pipe 50% slower than usual in this session. That is the session noise already seen between identical builds. The criterion for `one` (within 10% everywhere) is not met at 3 repetitions, so the final build is confirmed separately below before it replaces `base`.

## Shared policy header and the scx export, 2026-11-16

The policy (queues per LLC, continuation, affine pick, sync wakeup, cgroup weights) moved from `scx_lertick.bpf.c` into `src/bpf/policy.h`, which the product and `upstream/scx_lertick` both include. `scripts/export_scx.py` copies it and `src/shared/model.h` (without the product-only regions marked `LERTICK_UPSTREAM`) into the port; CI runs it with `--check`. Built in the VM: the product (`pol`) attaches and runs hackbench; the port builds in the scx 1.1.4 workspace, passes `cargo fmt --check`, attaches and reports `8 CPUs, 1 queue(s)`.

**Confirmation** (`pol-run.sh`): held-out on 4 and 8 CPUs, 5 repetitions, arms EEVDF, `base` and `pol`. `pol` replaces `base` if no measure is worse than `base` by more than 10% on either CPU count.

### Confirmation result: shared-policy build accepted, 2026-11-16

`pol-run.sh` ran 02:12 to 02:35 UTC with no fallbacks; raw sets in `pol-sets.tar.gz`. `pol` against `base` (positive is better), 5 repetitions:

- 4 CPUs: perf messaging processes -8, threads +3, perf pipe -5, schbench -m4 -t2 wakeup p99 +0, request p99 +3, RPS -1, -m1 -t1 wakeup p99 +9, request p99 -1, hackbench threads -0, 16 groups -1, kernel compile -3.
- 8 CPUs: perf messaging processes +1, threads +7, perf pipe +40, schbench -m4 -t2 wakeup p99 +16, request p99 -5, RPS -0, -m1 -t1 wakeup and request p99 +0, hackbench threads +1, 16 groups -4, kernel compile -1.

No measure is more than 10% worse on either CPU count, so the shared-policy build with one queue per LLC replaces `base`. After the screening switch was removed, the final source was rebuilt in the VM: the product attaches in online and fixed mode and runs hackbench, and the port builds in the scx 1.1.4 workspace, passes `cargo fmt --check` and does the same with and without `--fixed`.

## Baseline of the scx submission build, 2026-11-17

From here on the scx port (`Denny-Lin/scx` branch `scx_lertick`, built on scx main 48e2a9c) is the build under test. `scx-base-run.sh` ran it against EEVDF with the product build (`prod`, online mode, `--track-all`) in the same sessions: held-out and `kbuild-long.sh` on 4 and 8 CPUs, 5 repetitions, 04:42 to 05:23 UTC. No fallbacks; guest steal time 0. Raw sets in `scx-base-sets.tar.gz`.

`scx` against EEVDF (positive is better):

| Test | 4 CPUs | 8 CPUs |
|---|---|---|
| perf messaging processes | -5 | -3 |
| perf messaging threads | +1 | -10 |
| perf pipe | +91 | +85 |
| hackbench pipe threads | +25 | +12 |
| hackbench 16 groups | +5 | -3 |
| schbench -m1 -t1 request p99 | +0 | -2 |
| schbench -m1 -t1 wakeup p99 | +20 | +26 |
| schbench -m4 -t2 request p99 | -143 | -10 |
| schbench -m4 -t2 wakeup p99 | -49 | +7 |
| schbench -m4 -t2 RPS | +9 | -1 |
| kernel compile | -1 | +0 |

Better/same/worse at 5%: 5/3/3 on 4 CPUs and 4/5/2 on 8 CPUs. `prod` in the same sessions: 4/5/2 and 4/6/1. `scx` against `prod`: within 5% on most measures; perf pipe is slower in `scx` (-22 and -34) though still far ahead of EEVDF, perf messaging threads +15 on 8 CPUs, schbench -m4 -t2 request p99 -14 on 8 CPUs.

Against the submission bar (no measure more than 5% worse than EEVDF, at least 4 wins), the scx build fails on schbench -m4 -t2 request and wakeup p99 on 4 CPUs and, on 8 CPUs, perf messaging threads and -m4 -t2 request p99 by 10%.

## Diagnosis: schbench -m 4 -t 2 on 4 CPUs, 2026-11-17

schbench workers take a per-CPU mutex with a `trylock` spin (`lock_this_cpu()`) around their CPU work. A worker preempted while holding it leaves the next worker on that CPU spinning until its own slice ends. `-L` disables the lock. `lock-run.sh` ran `-m 4 -t 2 -r 20 -w 3` on CPUs 0-3 under EEVDF and the scx build, with and without `-L`, 3 repetitions, no fallbacks; raw sets in `lock-sets.tar.gz`. Medians (us):

| | wakeup p99 | request p99 | RPS |
|---|---|---|---|
| EEVDF, lock | 3756 | 52160 | 188 |
| scx, lock | 5528 | 137984 | 191 |
| EEVDF, no lock | 3788 | 31008 | 382 |
| scx, no lock | 6504 | 29216 | 367 |

Two separate losses:

- **Request p99 is the lock.** Without the lock, scx is 6% better than EEVDF; with it, 2.6 times worse. Lertick lets a spinning worker keep its CPU (a burst past its prediction falls back to the 2 ms fixed slice, and `keep_prev` can extend it to 20 ms) while the lock holder waits in the queue, so a lock handoff can cost several milliseconds. EEVDF's shorter effective slices under contention cut the spin sooner.
- **Wakeup p99 is not the lock.** It is 47 to 72% worse with or without it, so a woken worker waits behind running tasks for another reason, most likely the absence of wakeup preemption: EEVDF lets an eligible wakee with an earlier deadline preempt, while Lertick waits for the running slice to end.

### Migration is the cause, and three candidates, hypothesis fixed 2026-11-17 before the run

`perf stat` over `schbench -m 4 -t 2` on CPUs 0-3: EEVDF makes 1,349 migrations, the scx build 5,908. With workers and message threads pinned (`-M 0-3 -W auto`) the scx build beats EEVDF on both measures (wakeup p99 381 against 947 us, request p99 20,768 against 29,344 us). schbench's lock is per CPU and checked only when taken, so a holder preempted and resumed on another CPU leaves workers on its old CPU spinning until its whole request ends. The shared queue lets any CPU pick up a preempted task.

Candidates, as switches in `policy.h` (all off in the default build):

- `A`, `LERTICK_OVERRUN_YIELD`: a task past its predicted burst gets the minimum slice and no `keep_prev` continuation.
- `B`, `LERTICK_WAKE_PREEMPT`: a woken task that finds no idle CPU preempts its previous CPU or one of two random CPUs if the task there has overrun its prediction.
- `C`, `LERTICK_CPU_REQUEUE`: a task preempted mid-burst waits on a queue of its own CPU; that CPU runs it unless the LLC queue's head is more than 2 ms earlier, and idle CPUs steal from other CPUs' queues when the LLC queues are empty. Woken tasks still go to the LLC queue.

Short load tests (one run each, 4 CPUs): wakeup p99 / request p99 in us were `A` 4,004 / 149,760, `B` 5,112 / 131,328, `AB` 2,996 / 169,728, `C` 4,504 / 54,464, `AC` 2,116 / 115,072, `ABC` 1,998 / 124,800, against about 5,500 / 130,000 for the current build and 3,760 / 50,000 for EEVDF.

**Screening** (`cand-run.sh`): held-out on 4 and 8 CPUs, 3 repetitions, arms EEVDF, `base` (the current scx build), `A`, `C`, `AC` and `ABC`. A candidate goes on if, against `base` on 4 CPUs, schbench `-m 4 -t 2` request p99 or wakeup p99 is lower by more than 10% and neither is higher by more than 10%, and no other measure is worse than `base` by more than 10% on either CPU count.

### Screening result: `A` fixes wakeup, `C` fixes request, not both, 2026-11-17

`cand-run.sh` ran 06:06 to 06:35 UTC with no fallbacks; raw sets in `cand-sets.tar.gz`. schbench `-m 4 -t 2` on 4 CPUs, against `base` and (in brackets) against EEVDF:

| Arm | request p99 | wakeup p99 | perf messaging processes |
|---|---|---|---|
| `base` | 0 (-147) | 0 (-49) | 0 (-4) |
| `A` | -18 (-191) | -11 (-66) | +1 (-3) |
| `C` | +55 (-12) | +5 (-41) | -21 (-26) |
| `AC` | -2 (-152) | +46 (+20) | -15 (-19) |
| `ABC` | -1 (-150) | +64 (+46) | -2 (-5) |

`C` removes most of the request loss and `A` with `C` turns the wakeup loss into a win, but no arm does both: with `A`, a lock holder past its prediction also gets minimum slices, which undoes `C`'s gain. `C` costs perf messaging processes 21% on 4 CPUs and 17% on 8 CPUs, and perf pipe 29 to 53% (still about 90% faster than EEVDF). `ABC` collapses on 8 CPUs (-m4 -t2 wakeup p99 -191%). None passes as screened.

Two likely costs of the CPU queues: an idle CPU is not told about a task queued on another CPU's queue, so messaging bunches up, and every dispatch now iterates two queue heads, which shows on perf pipe's microsecond round trips. Next round: `AC2` (AC with a cheap `scx_bpf_dsq_nr_queued()` check before each head iteration), `ACK` (AC2 plus `LERTICK_CPU_KICK`, which wakes an idle CPU to steal when a task is queued on a CPU's queue) and `CK` (C with both), screened by the same rule (`cand2-run.sh`).

### Round 2 result and round 3, 2026-11-17

`cand2-run.sh` ran 06:37 to 07:01 UTC with no fallbacks; raw sets in `cand2-sets.tar.gz`. Against `base` (EEVDF in brackets), 4 CPUs: `AC2` request p99 -10 (-183), wakeup p99 +48 (+20), messaging processes -6 (-10); `ACK` -18 (-205), +48 (+20), +1 (-4), with perf messaging threads -55 on 8 CPUs; `CK` +51 (-28), +3 (-50), -5 (-10), with messaging processes -27 on 8 CPUs. The idle kick does not cure the messaging loss of `C` and hurts with `A`.

perf messaging runs hundreds of tasks whose bursts last microseconds, while schbench workers run for milliseconds. Round 3 keeps a preempted task on its CPU only once its current burst has run at least 1 ms (`CPU_REQUEUE_MIN_NS`): `C1` (C with the threshold) and `C1K` (with the kick), against `base` and `CK`, by the same rule (`cand3-run.sh`).

### Round 3 result, 2026-11-17

`cand3-run.sh` ran 07:02 to 07:26 UTC with no fallbacks; raw sets in `cand3-sets.tar.gz`. Against `base` (EEVDF in brackets):

| Test | `C1` 4 CPUs | `C1` 8 CPUs | `C1K` 4 | `C1K` 8 | `CK` 4 | `CK` 8 |
|---|---|---|---|---|---|---|
| -m4 -t2 request p99 | +51 (-21) | -2 (+5) | +50 (-22) | -5 (+1) | +50 (-22) | -2 (+4) |
| -m4 -t2 wakeup p99 | +9 (-45) | -2 (+39) | +9 (-45) | -2 (+39) | +9 (-45) | +0 (+40) |
| -m4 -t2 RPS | +13 (+11) | -0 | +13 | +0 | +14 | +0 |
| messaging processes | +12 (-3) | -10 (-9) | +4 | -7 | +8 | -18 |
| messaging threads | -4 | -31 (-22) | -5 | -29 | -2 | -9 |
| perf pipe | +1 | +43 | +4 | -32 | +6 | +53 |
| kernel compile | -6 | -2 | -11 | -1 | -2 | -2 |

All three keep the request gain on 4 CPUs. The 8-CPU messaging results move by 10 to 30 points between arms that differ only in a threshold, and `base` itself moved from -4 to -17 against EEVDF on 4-CPU messaging processes between sessions, so 3 repetitions cannot settle them. `C1` goes to a 5-repetition confirmation against `base` (`c1-run.sh`): it is accepted if -m4 -t2 request p99 on 4 CPUs is lower by more than 10% and no measure is worse than `base` by more than 10% on either CPU count.

### Confirmation result: `C1` accepted, 2026-11-17

`c1-run.sh` (held-out, 5 repetitions, 07:27 to 07:51 UTC) and `c1-kb-run.sh` (`kbuild-long.sh`, 5 repetitions, to 08:06 UTC) ran with no fallbacks; raw sets in `c1-sets.tar.gz`. `C1` against `base` (EEVDF in brackets):

- 4 CPUs: schbench -m4 -t2 request p99 +52 (-25, from -162 for `base`), RPS +13 (+20), wakeup p99 +2 (-52); perf messaging processes -9 (-7), threads -7 (-5); hackbench, schbench -m1 -t1 and kernel compile within 2.
- 8 CPUs: perf messaging processes -8 (-8), -m4 -t2 wakeup p99 -10 (+18), the rest within 8.
- Kernel compile (`kbuild-long`): 26.32 s against 26.72 s for `base` and 26.35 s for EEVDF on 4 CPUs; 19.87 s against 19.55 s and 19.57 s on 8 CPUs.
- perf pipe reads -71 on 4 CPUs, but per repetition both arms take the same values (1.1, 2.0, 2.5 and 4.2 us per round trip, against 20 us for EEVDF); only the mix of modes differs, so this is not a change.

No measure is more than 10% worse than `base`, so the CPU requeue with the 1 ms threshold becomes the default code and the next baseline (`b1`). The rejected switches (`LERTICK_WAKE_PREEMPT`, `LERTICK_CPU_KICK`) are removed; `LERTICK_OVERRUN_SLICE_NS` and `LERTICK_OVERRUN_NOKEEP` stay off for the wakeup-latency screening that follows. The final source was rebuilt: the scx build and the product attach in online and fixed mode and run hackbench, schbench and perf messaging; the port passes `cargo fmt --check`, clang-format with the scx style and clippy.
