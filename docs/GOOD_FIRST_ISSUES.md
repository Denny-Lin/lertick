# Lertick contribution proposals

These are proposed tasks, not already-created GitHub issues. Phase 1 passed Linux validation; see [current results](PHASE1_RESULTS.md) before choosing a follow-up. Lertick's active scope is CPU scheduling. Phase 2 implementation needs explicit approval.

| Task | Track | Completion evidence |
| --- | --- | --- |
| Check five prior-art links and version dates | Documentation | Exact sources and corrections in a PR |
| Translate deployment terminology into Traditional Chinese | Documentation | Technical review and synchronized status wording |
| Audit Linux 6.12 and one newer sched_ext interface | Compatibility | Callback/kfunc differences linked to immutable source |
| Review CPU-burst labels under preemption, sleep, yield, and exit | Algorithm design | A state-transition table and counterexamples |
| Audit fixed-point clipping and overflow bounds | Algorithm design | Independent counterexamples beyond the existing arithmetic oracle |
| Review benchmark randomization and percentile aggregation | Experiment design | A reproducible statistical protocol |
| Design fairness and starvation adversarial workloads | Tests | Expected bounds and observable failure conditions |
| Extend VM recovery tests to task churn and repeated attach/detach | Safety and tests | Recorded trials with final disabled state |

Suggested labels: `good first issue`, `documentation`, `research`, `compatibility`, `benchmark`, `algorithm`, `safety`. A task may need a mentor even when small. Re-check scope and assign a maintainer before applying the first label.
