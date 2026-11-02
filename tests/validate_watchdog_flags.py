#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check normal, expected-stall and unexpected-stall exit telemetry in QEMU."""
import argparse
import os
from pathlib import Path
import shlex
import signal
import subprocess
import time

from validate_linux import Validation, read_state, rows, wait_state


def stalled(validation, expected):
    name = "expected" if expected else "unexpected"
    command = shlex.join([str(validation.workload), "--role", "burst", "--duration", "1",
                          "--policy-ext", "--cpu", "0", "--burst-us", "1000", "--sleep-us", "2000"])
    worker = validation.launch(["/bin/bash", "-c", f"kill -STOP $$; exec {command}"], name + "-worker")
    deadline = time.monotonic() + 3
    while "State:\tT" not in Path(f"/proc/{worker.pid}/status").read_text():
        if time.monotonic() > deadline:
            raise RuntimeError("fault target did not stop")
        time.sleep(.01)
    extra = ["--partial", "--fault-stall", "--stall-tgid", str(worker.pid), "--dangerous-vm-test"]
    if expected:
        extra.append("--expect-watchdog")
    loader = validation.scheduler(name, mode="fixed", duration=10, extra=extra)
    worker.send_signal(signal.SIGCONT)
    wait_state("disabled", timeout=12)
    validation.finish(loader, expected=0 if expected else 1)
    validation.finish(worker)
    events = rows(validation.output / f"{name}.jsonl")
    end = events[-1]
    assert end["watchdog_expected"] is expected
    assert end["watchdog_triggered"] is True
    assert end["watchdog_observed"] is expected
    assert end["unexpected_fallback"] is (not expected)
    assert end["default_restored"] and end["status"] == (0 if expected else 1)
    final = next(row for row in reversed(events) if row.get("phase") == "final")
    assert final["stats"]["fault_stalls"] > 0
    validation.result["tests"][name] = {"passed": True, "intentional_fault": True, "exit": end}
    validation.save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert os.uname().sysname == "Linux" and os.geteuid() == 0
    assert subprocess.check_output(["systemd-detect-virt", "--vm"], text=True).strip() in ("qemu", "kvm")
    assert read_state() == "disabled", "refusing an existing scheduler"
    args.output.mkdir(parents=True, exist_ok=False)
    validation = Validation(args)
    try:
        loader = validation.scheduler("normal", mode="fixed", duration=2)
        validation.finish(loader)
        end = rows(validation.output / "normal.jsonl")[-1]
        assert all(end[field] is False for field in ("watchdog_expected", "watchdog_triggered", "watchdog_observed", "unexpected_fallback"))
        assert end["default_restored"] and end["status"] == 0
        validation.result["tests"]["normal"] = {"passed": True, "exit": end}
        stalled(validation, True)
        stalled(validation, False)
        validation.result.update(completed=True, final_state=read_state())
        validation.save()
        print("PASS normal/expected/unexpected watchdog flags and unchanged statuses", flush=True)
    except BaseException as error:
        validation.result.update(completed=False, failure=repr(error))
        validation.save()
        raise
    finally:
        validation.cleanup()


if __name__ == "__main__":
    main()
