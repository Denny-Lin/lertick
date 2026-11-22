# Learned slices and queue order

This folder holds the experiments behind the current scheduler: the time slice
learned per task, the queue ordered by vtime plus half that slice, sync
wakees queued where idle CPUs can take them, wakeup preemption and the choice
of queue learned per program, and the variants that were tried and dropped.

## Status

On 8 CPUs the scx port (`upstream/scx_lertick`) is better than EEVDF on 7 of
the 12 benchmarks, the same on 4 and worse on 1. On 4 CPUs it is better on 5, the same on 3 and worse on 4.
These are medians of 6 runs in one session
([numbers](RECORD.md#learned-wakeup-preemption-and-queue-choice-2026-11-22)).

The losses: stress-ng futex at 62% (8 CPUs) and 71% (4 CPUs) of EEVDF's
throughput, up from 0.2% before wakeup preemption; and on 4 CPUs, schbench
-m 4 -t 2 with 7% fewer requests per second and request and wakeup p99 7%
and 9% higher. The schbench loss comes with preemption itself. The product in
`src/` does not have the learned preemption or queue choice.

scx_bpfland, scx_lavd, scx_p2dq and scx_flash were compared with an earlier
version only, in one session
([table](RECORD.md#same-session-comparison-2026-11-18)).

All of this ran in one ARM64 VM. Bare metal, SMT, hybrid cores, NUMA, more
than one LLC and CPU hotplug are untested.

## Changes tried

Newest first. The kept changes are in the current code.

- 2026-11-22, kept: a woken task preempts the running one as learned per
  program, and woken and preempted tasks wait on their CPU's queue or the
  LLC queue as learned per program, within fairness rules
  ([record](RECORD.md#learned-wakeup-preemption-and-queue-choice-2026-11-22)).
- 2026-11-19 to 11-22, dropped: fixed preemption rules, a fixed rule for
  preempted tasks, a learned number of LLC queues, taking several tasks per
  lock, a wider tolerance for tasks ahead of the clock, and kicking the CPU
  on every CPU-queue insert (same record).
- 2026-11-19, kept: a running task moves off a core whose other thread is
  busy to a whole idle core, and whether a running task is worth moving
  follows its predicted burst. Checked only with SMT siblings declared on
  the VM ([record](RECORD.md#smt-and-moving-running-tasks-by-their-prediction-2026-11-19)).
- 2026-11-19, kept: handling of CPUs of unequal capacity (big and little
  cores): bursts learned as work, long bursts placed on idle big CPUs, and
  long runners handed from little to idle big CPUs. Checked only with
  capacities declared on the VM's alike CPUs
  ([record](RECORD.md#cpus-of-unequal-capacity-2026-11-19)).
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
