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
