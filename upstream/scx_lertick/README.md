# scx_lertick

This is a single user-defined scheduler used within [`sched_ext`](https://github.com/sched-ext/scx/tree/main), which is a Linux kernel feature which enables implementing kernel thread schedulers in BPF and dynamically loading them. [Read more about `sched_ext`](https://github.com/sched-ext/scx/tree/main).

## Overview

A weighted vtime scheduler that learns each task's time slice, and for each
program whether its woken tasks preempt and which queue its tasks wait on.

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
virtual time plus half the slice the task is given, in virtual time, so a task
about to run briefly goes ahead of longer ones about as far behind.
Virtual-time accounting charges each task for the CPU time it actually uses, so
`nice` values and cgroup `cpu.weight` set each task's share. An idle CPU steals
from another LLC's queue. All queues share one virtual clock, and how far a
task may sit ahead of it or behind it is bounded, so no task waits long for the
clock to catch up.

Each CPU also has a queue of its own. When no CPU is idle, a task woken with
the sync hint can wait there on the waker's CPU. A CPU with nothing else to run
takes from the longest of these queues, so the tasks a waker wakes at once do
not all wait on its CPU while other CPUs idle. A CPU tries its own queue before
the LLC queue while its own head deadline is at most 2 ms after the LLC
queue's.

A woken task whose virtual time is before the running task's virtual time plus
its uncharged run and a fixed slice, both scaled by its weight, may preempt it,
much as EEVDF checks a wakeup. Whether it does is learned for its program. A
wakeup with the sync hint, as seen when its CPU is picked, does not preempt.
Each program also learns whether its woken and preempted tasks wait on their
CPU's queue, which spares the LLC queue's lock, or on the LLC queue. Of these,
a task that has had more than its share always waits on the LLC queue.

A CPU prefers a queued task that last ran on it when that task is near the head
of the queue, so a preempted task usually resumes where its cache is.

On CPUs of unequal capacity, as with big and little cores, bursts are learned
as work, their time scaled by the capacity of the CPU they ran on, and a slice
is that work scaled back by the capacity of the CPU the task last ran on. A
woken task whose bursts run long takes an idle big CPU when one is free, and a
task that keeps running on a little CPU is handed to an idle big one;
`--no-asym-rotate` turns that hand-over off. `--cpu-capacity` overrides the
capacities, to try this on CPUs that are alike.

On cores with two hardware threads, a task that keeps running while the
other thread of its core is busy moves to a whole idle core, as the kernel's
load balancing spreads busy threads over cores; `--no-smt-balance` turns
this off and `--smt-siblings` declares sibling pairs for testing. Whether a
running task is worth moving at all follows its predicted burst: one about
to finish stays rather than finish on a cold cache (`--no-pred-move` uses
the fixed slice instead).

## Typical Use Case

Workloads where tasks alternate short CPU bursts with sleeps, such as message
passing and pipelines.

## Production Ready?

No, it is experimental. It has only run in an ARM64 virtual machine, not on
physical hardware, SMT or hybrid CPUs, or machines with more than one LLC; the
handling of big and little cores was tested only with `--cpu-capacity`, and
that of SMT only with `--smt-siblings`. It does not weigh energy, so on battery
it may move long-running tasks to big cores where an energy-aware scheduler
would not, and it picks a big core without regard to its LLC. With more than
two threads per core, only one sibling is checked. Threaded cgroups are not
weighted specially.

stress-ng futex reaches about 60 to 70% of its throughput under EEVDF. With 4
CPUs fully loaded, schbench -m 4 -t 2 serves about 7% fewer requests per
second, and its request and wakeup p99 are 7% and 9% higher. The learned
choices take a table of 128 KiB per possible CPU.
