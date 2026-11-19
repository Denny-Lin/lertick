# Learned slices and queue order

## Now (2026-11-19)

| | |
|---|---|
| Build | The scx port orders each LLC's queue by vtime plus half the predicted slice ([deadline order](RECORD.md#deadline-order-2026-11-18)). Same code as sched-ext/scx#3994 at commit `45d1fda`. |
| Against EEVDF | Median of 2 sessions, 8 runs each, better/same/worse: 8/2/1 on 4 CPUs, 4/6/1 on 8 CPUs. |
| Against sched_ext schedulers | Previous build, one session: scx_bpfland 9/2/0 and 8/3/0, scx_lavd 8/2/1 and 4/6/1, scx_p2dq 7/2/2 and 5/6/0, scx_flash 8/2/1 and 8/3/0 ([table](RECORD.md#same-session-comparison-2026-11-18)). |
| Open | perf messaging with processes, 6 to 8% behind EEVDF. The lag bounds act on under 0.1% of enqueues there, so they are not the cause ([lag bounds](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#how-often-the-lag-bounds-act-2026-11-18), [what it is not](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#what-the-8-cpu-perf-messaging-loss-is-not-2026-11-18)). |
| Untested | Bare metal, SMT, hybrid cores, NUMA, more than one LLC, CPU hotplug. All results are from one ARM64 VM. |

## Ledger, newest first

| Date | Change | Verdict | Record |
|---|---|---|---|
| 11-18 | Queue ordered by vtime plus half the slice | Kept: 4-CPU schbench -m4 -t2 wakeup p99 19% below EEVDF | [deadline order](RECORD.md#deadline-order-2026-11-18), [screen](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#deadline-ordering-screen-2026-11-18) |
| 11-18 | Wakeup preemption chosen by the model | Rejected: wakeup and request p99 up to 47% and 83% above the build without it | [round 2](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#round-2-wakeup-preemption-chosen-by-the-model-rejected) |
| 11-18 | Shorter slices for bursts past the maximum, narrower queue rules | Rejected: the wakeup p99 did not drop | [round 1](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#losses-after-the-waker-model-4-cpu-schbench-wakeup-p99-round-1-2026-11-18), [round 3](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#round-3-4-cpus-narrower-queue-rules-rejected) |
| 11-18 | Wakee placement from a learned "waker sleeps soon" | Rejected: not predictable in perf messaging, slower in schbench | [placement](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#what-the-8-cpu-perf-messaging-loss-is-not-2026-11-18) |
| 11-18 | Comparison with four sched_ext schedulers in one session | Measurement | [table](RECORD.md#same-session-comparison-2026-11-18) |
| 11-18 | Burst guess from the same waker, per-program model, trust after 2 bursts | Kept: schbench prediction error cut to a fifth | [waker model](RECORD.md#release-candidate-waker-model-2026-11-18) |
| 11-18 | Long bursts kept while only long tasks wait | Rejected: wakeup p99 up by half | [G](RECORD.md#long-bursts-kept-while-only-long-tasks-wait-g-2026-11-18-rejected) |
| 11-17 | Bounds on lag behind and ahead of the clock, root-cgroup weights | Kept: three stall causes fixed | [stalls](RECORD.md#two-more-stalls-two-more-bounds-and-the-scx-ci-test-2026-11-17), [clamp](RECORD.md#clamp-result-cap-accepted-2026-11-17) |
| 11-17 | Learned slices capped at 3 ms | Kept | [numbers](RECORD.md#readme-numbers-for-the-3-ms-slice-cap-2026-11-17) |
| 11-16 | Dispatch ordered by predicted burst | Rejected | [results](RECORD.md#results-2026-11-16) |

## How results are measured

- One QEMU/HVF virtual machine on an Apple silicon Mac: ARM64, Linux 7.0, 8 vCPUs, one LLC, no SMT. 4-CPU runs use `taskset`.
- `heldout.sh` runs 11 measures: perf bench sched messaging (processes, threads) and pipe, hackbench (threads, 16 groups), schbench -m 1 -t 1 and -m 4 -t 2 (request and wakeup p99, requests per second) and a kernel/sched rebuild. They were first run as a held-out check on 2026-11-16; later rounds also used them to choose between variants, such as the slice cap and the deadline share.
- Arms run in a shuffled order in each repetition, 8 repetitions in recent sessions; the tables give medians. Better or worse means more than 5% apart. EEVDF is the kernel's own scheduler, with sched_ext disabled.
- A change is kept only after the safety checks: nice and cgroup weights (`gate.sh`, `cgp.sh`), the sched-ext/scx CI stress test and edge cases (`edge-run.sh`), a stress battery (`stress-run.sh`) and a one-hour mixed long run (`long-run.sh`).

## Where to find

| Need | Where |
|---|---|
| Every hypothesis, result and rejected variant, oldest first | [RECORD.md](RECORD.md) |
| The switches behind the experiments of 2026-11-18, and their write-ups | branch [`learn-ab`](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md) |
| Screening rounds before the held-out tests | [SCREENING.md](SCREENING.md) |
| Scripts and raw data | this folder: `*-run.sh` with the `*-sets.tar.gz` they produced |
