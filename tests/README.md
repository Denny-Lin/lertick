# Validation boundaries

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

Linux integration must separately prove compilation, verifier acceptance,
registration, actual task execution, completed burst labels, coefficient
updates, selected/observed slice agreement and default-scheduler recovery.
Partial switching, loader termination and watchdog tests belong in disposable
QEMU guests with an independent host control path. Map capacity, extended
affinity/migration stress, fairness and starvation require broader validation
beyond the small Phase 1 deployment check.

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
