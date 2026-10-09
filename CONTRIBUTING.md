# Contributing to Lertick

Lertick is an experimental CPU scheduler focused on online machine learning in Linux sched_ext and eBPF. Phase 1 implements and validates a minimal Linux scheduler. Contributions can improve correctness, compatibility, reproducibility, and documentation. Current maintainership belongs to the repository owner, Denny-Lin; this is a new project with no claimed external contributor community yet.

Active work covers CPU-burst prediction, bounded adaptive time slices, CPU scheduling decisions, fairness, starvation prevention, low overhead, and reproducible benchmarks. Memory allocation, reclaim, DAMON/DAMOS control, NUMA memory placement, and joint CPU/memory management are outside the project scope. The [Phase 0 archive](docs/planning/README.md) preserves their historical research value without making them development alternatives.

## Workflow

1. Read the [current results](docs/PHASE1_RESULTS.md), [deployment guide](docs/DEPLOYMENT.md), and historical [prior art](docs/planning/PRIOR_ART.md).
2. For a design change, open a focused proposal with the problem, alternatives, primary references, and falsifiable validation. Small documentation fixes may go directly to a pull request.
3. Fork and create a topic branch. Keep one logical change per commit; use an imperative subject and explain why the change is needed.
4. Run `python3 tests/check_docs.py`, `make test-model`, and `git diff --check`. Changes to scheduling or loading also need actual Linux verifier, execution, and recovery evidence from the [validation runner](tests/README.md). Document checks you could not run.
5. Submit a pull request with scope, evidence, tests, compatibility, recovery implications, and licensing provenance. Do not invent measurements.

Contributions are submitted under the applicable file's license. No CLA is currently required. Do not contribute material you cannot license. Preserve authorship and upstream SPDX identifiers. Review the implemented [license boundaries and provenance](THIRD_PARTY.md) before C/eBPF contributions.

## Style and evidence

C/eBPF follows [Linux kernel coding conventions](https://docs.kernel.org/process/coding-style.html): 8-column tabs, short functions, descriptive identifiers, explicit ownership, and bounded hot-path work. Document units, fixed-point scale, overflow bounds, locking, and verifier assumptions. Task storage is allocated in task initialization; hot-path model loops are bounded.

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

The recorded Ubuntu ARM64 kernel `7.0.0-38-generic` passed actual runtime validation in QEMU HVF. This is one experimental environment, not a supported kernel matrix. Compatibility reports must identify kernel release and config hash, architecture, CPU topology, Ubuntu image, libbpf/LLVM versions, source commits, and VM accelerator. A successful build alone does not establish runtime compatibility. CI must not run privileged experiments on ordinary shared runners.

## Maintainer review checklist

- Is the problem concrete and already covered by prior art?
- Are facts sourced and claims supported by raw data?
- Are integer arithmetic, task lifecycle, concurrency, fairness, and resource bounds justified?
- Are new tests tied to real failure cases rather than reproducing implementation details?
- Are load, shutdown, failure recovery, and compatibility results recorded?
- Are licenses and copied code provenance correct?
- Does the change keep the MVP small and independently reproducible?

See [good first issue proposals](docs/GOOD_FIRST_ISSUES.md). Performance submissions use the benchmark issue template, [current workload guide](benchmarks/README.md), and CPU sections of the [historical protocol](docs/planning/BENCHMARK_PLAN.md). Phase 2 has not been approved; proposals do not authorize a new implementation phase.
