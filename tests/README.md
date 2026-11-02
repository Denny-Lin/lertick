# Validation boundaries

`make test-deployment` checks private telemetry output, controller/maintenance
lock lifetime, source-build fingerprints and the deployment helper using
temporary files and stub tools. It never loads BPF or changes host scheduling.
It is part of the Ubuntu and musl CI jobs.

On a target Linux machine with the native loader already built:

```sh
python3 tests/test_loader_deployment.py --loader build/scx_lertick
sudo python3 tests/test_loader_deployment.py --loader build/scx_lertick --live
```

The default invocation checks CLI and unsafe output rejection only. `--live`
also verifies the BPF programs and maintenance lock without attaching. In an
idle disposable QEMU/KVM VM, explicitly add `--allow-attach` for short signal,
loader-death, all-task-learning and telemetry-failure recovery checks. That
flag refuses an active scheduler or a non-VM host. Ordinary hardware users
should use the finite foreground trial in the [native guide](../docs/INSTALL.md).

`make test-burst-profiles` checks the optional deterministic burst workloads'
phase boundaries, seed vectors and unsigned-width bounds. It is an input
fixture test, not a model-accuracy result. `python3 tests/test_confirmation.py`
tests that the confirmation controller refuses active schedulers and restores
preemption on child or write errors without touching real kernel settings.

Run `python3 tests/check_docs.py` from any working directory. It checks required planning files, local Markdown link targets, and text hygiene. CI runs the same command without root privileges or network access. It does not verify remote URLs, claim scientific completeness, compile BPF, or establish runtime safety.

The original host model test runs on macOS or Linux:

```sh
make test-model
# Or run with arithmetic and memory instrumentation:
clang -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  -I src/shared tests/test_model.c -o /tmp/lertick-test-model
/tmp/lertick-test-model
```

It checks 100,000 extreme-input prediction/training cases against an independent
128-bit arithmetic oracle, full-width nanosecond normalization against a 128-bit
reference, deterministic initial updates, adaptation to a changed target,
recovery from prediction saturation, and learning on a synthetic signal with
held-out inputs. Synthetic learning establishes arithmetic behavior only; it
does not establish real-kernel burst labels or scheduling benefit.

`make test-cpuperf` checks the frequency-target arithmetic against 128-bit and
floating-point references and runs a closed loop against a schedutil model (25%
headroom, frequency invariance): from low or high frequency, a steady load
settles at 80% busy without oscillating. `make test-lerwatt` builds fake sysfs
trees for cluster discovery and checks demand aggregation and the seqcount
writer against Lerwatt's reader rule. Neither runs BPF; verifier acceptance of
`--cpuperf util` and `--lerwatt` must be shown in the guest
([deployment steps](../docs/DEPLOYMENT.md#cpu-frequency-targets-and-the-lerwatt-plug-in)).

Linux integration must separately prove compilation, verifier acceptance,
registration, actual task execution, completed burst labels, coefficient
updates, selected/observed slice agreement and default-scheduler recovery.
Partial switching, loader termination and watchdog tests belong in disposable
QEMU guests with an independent host control path. Map capacity, extended
affinity/migration stress, fairness and starvation require broader validation
beyond the small Phase 1 deployment check.

Exit telemetry reports `watchdog_triggered` whenever the kernel reports a
runnable-task stall, and `watchdog_expected` when the loader was explicitly
started with `--expect-watchdog`. The legacy `watchdog_observed` field means
that an **expected** watchdog was observed. An unexpected stall therefore
has `watchdog_triggered: true`, `watchdog_observed: false`,
`unexpected_fallback: true`, and a nonzero loader status. Existing status,
recovery and legacy-field behavior are preserved.
`watchdog_triggered` is null if the running kernel does not expose the stall
exit enum; this optional classification does not prevent an ordinary run.

In an isolated QEMU/KVM guest, `sudo python3 tests/validate_watchdog_flags.py
--output results/local/watchdog-flags-new` checks normal shutdown and both
expected/unexpected intentional stalls, including their distinct loader
statuses. It refuses an existing scheduler and retains every log.
See the [recorded exit and eight-vCPU plug-in checks](../results/watchdog-status/README.md)
for actual results, including the failed collector attempt and complete rerun.

Phase 2 adds independent service arithmetic and actual Linux lifecycle tests:

```sh
make test-accounting
clang -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  -I src/shared tests/test_accounting.c -o /tmp/lertick-test-accounting
/tmp/lertick-test-accounting
# Inside the disposable Linux guest, after make all:
printf full | sudo tee /sys/kernel/debug/sched/preempt
sudo python3 tests/validate_phase2.py --output results/local/phase2a
printf lazy | sudo tee /sys/kernel/debug/sched/preempt
```

The recorded guest's original selected mode is `lazy`; preserve the original
mode when reproducing elsewhere. Concurrent fork stress requires selected
full preemption and verifies 8,000 independently completed child tasks.

The accounting test checks 202,000 cases against an independent 128-bit oracle
and callback-order traces. The Linux suite compares task service against both
thread CPU clocks and procfs, captures continuation decisions, checks lifecycle
and map ownership, tests finite fairness, and repeats five shutdown/recovery
mechanisms. See [the correctness boundaries](../docs/PHASE2_CORRECTNESS.md) and
[actual results](../docs/PHASE2_RESULTS.md). Portable tests cannot establish
verifier acceptance, registration or execution on Linux. CPU performance and
ML overhead require the separate [repeated benchmark protocol](../benchmarks/README.md).
