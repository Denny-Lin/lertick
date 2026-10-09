# Source boundary

No scheduler source exists in Phase 0. Proposed layout after direction approval:

```text
src/
  bpf/                 sched_ext callbacks, bounded telemetry, online model
  userspace/           C/libbpf loader, configuration, statistics, shutdown
  shared/              auditable model and fixed-point definitions
```

See the [architecture](../docs/planning/ARCHITECTURE.md). Future BPF code needs a GPL-compatible license declaration and per-file provenance; current MIT documentation does not relicense upstream material. Generated BTF headers and skeletons belong in `build/`, not version control.
