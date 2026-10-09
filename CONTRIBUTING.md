# Contributing

Phase 0 accepts research corrections, documentation, test-design proposals, and architecture review. Scheduler implementation begins after the research direction is approved. Current maintainership belongs to the repository owner, Denny-Lin; this is a new project with no claimed external contributor community yet.

## Workflow

1. Read the [recommendation](docs/planning/RECOMMENDATION.md), [prior art](docs/planning/PRIOR_ART.md), and [roadmap](docs/planning/ROADMAP.md).
2. For a design change, open a focused proposal with the problem, alternatives, primary references, and falsifiable validation. Small documentation fixes may go directly to a pull request.
3. Fork and create a topic branch. Keep one logical change per commit; use an imperative subject and explain why the change is needed.
4. Run `python3 tests/check_docs.py` and `git diff --check`. Document any checks you could not run.
5. Submit a pull request with scope, evidence, tests, compatibility, recovery implications, and licensing provenance. Do not invent measurements.

Contributions are submitted under the applicable file's license. No CLA is currently required. Do not contribute material you cannot license. Preserve authorship and upstream SPDX identifiers. Review [license boundaries](docs/planning/LICENSE_ANALYSIS.md) before future C/eBPF contributions.

## Style and evidence

Future C/eBPF follows [Linux kernel coding conventions](https://docs.kernel.org/process/coding-style.html): 8-column tabs, short functions, descriptive identifiers, explicit ownership, and bounded hot-path work. Document units, fixed-point scale, overflow bounds, locking, and verifier assumptions. Avoid allocation and unbounded iteration in scheduling callbacks.

Documentation uses plain English, descriptive Markdown links, and primary sources. Label statements as **verified**, **estimate**, or **hypothesis** when their status matters. Include checked dates; pin source commits for reproducible experiments. Traditional Chinese improvements are welcome; keep status and limitations consistent with the English README.

## Independent contribution tracks

| Track | Suitable work now | Validation later |
| --- | --- | --- |
| Documentation | Source corrections, glossary, translations | Local links and source verification |
| Compatibility | Review kernel configs and versioned APIs | Exact guest/kernel/toolchain manifest and verifier logs |
| Learning | Analyze censoring, overflow, and drift | Deterministic replay; compare against EWMA and frozen models |
| Benchmarks | Review workload and fairness protocol | Raw repeated trials and reproducible commands |
| Safety | Review rollback and fault cases | VM exit, loader death, watchdog, and emergency recovery |

## Compatibility expectations

No runtime combination is currently supported. Future compatibility reports must identify kernel release and config hash, architecture, CPU topology, Ubuntu image, libbpf/LLVM versions, source commits, and VM accelerator. A successful build alone does not establish runtime compatibility. CI must not run privileged experiments on ordinary shared runners.

## Maintainer review checklist

- Is the problem concrete and already covered by prior art?
- Are facts sourced and claims supported by raw data?
- Are integer arithmetic, task lifecycle, concurrency, fairness, and resource bounds justified?
- Are new tests tied to real failure cases rather than reproducing implementation details?
- Are load, shutdown, failure recovery, and compatibility results recorded?
- Are licenses and copied code provenance correct?
- Does the change keep the MVP small and independently reproducible?

See [good first issue proposals](docs/GOOD_FIRST_ISSUES.md). Performance submissions use the benchmark issue template and [protocol](docs/planning/BENCHMARK_PLAN.md).
