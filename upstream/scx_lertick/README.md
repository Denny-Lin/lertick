# scx_lertick

This is a single user-defined scheduler used within [`sched_ext`](https://github.com/sched-ext/scx/tree/main), which is a Linux kernel feature which enables implementing kernel thread schedulers in BPF and dynamically loading them. [Read more about `sched_ext`](https://github.com/sched-ext/scx/tree/main).

## Overview

`scx_lertick`: a weighted virtual-time `sched_ext` scheduler that learns each
task's time slice in BPF.

Tasks wait in one queue per last-level cache, ordered by weighted virtual time,
and an idle CPU steals from another cache's queue. All queues share one virtual
clock, which advances with the CPU time actually delivered over the total
runnable weight, so tasks that keep waking cannot hold back tasks that have
already run. A woken task is placed at most 2 ms behind the clock, and a queued
task at most 20 ms (scaled by its weight) ahead of it, so no task waits long for
the clock to catch up.

Each task trains a small linear model, in fixed-point arithmetic inside BPF, on
its own completed CPU bursts (from wakeup to sleep). The features are the last
burst, an average of recent bursts, the sleep time and the queue depth. At the
next wakeup the prediction sets the task's time slice, between 0.5 ms and 3 ms.
Fairness always charges the CPU time actually used, never the prediction. There
is no offline training, model file or user-space inference.

A CPU prefers a queued task that last ran on it when that task is among the
first 16 in the queue and within 8 ms of its head, so a preempted task usually
resumes where its cache, and any lock it holds, is. A synchronous wakeup can
queue on the waker's CPU. `nice`
values and cgroup `cpu.weight` set each task's share.

## Typical Use Case

Mixed desktop and server workloads where tasks alternate short CPU bursts with
sleeps, such as message passing and pipelines.

## Production Ready?

No. It is experimental. It has been tested in a QEMU/HVF ARM64 virtual machine
with 4 and 8 CPUs on Linux 7.0, but not on physical hardware or large machines.
That VM reports a single last-level cache, so the per-cache queues have only
been exercised with CPUs split into groups artificially.

cgroup weights assume the cgroup v2 rule that only the root cgroup holds both
tasks and child cgroups. Threaded cgroups, which may also do so, are not handled
specially and can skew weights between their own tasks and their children.
