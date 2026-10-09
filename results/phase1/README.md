# Phase 1 deployment evidence

Recorded on 2026-10-09 in an Ubuntu 24.04.5 ARM64 QEMU HVF guest running
`7.0.0-38-generic`. [The report](../../docs/PHASE1_RESULTS.md) interprets these
measurements; [the deployment guide](../../docs/DEPLOYMENT.md) reproduces them.
The files contain actual measurements, not replay or simulated scheduling.

| File | Evidence |
| --- | --- |
| `validation.json` | Assertions, learning counters, recovery reasons, 12 comparison runs, final disabled state |
| `raw-evidence.tar.gz` | All scheduler JSONL, real workload JSONL, loader/control logs, and commands from the final full suite |
| `commands.txt` | Exact argv launched by that suite, including the intentional stopped watchdog child |
| `environment.txt` | Running kernel, required config flags, config/BTF hashes, packages, CPUs, QEMU detection |
| `host.json` | Host version, actual acceleration, resources, and launch options |
| `image-sha256.txt` | Verified pinned Ubuntu cloud-image digest |
| `final-build.log` | Actual clean regeneration and compilation of BTF header, BPF object, skeleton, loader, workload, and arithmetic tests |
| `online-verifier.log.gz` | Successful online-mode libbpf/verifier diagnostics for the final implementation |
| `online-verifier.jsonl` | Its acceptance, registration, actual system-task callbacks, and finite unload |
| `source-and-binary-sha256.txt` | Hashes tying final tested code and binaries to the evidence |
| `qemu-helper-smoke.jsonl` | Real load, callbacks, finite detach, and disabled state after helper prepare/HVF restart |

The verbose verifier smoke run has no named learning workload; training and
prediction evidence comes from the separate full suite. Per-CPU counters and
live task snapshots are approximate; final snapshots are taken after detach.
Task names and numeric PIDs in these files belong to our generated workloads.
No VM disks, SSH private keys, guest console, or unrelated personal task data
are included.

Inspect raw records without booting Linux:

```sh
tar -tzf results/phase1/raw-evidence.tar.gz
tar -xOzf results/phase1/raw-evidence.tar.gz ./evidence-online.jsonl
tar -xOzf results/phase1/raw-evidence.tar.gz ./recovery-watchdog.jsonl
gzip -dc results/phase1/online-verifier.log.gz | rg 'processed .* insns'
python3 -m json.tool results/phase1/validation.json
```

The baseline values are medians of three per-run percentiles, not percentiles
of pooled samples. Every burst worker and both CPU competitors were pinned to
guest CPU 0. Trial duration and the VM environment sharply limit inference;
these data establish functional comparison, not a general performance claim.
