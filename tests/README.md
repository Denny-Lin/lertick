# Validation

Run `python3 tests/check_docs.py` from any working directory. It checks required planning files, local Markdown link targets, and text hygiene. CI runs the same command without root privileges or network access. It does not verify remote URLs, claim scientific completeness, compile BPF, or establish runtime safety.

Planned tests after approval include fixed-point reference equivalence, saturation/overflow, task lifecycle and censoring, map capacity, training determinism, partial-switch behavior, loader death, sched_ext watchdog fallback, affinity constraints, fairness, and starvation. Tests must exercise meaningful failure cases. Privileged checks belong in disposable QEMU guests.
