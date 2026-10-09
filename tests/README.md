# Validation boundaries

Run `python3 tests/check_docs.py` from any working directory. It checks required planning files, local Markdown link targets, and text hygiene. CI runs the same command without root privileges or network access. It does not verify remote URLs, claim scientific completeness, compile BPF, or establish runtime safety.

The original host model test runs on macOS or Linux:

```sh
make test-model
# Or run with arithmetic and memory instrumentation:
clang -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
  -I src/shared tests/test_model.c -o /tmp/scxsprout-test-model
/tmp/scxsprout-test-model
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
