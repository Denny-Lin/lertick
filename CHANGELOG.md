# Changelog

## Unreleased — 2026-10-09

### Lertick rebranding and CPU-only scope

- Renamed the project to **Lertick — A Self-Learning CPU Scheduler for Linux**,
  with repository `Denny-Lin/lertick` and scheduler binary/ops `scx_lertick`.
- Narrowed active work to online learning for CPU scheduling with sched_ext and
  eBPF. Memory allocation, reclaim, DAMON/DAMOS, NUMA memory placement, and joint
  CPU/memory management are preserved as historical background only.
- Preserved the Phase 1 algorithm and original validation evidence. Historical
  names, paths, and measurements remain in the evidence and Phase 0 archive.
- Updated public documentation, examples, metadata, and contribution templates.
  This change does not begin Phase 2 or claim a scheduling performance gain.

### Phase 1 implementation and validation (originally ScxSprout)

- Implemented ScxSprout in C/eBPF using the pinned official sched_ext C examples,
  with weighted virtual-time dispatch and bounded fixed/shadow/online slices.
- Added verified wake-to-sleep CPU accounting, five-feature Q16 online training
  and inference inside eBPF, a C/libbpf loader, and JSONL task/model telemetry.
- Added calibrated real CPU workloads, independent 128-bit model tests,
  reproducible Ubuntu/QEMU setup, and a Linux deployment/recovery suite.
- Compiled and executed on Ubuntu 24.04.5 ARM64, Linux `7.0.0-38-generic`,
  QEMU 10.2.1 with HVF. Fixed, shadow and online BPF acceptance/registration,
  model updates, adaptive execution and safe recovery passed.
- Recorded three four-second comparison repetitions per policy against the
  default Linux scheduler and official non-ML `scx_simple`. Online p99 completion
  latency was worse than fixed and default in this short comparison; no ML
  improvement is demonstrated. See [Phase 1 results](docs/PHASE1_RESULTS.md) and
  [validation evidence](results/phase1/validation.json).
- Retained upstream GPL provenance alongside the original MIT model, workloads
  and documents; see [THIRD_PARTY](THIRD_PARTY.md).

### Original Phase 0 background research

- Added a primary-source review of Linux resource learning and related projects.
- Compared six directions and originally proposed ScxSprout with a memory-controller
  backup. That backup is superseded by the current CPU-only scope.
- Documented architecture, deployment, benchmarks, effort, licensing, and contribution plans.
- Added repository templates and documentation validation in CI.

This is a validated Phase 1 prototype in one tested Linux environment, not a
production release or a claim of scheduling performance improvement.
