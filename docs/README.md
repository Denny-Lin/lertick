# Lertick documentation

[← Product overview](../README.md)

## Install and use

| I want to… | Read |
| --- | --- |
| Install on a compatible Linux kernel | [Installation](INSTALL.md) |
| Run continuously, update, roll back or collect a report | [Daily use](DAILY_USE.md) |
| Connect Lerwatt while keeping both AIs independent | [Integration guide](INTEGRATION.md) |
| Reproduce the VM environment | [VM deployment](DEPLOYMENT.md) |
| See delivered changes | [Changelog](CHANGELOG.md) |

## Understand and develop

| Topic | Read |
| --- | --- |
| Model, scheduling behavior and boundaries | [Implementation overview](OVERVIEW.md) · [Correctness](PHASE2_CORRECTNESS.md) |
| Source layout and validation tools | [Source](../src/README.md) · [Tests](../tests/README.md) · [Benchmarks](../benchmarks/README.md) · [Examples](../examples/README.md) |
| Design choices and open work | [Decisions](DECISIONS.md) · [Optimization](PHASE2_OPTIMIZATION.md) · [Starter tasks](GOOD_FIRST_ISSUES.md) |
| Project name and earlier names | [Naming](NAMING.md) · [Rebrand validation](REBRAND_VALIDATION.md) |

## Results and research

| Evidence | Read |
| --- | --- |
| Independent controller updates and service lifecycle | [ABI 2 validation](../results/integration-abi2/README.md) |
| Scheduling outcomes and limitations | [Phase 2 results](PHASE2_RESULTS.md) · [Benchmark protocol](PHASE2_BENCHMARKS.md) |
| Model variants | [Model confirmation](../results/model-confirmation/README.md) · [Prediction studies](../results/model-precision/README.md) |
| Earlier kernel and controller validation | [Phase 1](PHASE1_RESULTS.md) · [Lerwatt VM integration](LERWATT_VM_VALIDATION.md) |
| Original proposals and source research | [Planning archive](planning/README.md) |

## Contribute and report

[Contributing](../.github/CONTRIBUTING.md) · [Code of conduct](../.github/CODE_OF_CONDUCT.md) · [Security policy](../.github/SECURITY.md) · [License boundaries and provenance](legal/THIRD_PARTY.md)

The [product overview](../README.md) is the entry point; `docs/` contains guides, `docs/planning/` retains original proposals, and `results/` retains evidence. GitHub community policies live in `.github/`. [AGENTS.md](../AGENTS.md) records repository instructions for coding agents. Keep the root [license](../LICENSE) and [GPL text](../LICENSES/GPL-2.0-only.txt) with the applicable source notices.
