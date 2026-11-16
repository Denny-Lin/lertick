# scx_lertick

This is a single user-defined scheduler used within [`sched_ext`](https://github.com/sched-ext/scx/tree/main), which is a Linux kernel feature which enables implementing kernel thread schedulers in BPF and dynamically loading them. [Read more about `sched_ext`](https://github.com/sched-ext/scx/tree/main).

## Overview

`scx_lertick`: a weighted virtual-time `sched_ext` scheduler that learns each
task's time slice in BPF.

All tasks share one queue ordered by weighted virtual time. The virtual clock
advances with the CPU time actually delivered over the total runnable weight,
so tasks that keep waking cannot hold back tasks that have already run. A woken
task is placed at most 2 ms behind the clock.

Each task trains a small linear model, in fixed-point arithmetic inside BPF, on
its own completed CPU bursts (from wakeup to sleep). The features are the last
burst, an average of recent bursts, the sleep time and the queue depth. At the
next wakeup the prediction sets the task's time slice, between 0.5 ms and 5 ms.
Fairness always charges the CPU time actually used, never the prediction. There
is no offline training, model file or user-space inference.

A CPU prefers a queued task that last ran on it when that task is within 2 ms of
the queue head, and a synchronous wakeup can queue on the waker's CPU. `nice`
values and cgroup `cpu.weight` set each task's share.

## Typical Use Case

Mixed desktop and server workloads where tasks alternate short CPU bursts with
sleeps, such as message passing and pipelines.

## Production Ready?

No. It is experimental. It has been tested in a QEMU/HVF ARM64 virtual machine
with 4 and 8 CPUs on Linux 7.0, but not on physical hardware or large machines.
The single shared queue is not expected to scale to many CPUs.
