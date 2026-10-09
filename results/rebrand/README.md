# Lertick branding regression evidence

These are new actual Linux records for the renamed `scx_lertick` implementation,
recorded on 2026-10-09. They supplement the unchanged `results/phase1/` archive.
See [the rebranding report](../../docs/REBRAND_VALIDATION.md) for checks and limits.

- `build.log`: actual fresh BTF/BPF/skeleton/loader/workload build and model tests.
- `validation.json`, `commands.txt`, `regression.log`: complete assertions and exact execution.
- `raw-evidence.tar.gz`: all per-mode scheduler/task JSONL and recovery/comparison logs.
- `online-verifier.log.gz`, `online-verifier.jsonl`: actual accepted online-mode programs and unload.
- `environment.txt`: running Linux/kernel configuration and live BTF/config hashes.
- `source-and-binary-sha256.txt`: hashes of tested implementation and artifacts.

The small comparisons are functional regression coverage. They establish no
performance improvement or cross-kernel support. All sched_ext attachments were
removed and the disposable VM was shut down after testing.
