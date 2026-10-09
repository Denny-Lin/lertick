# Good first issue proposals

These are proposed tasks, not already-created GitHub issues. Keep implementation tasks behind the direction-review gate.

| Task | Track | Completion evidence |
| --- | --- | --- |
| Check five prior-art links and version dates | Documentation | Exact sources and corrections in a PR |
| Translate deployment terminology into Traditional Chinese | Documentation | Technical review and synchronized status wording |
| Audit Linux 6.12 and one newer sched_ext interface | Compatibility | Callback/kfunc differences linked to immutable source |
| Review CPU-burst labels under preemption, sleep, yield, and exit | Algorithm design | A state-transition table and counterexamples |
| Specify fixed-point clipping and overflow bounds | Algorithm design | Worked maximum-value calculations |
| Review benchmark randomization and percentile aggregation | Experiment design | A reproducible statistical protocol |
| Design fairness and starvation adversarial workloads | Tests | Expected bounds and observable failure conditions |
| Review VM emergency-recovery commands | Safety documentation | Primary documentation; later a recorded VM trial |

Suggested labels: `good first issue`, `documentation`, `research`, `compatibility`, `benchmark`, `algorithm`, `safety`. A task may need a mentor even when small. Re-check scope and assign a maintainer before applying the first label.
