# Open-source strategy

Status: Phase 0 planning. Source/name checks: 2026-10-09. The public umbrella repository is `Denny-Lin/linux-resource-learning`; the proposed CPU scheduler brand is **ScxSprout (`scx-sprout`)**. The name and direction are proposals, not an approved implementation or software release.

## Name candidates and observed collisions

We queried GitHub's public repository search API using `<name> in:name`. Search tokenization can produce partial matches; counts are observations, not exact-name ownership checks. Search cannot find private/unindexed projects, settle trademarks, or guarantee continuing availability.

| Candidate | Public results | Examples / interpretation |
| --- | ---: | --- |
| scx-learn | 1 | `Tian-scx/learn`; apparent token match, inspect before adopting |
| scx-adapt | 3 | `dogukanmeral/scx-adapt`, `onurkaragur/scx-adapt-experiments`; avoid close overlap |
| kernlearn | 2 | `crimbs/kernlearn`, `MartenThompson/gp_kernlearn`; avoid |
| mempilot | 9 | `ViktorAxelsen/MemPilot`, `Cosmos-0118/MemPilot`; crowded |
| schedmind | 4 | Includes `tanzeela-mahmood/schedmind`; avoid |
| scx-sprout | 0 | Preferred provisional CPU brand; conveys incremental growth |
| reclaimwise | 0 | Suitable provisional name for a memory-controller backup |
| memsprig | 0 | Short memory-oriented alternative; meaning less obvious |
| scx-bud | 0 | Short CPU alternative; less descriptive |
| learnreclaim | 0 | Clear memory alternative; longer pronunciation |

The broad `linux-resource-learning in:name` query returned 11 partial matches, mostly Linux-learning resource collections. A direct request for `repos/Denny-Lin/linux-resource-learning` returned HTTP 404 before creation. This confirms only the observed account-path availability at that time.

Re-run the [public name query](https://github.com/search?q=scx-sprout+in%3Aname&type=repositories) and inspect results before a branded release. Avoid logos or language suggesting endorsement by the Linux Foundation, sched-ext maintainers, Meta, or another company. No trademark clearance has been performed.

## Repository positioning

Proposed description: **Research and design for lightweight online learning in Linux CPU scheduling and memory management.**

Tagline: **Small, measurable online learning for Linux resource management.**

Topics: `linux`, `ebpf`, `sched-ext`, `online-learning`, `cpu-scheduling`, `memory-management`, `systems-research`, `open-source`.

The README must let a visitor learn the problem, current maturity, candidate mechanism, evidence requirements, and contribution path within 30 seconds. Lead with phase status and the narrow question. Put quick-start documentation checks before future runtime commands. Link the primary-source review so readers can evaluate overlap. Publish a runnable install guide only when the implementation and compatibility evidence exist.

## Structure and ownership

```text
README.md / README.zh-TW.md       concise English/Traditional Chinese entry points
LICENSE                          MIT for original Phase 0 material
CONTRIBUTING.md                   workflow, style, compatibility, review
CODE_OF_CONDUCT.md / SECURITY.md  community and reporting expectations
CHANGELOG.md                     actual delivered changes
docs/planning/                   nine required planning deliverables
docs/DEPLOYMENT.md                proposed Ubuntu/QEMU setup and recovery
src/                             documented future C/BPF boundary
tests/                           current docs checker; future semantic/VM tests
benchmarks/                      future reproducible harness contract
examples/                        future terminal demonstration contract
.github/workflows/               unprivileged documentation CI
.github/ISSUE_TEMPLATE/           bug, research, benchmark submissions
```

Source, benchmark, and demo directories currently contain boundary documents, not simulated implementations. Keep a kernel mirror, framework stack, cloud runtime, and container orchestration out of the MVP. Add dependencies only when a concrete test or kernel interface requires them.

## Contribution strategy

Use [Linux kernel C conventions](https://docs.kernel.org/process/coding-style.html), file-level SPDX licensing, short auditable functions, bounded arithmetic, and explicit task/map lifetimes. Documentation and experiment-design work can proceed independently of model and BPF code.

The [contribution guide](../../CONTRIBUTING.md) defines the pull-request workflow and maintainer checklist. The [starter tasks](../GOOD_FIRST_ISSUES.md) offer source validation, translations, lifecycle analysis, arithmetic review, test design, and compatibility audits. Mark tasks as `good first issue` only after a maintainer supplies clear scope and support. Do not manufacture contributor activity or create issue volume to imply adoption.

Compatibility reports must distinguish build, verifier acceptance, successful attach, task scheduling, recovery, and performance. Each needs exact version/config evidence. Prefer one pinned target with complete checks over a long unsupported kernel list.

## Demonstration and credible adoption

The future [terminal demo](../../examples/README.md) should show foreground startup, learned updates, actual bounded policy choices, workload measurements, and return to the default scheduler. Include skipped/censored labels and costs. Do not print fake improvement numbers or use documentation checks as evidence that the scheduler works.

Before inviting broad deployment, publish the minimal source, exact environment manifest, raw benchmark data, negative results, recovery recordings, limitations, and license inventory. A technical article can explain the model and tradeoffs; a short demo can reproduce one result. Engage upstream through concrete bug reports and small compatibility fixes after local validation. Stars are an outcome, not a technical acceptance criterion.

The repository owner is the initial maintainer. A private conduct-reporting contact and a second reviewer are unresolved community-launch requirements. Security reporting uses GitHub private vulnerability reporting. Avoid promising review or security response times the maintainer cannot sustain.
