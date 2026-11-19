# scx_lertick

This is a single user-defined scheduler used within [`sched_ext`](https://github.com/sched-ext/scx/tree/main), which is a Linux kernel feature which enables implementing kernel thread schedulers in BPF and dynamically loading them. [Read more about `sched_ext`](https://github.com/sched-ext/scx/tree/main).

## Overview

A weighted vtime scheduler in which each task learns its own time slice.

Each task trains a small fixed-point linear model inside BPF on its own CPU
bursts, from wakeup to sleep. The inputs are a burst guess, an average of
recent bursts, the last sleep time and the queue depth. The guess is the last
burst the same waker started, while that has recently been closer than the
average: a thread that does two kinds of work, each started by a different
waker, gets the right length for each. At the next wakeup the prediction sets
the task's slice, between 0.5 and 3 ms by default; a burst predicted to run
past 3 ms is cut anyway and gets the fixed 2 ms. A new process starts from
the model that earlier processes with the same name trained, so short-lived
processes such as a compiler's predict from their first burst. There is no
model file and no inference in user space, and `--fixed` gives every task the
same slice instead.

Tasks wait in one queue per LLC, ordered by a virtual deadline: the weighted
virtual time plus half the predicted slice, so a task about to run briefly goes
ahead of longer ones about as far behind, without preempting anyone. Tasks are
charged the CPU time they actually used, so `nice` values and cgroup
`cpu.weight` set each task's share. An idle CPU steals from another LLC's queue.
All queues share one virtual clock, and how far a task may sit ahead of it or
behind it is bounded, so no task waits long for the clock to catch up.

A task woken with the sync hint waits in a queue of the waker's CPU, which
takes from it before the LLC queue. A CPU with nothing else to run takes
from the longest of these queues, so the tasks a waker wakes at once do not
all wait on its CPU while other CPUs idle.

A CPU prefers a queued task that last ran on it when that task is near the head
of the queue, so a preempted task usually resumes where its cache is.

On CPUs of unequal capacity, as with big and little cores, bursts are learned
as work, their time scaled by the capacity of the CPU they ran on, so a burst
learned on a little CPU predicts the right slice on a big one. A woken task
whose bursts run long takes an idle big CPU when one is free, and a task that
keeps running on a little CPU is handed to an idle big one; `--no-asym-rotate`
turns that hand-over off. `--cpu-capacity` overrides the capacities, to try
this on CPUs that are alike.

## Typical Use Case

Workloads where tasks alternate short CPU bursts with sleeps, such as message
passing and pipelines.

## Production Ready?

No, it is experimental. It has only run in an ARM64 virtual machine, not on
physical hardware, SMT or hybrid CPUs, or machines with more than one LLC; the
handling of big and little cores was tested only with `--cpu-capacity`. It
does not weigh energy, so on battery it may move long-running tasks to big
cores where an energy-aware scheduler would not, and it picks a big core
without regard to its LLC or a busy SMT sibling. Threaded cgroups are not
weighted specially.
