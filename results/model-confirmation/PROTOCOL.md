# Longer burst-model confirmation

Written before collecting these confirmation results. Model coefficients,
initializers, update rules, feature definitions, caps and slice bounds are
frozen at the implementation validated in `6beaba2`. Only workloads and
measurement controls change; original defaults remain bias/trunc.

## Common method

One eight-vCPU ARM64 QEMU guest, four workload CPUs `0,1,2,3`; 30 s measured
time after 5 s warm-up, five randomized policy/workload repetition blocks.
The kernel wake observer is enabled in every trial. Candidate aliases select
EWMA initialization with fractional carry; baseline aliases select bias/trunc.
Record and verify actual model configuration, input seed, source/binary hashes,
and kernel preemption. Keep every trial, outlier and failure.

Each full or lazy session is independently randomized. Their grouped time
order is not a randomized per-trial preemption experiment; do not claim a
causal preemption effect or pool the earlier pilot/factorial with these runs.
The primary comparison is candidate versus baseline within each environment.
Every preemption change occurs only while sched_ext is disabled, and the
original selected mode is restored after success, failure or interruption.
No VM workload experiment overlaps the six Lerwatt runtime confirmation cases.

## Declared stages

| Stage | Workloads | Policies | Preemption | Sample pages |
| --- | --- | --- | --- | --- |
| Stable accuracy | shortburst, wakeheavy, mixed, multicore | shadow, shadow-ewma-carry | full and lazy | prefaulted |
| Online utility | same four | fixed, ewma, online, online-ewma-carry | original lazy | prefaulted |
| Fresh input patterns | stepburst, alternatingburst, rampburst, randomstepburst | shadow, shadow-ewma-carry | full and lazy | prefaulted |
| Observer ablation | wakeheavy, multicore | shadow, shadow-ewma-carry | original lazy | demand and prefaulted |

Stages may be split by workload subset or environment to keep the small guest
disk below capacity. Each subset still retains all five repetitions. Offload
completed raw data to the host and verify its archive before clearing only
this experiment's generated guest files. Keep existing user data untouched.

The default order seed is `2026110201`, and burst input seed `2026110211` plus
repetition index. The four fresh input generators were fixed before any
scheduler-model score on them. Steps switch 75us/2500us every 7 s; alternating
jobs use 75us/1750us; ramps span 75us to 3000us over 4 s; random steps retain
one seeded 75–2500us target for 17 jobs. Four burst workers and four CPU workers
use the selected CPU set. Targets/indices are logged afterward, never passed
to the scheduler. These are custom input families, not real applications.

## Scores and decision boundaries

Use global prequential model-error sums and eligible-label counts; report the
same-episode EWMA ratio. The one-second live baseline remains approximate.
Repetition, not an individual burst, is the bootstrap unit. Report paired
median changes and coarse 95% bootstrap intervals without claiming hardware
or general Linux superiority. Keep separate workload results; no cross-unit
throughput aggregate is meaningful.

Report online compute throughput, kernel wake p99, burst completion p99 and
fixed/EWMA controls. Lower prediction error is not automatically a scheduling
advantage. A claimed learned advantage over EWMA needs actual same-episode
error below that baseline; catching up is not a win over it. New defaults
need consistent accuracy and acceptable measured online tradeoffs, not only
better arithmetic or one favorable profile. If evidence is inconsistent,
retain defaults and publish the negative results.

The observer ablation tests whether logging-buffer first-touch faults affect
the conclusions. It does not authorize removing difficult episodes or
retroactively recasting the old pilot. No model parameters may be retuned
against confirmation outcomes. Additional work must fit the session deadline;
any omitted or incomplete stage is explicitly reported.
