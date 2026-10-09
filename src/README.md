# Phase 1 source

```text
src/
  bpf/scx_sprout.bpf.c     sched_ext callbacks, runtime labels and online training
  userspace/scx_sprout.c   C/libbpf loader, compatibility checks and JSONL telemetry
  shared/model.h          bounded five-feature Q16 model, host/BPF shared arithmetic
  shared/sprout.h         experiment parameters and telemetry ABI
```

The scheduler follows the official `scx_simple` global weighted-vtime policy.
It accounts for actual CPU execution with `sum_exec_runtime`. Verified
wake-to-sleep episodes provide capped burst labels; ordinary preemption keeps
the episode open. Model output chooses a bounded slice while dispatch ordering
and fairness charge remain based on the policy and actual execution.

`fixed` mode uses a constant slice. `shadow` trains and predicts while keeping
the fixed slice. `online` enables prediction-selected slices after warmup, with
fixed-slice fallback after actual service exceeds the prediction. Training and
inference execute in the BPF callbacks; the loader only reads telemetry.

Slice bounds apply to the budget inserted by BPF. A task resuming an earlier
budget may start with a remaining slice below the configured minimum. On the
continuing-task dispatch path, a decision can precede `stopping` accounting for
the latest execution segment; its remaining-service estimate can therefore use
stale accumulated service. Completed labels still include the segment after
accounting. This Phase 1 policy limitation needs correction before performance
tuning.

The snapshot's `exited` flag means `exit_task` released the task's sched_ext
association. That callback also occurs during scheduler unload, so the flag
does not by itself establish OS process death. Use process status independently.

Build in a compatible Linux guest with `make`; generated running-kernel BTF,
BPF objects and skeletons belong in `build/`. Source availability and a host
model test do not prove verifier acceptance or Linux deployment. Record actual
kernel/build/load results separately, following [deployment](../docs/DEPLOYMENT.md).

The original model is MIT. Kernel-facing sources and adapted reference material
carry their own GPL-compatible notices; see [third-party provenance](../THIRD_PARTY.md)
and the [architecture](../docs/planning/ARCHITECTURE.md).
