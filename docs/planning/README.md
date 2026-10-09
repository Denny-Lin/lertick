# Phase 0 background research archive

Lertick is dedicated to online machine learning for Linux CPU scheduling
using sched_ext and eBPF. This directory preserves the original Phase 0 survey,
comparisons, estimates, and proposals so their evidence and reasoning remain
available. It is an archive, not the current development plan.

The documents retain historical names **ScxSprout**, `scx-sprout`, and
`linux-resource-learning`, as well as statements made before Phase 1 existed.
Current behavior, build commands, validation results, and scope are documented
in the [main README](../../README.md), [deployment guide](../DEPLOYMENT.md),
[Phase 1 results](../PHASE1_RESULTS.md), and [decision record](../DECISIONS.md).

Memory allocation optimization, memory reclaim, DAMON/DAMOS controllers, NUMA
memory placement, and joint CPU/memory resource management are **background
research only**. They are neither active development directions nor fallback
projects. Original passages describing a memory-controller "backup" or later
memory work are superseded by Lertick's CPU-only scope. CPU topology metadata
and BPF map memory bounds remain relevant to scheduler testing and safety.

| Archived document | Preserved research value |
| --- | --- |
| [RESEARCH](RESEARCH.md) | Primary-source feasibility review and original hypotheses |
| [PRIOR_ART](PRIOR_ART.md) | CPU scheduling, in-kernel ML, and background memory literature |
| [PROJECT_COMPARISON](PROJECT_COMPARISON.md) | Original comparison of six possible project directions |
| [ARCHITECTURE](ARCHITECTURE.md) | Initial CPU design and superseded memory-controller alternative |
| [OPEN_SOURCE_STRATEGY](OPEN_SOURCE_STRATEGY.md) | Original naming checks and contribution proposals |
| [LICENSE_ANALYSIS](LICENSE_ANALYSIS.md) | Original licensing analysis; current boundaries are in [THIRD_PARTY](../../THIRD_PARTY.md) |
| [ROADMAP](ROADMAP.md) | Original phases, effort estimates, and decision gates |
| [BENCHMARK_PLAN](BENCHMARK_PLAN.md) | CPU measurement ideas plus a superseded memory experiment |
| [RECOMMENDATION](RECOMMENDATION.md) | Original CPU recommendation and risks before implementation |

Relevant CPU experiment designs may inform future proposals, but unimplemented
baselines, compatibility targets, and performance gates are not completed work.
Phase 2 implementation requires explicit approval. Keeping a historical
proposal here does not authorize that proposal or imply a performance result.
