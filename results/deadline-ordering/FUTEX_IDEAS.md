# Learning the futex handoff: 100 ideas

The problem (see README.md, "Why futex is slow"): a task woken onto a CPU
whose running task keeps running waits up to that task's slice. EEVDF lets
the wakee preempt at once; stress-ng futex then runs 500 times faster.
Always preempting (`WAKER_PREEMPT` 3) fixes futex but costs schbench request
p99 and the 4-CPU kernel compile. The question for every idea below is the
same: when a wakee could preempt, or could be moved, should it, and can the
scheduler learn the answer instead of using a fixed rule?

Each idea names what is learned, from which signal, and where the decision
is made. The ideas marked **tested** have a switch on this branch and a
result in README.md.

## A. Learn the cost of each choice directly

1. **tested (`WP_LEARN` 1)** Per pair of programs (wakee, running task),
   average the delay each choice caused: the wakee's wait when queued, the
   preempted task's wait when preempting. Take the cheaper one, explore 1 in
   16.
2. **tested (`WP_LEARN` 2)** The same, per wakee program.
3. **tested (`WP_LEARN` 3)** The same, per wakee task.
4. Per running-task program only: some programs (lock holders) are costly
   to preempt whoever wakes.
5. Weight each delay by the task's weight, so nice levels count.
6. Count the delay of every task queued behind as well, not only the two.
7. Score the preempted task's delay minus the wakee's run, the part caused
   by the preemption itself.
8. Use the median instead of the mean, so one long outlier does not flip
   the choice.
9. Thompson sampling over the two delays instead of a fixed explore rate.
10. Upper confidence bound on the delays, exploring the less tried choice.
11. Decaying explore rate: explore more when a cell is young.
12. Reset a cell when its program changes behavior (the delay jumps).
13. Learn per (wakee program, CPU count in the domain), since the right
    answer changes with load.
14. Learn per (wakee program, queue length bucket).
15. Score with throughput: bursts completed per ms after the choice.
16. Score with the wakee's next wakeup-to-run latency, the user-visible
    measure.
17. Two-step return: include the delay of the next decision the same pair
    makes (handoff chains).
18. Learn a cost per microsecond of the running task's remaining burst, and
    compare with the wakee's predicted burst.
19. Score the cache cost: the preempted task's next burst length against
    its usual (a cold cache runs longer).
20. Score migrations: when queued, did the wakee run on another CPU, and
    how long did its burst take there.

## B. Learn about the running task (the waker)

21. Learn whether the waker sleeps soon after waking someone (the run after
    a wake, `MEASURE_WAKE`); preempt only wakers that keep running.
22. Learn the waker's typical run after a wake per program.
23. Learn whether the waker wakes the same wakee again soon (a ping-pong):
    then a handoff is cheap.
24. Learn the waker's wake rate: a task that wakes others every few
    microseconds is a producer that will not yield.
25. Learn the waker's burst length as a distribution (two bins), and
    preempt only in the long mode.
26. Use the waker's run since its last sleep against its learned burst: far
    past it means a spinner.
27. Learn whether the waker is a lock holder: it wakes a task right before
    it sleeps (unlock then wait) or keeps going (unlock and continue).
28. Learn per waker how often a preemption of it was followed by its quick
    return (low cost).
29. Learn the waker's futex call rate from tracepoints (syscalls), as a
    feature.
30. Learn whether the waker's next burst after a preemption is longer (a
    cost signal).

## C. Learn about the wakee

31. Predicted burst of the wakee, unclamped: a 5 us wakee costs almost
    nothing to run first.
32. Learn the wakee's wakeup-to-sleep time distribution and preempt only
    for the short mode.
33. Learn whether the wakee wakes its waker in turn (a handoff pair).
34. Learn the wakee's sensitivity: how much its program's throughput moves
    with its wait (from its burst rate).
35. Learn the wakee's timeout: a task that sleeps with a short timeout
    (futex wait 5 us) will wake again anyway; preempt to let it run before.
36. Learn whether the wakee sleeps again right after running (a short
    episode): then its slice can be tiny, so preempting is cheap.
37. Learn per wakee the wait it usually gets when queued; preempt only when
    that wait is long.
38. Learn per wakee program whether its waits are on the waker's CPU
    (same-CPU handoff) or elsewhere.
39. Learn the wakee's chain depth: a wakee that wakes another wakee starts
    a chain where each wait adds up.
40. Learn the wakee's cache footprint from its burst length on a new CPU.

## D. Learn where to put the wakee

41. Learn per pair whether the wakee runs sooner on the waker's CPU
    (preempt) or on its own CPU (queue there).
42. Learn per pair whether to pull the wakee to the waker's CPU when no CPU
    is idle (`WAKER_PREEMPT` 2, but learned).
43. Learn the wait on each CPU queue and place the wakee on the shortest
    predicted wait (`PRED_PLACE`, tried for perf messaging).
44. Learn whether to place the wakee on the waker's SMT sibling.
45. Learn per pair whether to queue the wakee on the stealable CPU queue of
    its previous CPU or the domain queue.
46. Learn which CPU to kick for a queued wakee: the one whose task ends
    soonest by prediction.
47. Learn per pair the cost of a migration, and keep the pair together if
    high.
48. Learn to wake an idle CPU early for a wakee expected soon (the waker's
    wake rate).
49. Learn whether to run the wakee on the waker's CPU right after the
    waker's next sleep (deferred handoff).
50. Learn per program whether waking an idle CPU costs more than waiting
    (wakeup latency of idle states).

## E. Learn the slice instead of the preemption

51. Learn a shorter slice for tasks that wake others while running: the
    wakee then waits a short slice.
52. Learn the slice from the wakee waits it causes: shrink a task's slice
    when its wakees wait long behind it.
53. Learn a slice per running task from the delay its preemption caused
    (lower cost, shorter slice).
54. Give a waker that never sleeps the minimum slice once its wakees are
    seen waiting.
55. Learn a slice that ends near the waker's next futex call.
56. Learn the slice from the wakee's deadline: end the running slice at the
    wakee's deadline (EEVDF's rule, learned per pair whether to use it).
57. Learn a slice extension for lock holders (finish the critical section),
    and a cut for spinners.
58. Learn per program the slice that maximizes its burst rate.
59. Learn per CPU the slice from the number of waiting wakees.
60. Learn a slice from the gap between the running task's prediction and
    its run so far.

## F. Learn with a model, not a table

61. Add "the running task's run so far" and "the wakee's predicted burst"
    as features to a per-task linear model that predicts the wakee's wait.
62. A per-program logistic model of "preempting helps" from both tasks'
    features.
63. A small decision tree on the two delays and the two predicted bursts.
64. A two-layer fixed-point network in BPF predicting the delay of each
    choice.
65. Learn the delays with the existing LMS model, two outputs per cell.
66. Learn a threshold on the wakee's predicted burst (below it, preempt) by
    stochastic gradient on the observed delays.
67. Learn a threshold on the running task's run so far (above it, preempt).
68. Learn a ratio: preempt if the running task's remaining is k times the
    wakee's burst, with k learned per program.
69. Learn a per-program deadline scale (how much half the slice counts) for
    wakeups.
70. Learn the wakee's deadline offset from its observed waits.
71. A contextual bandit with features (both bursts, queue length, idle
    CPUs) and a linear reward model.
72. Online regression of the wakee's wait on the running task's features.
73. Learn per program a Markov model of burst lengths and predict the
    running task's remaining time.
74. Learn the wakeup pattern as a time series (the gap between wakes) and
    predict the next wake.
75. Learn an embedding of programs by their wake graph and share cells
    between similar programs.

## G. Learn from the workload as a whole

76. Learn per domain the share of wakeups that would preempt and the
    total delay, and turn preemption on only when it lowers the total.
77. Learn per program its throughput (bursts per second) under each choice
    over 100 ms windows.
78. Learn which programs are latency-sensitive from their sleep pattern
    (short bursts, many wakes) and preempt only for them.
79. Learn the system load at which preemption stops helping.
80. Learn when the waker's CPU is busy with only the waker (the futex case)
    against many tasks (schbench).
81. Learn per cgroup whether to preempt.
82. Learn from the run queue length when the wakee arrives.
83. Learn per CPU whether the last preemptions on it paid off.
84. Learn a global explore rate from how often the table's choices change.
85. A shadow model: score both choices on every wakeup with the model, and
    act only after the shadow is right more than a learned share of times.

## H. Learn about locks

86. Learn which futex addresses a program waits on (through a tracepoint)
    and treat tasks of one address as a group.
87. Learn the critical section length per program from the time between a
    futex wake and the waker's next futex wait.
88. Learn a lock-holder proxy: when a wakee waits on a lock the running
    task holds, run the holder first.
89. Learn whether a wake is a lock handoff (the waker then waits) or a
    signal (it continues), per program.
90. Learn the lock's waiter count from the wake rate and the wait rate.
91. Learn per program whether it uses FUTEX_WAKE with a timeout waiter
    (stress-ng futex) and treat that as a spin handoff.
92. Learn the futex wake-to-wait gap per pair and preempt when it is
    shorter than the slice.
93. Learn which tasks spin (run past their prediction without sleeping
    while others wait on them).

## I. Learn when to stop learning

94. Freeze a cell's choice when its two delays differ by far, and stop
    exploring it.
95. Fall back to the release behavior when a cell's estimates disagree
    with the outcomes (a confidence check).
96. Learn a confidence per cell from the variance of the delays.
97. Age cells out with an LRU table, as the program table does.
98. Bound the delay a preemption can cause per task per second, so a
    learned choice cannot starve a task.
99. Learn only while the gap between the two delays is significant, and
    otherwise keep the release behavior.
100. Check every learned choice against fairness: preempt only when the
     wakee's deadline is earlier, as EEVDF does, so learning decides only
     between fair choices.
