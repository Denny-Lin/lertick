# Learned slices and queue order

This folder holds the experiments behind the current scheduler: the time slice
learned per task, the queue ordered by vtime plus half that slice, sync
wakees queued where idle CPUs can take them, and the variants that were tried
and dropped. The scx copy at sched-ext/scx#3994 commit `45d1fda` does not yet
have the CPU queues.

## Status

On 8 CPUs Lertick is better than EEVDF on 8 of the 11 benchmarks, the same on
3 and worse on none. On 4 CPUs it is better on 7, the same on 3 and worse on
1. These are medians of 6 runs in one session
([numbers](RECORD.md#stealable-cpu-queues-2026-11-19)).

The 4-CPU loss is schbench -m 4 -t 2 request p99, 10% above EEVDF. The
previous release measured the same in that session, and earlier sessions put
it 1 to 4% above EEVDF, so it is not from the latest change.

scx_bpfland, scx_lavd, scx_p2dq and scx_flash were compared with an earlier
version only, in one session
([table](RECORD.md#same-session-comparison-2026-11-18)).

All of this ran in one ARM64 VM. Bare metal, SMT, hybrid cores, NUMA, more
than one LLC and CPU hotplug are untested.

## Changes tried

Newest first. The kept changes are in the current code.

- 2026-11-19, kept: queue sync wakees on a queue of the waker's CPU that
  idle CPUs can take from, and count statistics per CPU. perf messaging with
  processes went from 14% behind EEVDF to 15% ahead on 8 CPUs
  ([record](RECORD.md#stealable-cpu-queues-2026-11-19),
  [experiments](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#sync-wakees-stacked-on-the-wakers-cpu-2026-11-19)).
- 2026-11-19, dropped: refusing sync placements past a queue length, and
  placing wakees by predicted wait. The cap worked but is a tuned number;
  the predicted wait was 2.3 times slower than EEVDF on 8-CPU hackbench
  threads (same links).
- 2026-11-18, kept: order the queue by vtime plus half the predicted slice.
  The 4-CPU schbench -m 4 -t 2 wakeup p99 is now 19% below EEVDF's
  ([record](RECORD.md#deadline-order-2026-11-18),
  [screen](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#deadline-ordering-screen-2026-11-18)).
- 2026-11-18, dropped: wakeup preemption chosen by the model. Wakeup and
  request p99 rose by up to 47% and 83% over the build without it
  ([round 2](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#round-2-wakeup-preemption-chosen-by-the-model-rejected)).
- 2026-11-18, dropped: shorter slices for bursts past the maximum, and
  narrower queue rules. The wakeup p99 did not drop
  ([round 1](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#losses-after-the-waker-model-4-cpu-schbench-wakeup-p99-round-1-2026-11-18),
  [round 3](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#round-3-4-cpus-narrower-queue-rules-rejected)).
- 2026-11-18, dropped: placing the wakee by a learned guess of whether the
  waker sleeps soon. In perf messaging the guess was no better than a coin,
  and the schbench wakeup p99 rose
  ([notes](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md#what-the-8-cpu-perf-messaging-loss-is-not-2026-11-18)).
- 2026-11-18: one session with four sched_ext schedulers
  ([table](RECORD.md#same-session-comparison-2026-11-18)).
- 2026-11-18, kept: guess the next burst from the last one the same waker
  started, share a model per program name, and trust a model after 2 bursts.
  The schbench prediction error fell to a fifth
  ([record](RECORD.md#release-candidate-waker-model-2026-11-18)).
- 2026-11-18, dropped: keep long bursts running while only long tasks wait.
  The wakeup p99 rose by half
  ([record](RECORD.md#long-bursts-kept-while-only-long-tasks-wait-g-2026-11-18-rejected)).
- 2026-11-17, kept: bound how far a task's vtime may fall behind or run ahead
  of the clock, and stop counting root-cgroup tasks twice in the weights. This
  fixed three causes of stalls
  ([stalls](RECORD.md#two-more-stalls-two-more-bounds-and-the-scx-ci-test-2026-11-17),
  [clamp](RECORD.md#clamp-result-cap-accepted-2026-11-17)).
- 2026-11-17, kept: cap learned slices at 3 ms
  ([numbers](RECORD.md#readme-numbers-for-the-3-ms-slice-cap-2026-11-17)).
- 2026-11-16, dropped: dispatch in order of predicted burst
  ([results](RECORD.md#results-2026-11-16)).

## How results are measured

- One QEMU/HVF virtual machine on an Apple silicon Mac: ARM64, Linux 7.0, 8
  vCPUs, one LLC, no SMT. 4-CPU runs use `taskset`.
- `heldout.sh` runs 11 measures: perf bench sched messaging (processes,
  threads) and pipe, hackbench (threads, 16 groups), schbench -m 1 -t 1 and
  -m 4 -t 2 (request and wakeup p99, requests per second) and a kernel/sched
  rebuild. They were first run as a held-out check on 2026-11-16. Later
  rounds also used them to choose between variants, such as the slice cap and
  the deadline share.
- Arms run in a shuffled order in each repetition, 8 repetitions in recent
  sessions, and the tables give medians. Better or worse means more than 5%
  apart. EEVDF is the kernel's own scheduler, with sched_ext disabled.
- A change is kept only after the safety checks: nice and cgroup weights
  (`gate.sh`, `cgp.sh`), the sched-ext/scx CI stress test and edge cases
  (`edge-run.sh`), a stress battery (`stress-run.sh`) and a one-hour mixed
  long run (`long-run.sh`).

## Files

- [RECORD.md](RECORD.md): every hypothesis, result and dropped variant,
  oldest first.
- Branch
  [`learn-ab`](https://github.com/Denny-Lin/lertick/blob/learn-ab/results/deadline-ordering/README.md):
  the switches behind the 2026-11-18 experiments, and their write-ups.
- [SCREENING.md](SCREENING.md): screening rounds before the held-out tests.
- In this folder, the `*-run.sh` scripts and the `*-sets.tar.gz` raw data
  they produced.
