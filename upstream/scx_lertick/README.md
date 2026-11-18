# scx_lertick

This is a single user-defined scheduler used within [`sched_ext`](https://github.com/sched-ext/scx/tree/main), which is a Linux kernel feature which enables implementing kernel thread schedulers in BPF and dynamically loading them. [Read more about `sched_ext`](https://github.com/sched-ext/scx/tree/main).

## Overview

A weighted vtime scheduler in which each task learns its own time slice.

Each task trains a small fixed-point linear model inside BPF on its own CPU
bursts, from wakeup to sleep. The inputs are the last burst, an average of
recent bursts, the last sleep time and the queue depth. At the next wakeup the
prediction sets the task's slice, between 0.5 and 3 ms by default. There is no
model file and no inference in user space, and `--fixed` gives every task the
same slice instead.

The prediction only sets the slice. Tasks wait in one queue per last-level
cache, ordered by weighted virtual time, and are charged the CPU time they
actually used, so `nice` values and cgroup `cpu.weight` set each task's share.
An idle CPU takes work from another cache's queue. All queues share one virtual
clock, and how far a task may sit ahead of it or behind it is bounded, so no
task waits long for the clock to catch up.

A CPU prefers a queued task that last ran on it when that task is near the head
of the queue, so a preempted task usually resumes where its cache is.

## Typical Use Case

Workloads where tasks alternate short CPU bursts with sleeps, such as message
passing and pipelines.

## Production Ready?

No, it is experimental. It has only run in an ARM64 virtual machine, not on
physical hardware, SMT or hybrid CPUs, or machines with more than one last-level
cache. Threaded cgroups are not weighted specially.
