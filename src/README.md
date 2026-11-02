# Source map

```text
src/
  bpf/scx_lertick.bpf.c     sched_ext callbacks, runtime labels and online training
  userspace/scx_lertick.c   C/libbpf loader, compatibility checks and JSONL telemetry
  userspace/runtime.c      private output, lifetime/maintenance locks and sleep helpers
  userspace/lerwatt_plugin.c optional topology, demand and pinned-map integration
  shared/model.h          bounded five-feature Q16 model, host/BPF shared arithmetic
  shared/lertick.h         experiment parameters and telemetry ABI
```

The scheduler follows the official `scx_simple` global weighted-vtime policy.
It accounts for actual CPU execution with `sum_exec_runtime`. Verified
wake-to-sleep episodes provide capped burst labels; ordinary preemption keeps
the episode open. Model output chooses a bounded slice while dispatch ordering
and fairness charge remain based on the policy and actual execution.

`fixed` mode uses a constant slice. `shadow` trains and predicts while keeping
the fixed slice. `online` enables prediction-selected slices after warmup, with
fixed-slice fallback after actual service exceeds the prediction. Training and
inference execute in the BPF callbacks; the loader configures, attaches and
observes the scheduler and can publish the optional Lerwatt demand records.

Slice bounds apply to the budget inserted by BPF. A task resuming an earlier
budget may start with a remaining slice below the configured minimum. On the
continuing-task dispatch path, decisions include both committed and pending
actual CPU service. Only `stopping` commits runtime and fairness charges, which
prevents double charging. See the [Phase 2 correctness record](../docs/PHASE2_CORRECTNESS.md)
for the implemented invariants and remaining proof limits.

The snapshot's `exited` flag means `exit_task` released the task's sched_ext
association. That callback also occurs during scheduler unload, so the flag
does not by itself establish OS process death. Use process status independently.

Build on a compatible Linux machine with `make native`; generated running-kernel BTF,
BPF objects and skeletons belong in `build/`. Source availability and a host
model test do not prove verifier acceptance or Linux deployment. Record actual
kernel/build/load results separately, following [native installation](../docs/INSTALL.md)
or the preserved [VM reproduction](../docs/DEPLOYMENT.md).

The original model is MIT. Kernel-facing sources and adapted reference material
carry their own GPL-compatible notices; see [third-party provenance](../docs/legal/THIRD_PARTY.md)
and the [architecture](../docs/planning/ARCHITECTURE.md).
