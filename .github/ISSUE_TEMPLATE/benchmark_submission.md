---
name: Benchmark submission
about: Submit reproducible Lertick CPU measurements, including regressions
title: "[benchmark] "
labels: benchmark
---

## Question and policies compared

## Immutable environment and workload manifest

Include kernel/config, source SHAs, toolchain, CPU/NUMA/SMT, VM or bare metal, governor, affinity, workload versions, seeds, and load generator.

## Protocol and raw trials

Include warmup, randomized order, repetitions, raw CSV/JSON, failed trials, summary script, and confidence intervals. Follow benchmarks/README.md and the CPU sections of docs/planning/BENCHMARK_PLAN.md. The archived memory-controller experiment is outside the project scope.

## Results and limitations

Include absolute throughput/latency, p95/p99, fairness, starvation, migrations, model costs, and matched baselines. Do not submit only an improvement percentage. Distinguish training and inference cost and disclose instrumentation.

## Recovery verification

Include the scheduler state after unload and any failure or delayed recovery.
