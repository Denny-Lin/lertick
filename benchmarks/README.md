# Benchmark boundary

No performance measurements or runnable scheduler benchmark scripts exist yet. The [protocol](../docs/planning/BENCHMARK_PLAN.md) defines what later scripts must collect and how results must be compared.

After approval, provide small orchestration scripts for environment capture, randomized trials, raw CSV/JSON output, overhead ablations, and summary generation. Workload code and external benchmark versions must be pinned. Every result needs an environment manifest and failure log; never publish a percent improvement without its denominator and raw trials.

VM tests first validate deployment and recovery. Bare-metal tests are required before broader CPU-performance conclusions. Memory pressure and recovery experiments need separate protocols and cannot silently be mixed into scheduler results.
