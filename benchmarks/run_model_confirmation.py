#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Longer model comparisons in an isolated QEMU guest; always restore preemption.

Each environment is its own randomized five-repetition experiment. Environment
order alone does not establish a causal preemption effect. No files are deleted.
Use separate fresh outputs when offloading completed sessions to the host.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = Path("/sys/kernel/sched_ext/state")
PREEMPT = Path("/sys/kernel/debug/sched/preempt")
STAGES = {
    "stable-shadow": {"workloads": ["shortburst", "wakeheavy", "mixed", "multicore"],
                      "policies": ["shadow", "shadow-ewma-carry"], "modes": ["full", "lazy"]},
    "heldout-shadow": {"workloads": ["stepburst", "alternatingburst", "rampburst", "randomstepburst"],
                       "policies": ["shadow", "shadow-ewma-carry"], "modes": ["full", "lazy"]},
    "online-controls": {"workloads": ["shortburst", "wakeheavy", "mixed", "multicore"],
                        "policies": ["fixed", "ewma", "online", "online-ewma-carry"], "modes": ["lazy"]},
    "observer-lazy": {"workloads": ["wakeheavy", "multicore"],
                      "policies": ["shadow", "shadow-ewma-carry"], "modes": ["lazy"]},
}


def selected(text):
    match = re.search(r"\(([^)]+)\)", text)
    if not match:
        raise ValueError("cannot identify selected kernel preemption mode")
    return match[1]


@contextmanager
def temporary_preemption(mode, preempt=PREEMPT, state=STATE):
    if state.read_text().strip() != "disabled":
        raise RuntimeError("refusing a preemption change with an active scheduler")
    original = selected(preempt.read_text())
    try:
        if mode != original:
            preempt.write_text(mode + "\n")
        if selected(preempt.read_text()) != mode:
            raise RuntimeError("kernel did not select requested preemption")
        yield original
    finally:
        deadline = time.monotonic() + 15
        while state.read_text().strip() != "disabled" and time.monotonic() < deadline:
            time.sleep(.05)
        if state.read_text().strip() != "disabled":
            raise RuntimeError("scheduler is still active; refusing to change preemption under it")
        if selected(preempt.read_text()) != original:
            preempt.write_text(original + "\n")
        if selected(preempt.read_text()) != original:
            raise RuntimeError("original preemption mode was not restored")


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def plan(args):
    stage = STAGES[args.stage]
    workloads = args.workloads.split(",") if args.workloads else stage["workloads"][:]
    modes = args.modes.split(",") if args.modes else stage["modes"][:]
    if not workloads or len(set(workloads)) != len(workloads) or set(workloads) - set(stage["workloads"]):
        raise ValueError("unknown or duplicate workload for this stage")
    if not modes or len(set(modes)) != len(modes) or set(modes) - {"full", "lazy"}:
        raise ValueError("unknown or duplicate preemption mode")
    if args.stage in ("online-controls", "observer-lazy") and modes != ["lazy"]:
        raise ValueError("online/observer confirmation is declared for the original lazy environment")
    random.Random(args.seed).shuffle(modes)
    prefaults = [False, True] if args.stage == "observer-lazy" else [True]
    return [{"mode": mode, "prefault_samples": prefault, "workloads": workloads,
             "policies": stage["policies"], "repetitions": 5, "duration_s": 30, "warmup_s": 5,
             "order_seed": args.seed, "burst_seed": args.burst_seed}
            for mode in modes for prefault in prefaults]


def other_power_experiments():
    found = []
    for directory in Path("/proc").iterdir():
        if not directory.name.isdigit():
            continue
        try:
            parts = (directory / "cmdline").read_bytes().split(b"\0")
            name = Path(os.fsdecode(parts[0])).name if parts[0] else ""
        except (OSError, IndexError):
            continue
        if name in ("lerwattd", "lerwatt-sim"):
            found.append(int(directory.name))
    return found


def interrupt(_signal, _frame):
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, signal.SIG_IGN)
    raise KeyboardInterrupt


def stop_owned(process, directory):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            # Only this fresh experiment's redirected children may be stopped.
            for entry in Path("/proc").iterdir():
                if not entry.name.isdigit():
                    continue
                try:
                    argv = (entry / "cmdline").read_bytes().split(b"\0")
                    name = Path(os.fsdecode(argv[0])).name if argv[0] else ""
                    output = os.readlink(entry / "fd/1")
                    if name in ("scx_lertick", "phase2_workload", "bpftrace") and output.startswith(str(directory) + "/"):
                        os.kill(int(entry.name), signal.SIGTERM)
                except (OSError, IndexError):
                    continue
            process.kill()
            process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=STAGES, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--modes")
    parser.add_argument("--workloads")
    parser.add_argument("--seed", type=int, default=2026110201)
    parser.add_argument("--burst-seed", type=int, default=2026110211)
    parser.add_argument("--print-plan", action="store_true")
    args = parser.parse_args()
    sessions = plan(args)
    if args.print_plan:
        print(json.dumps(sessions, indent=2))
        return
    if not args.output or os.uname().sysname != "Linux" or os.geteuid() != 0:
        parser.error("execution requires Linux root and a fresh --output")
    vm = subprocess.check_output(["systemd-detect-virt", "--vm"], text=True).strip()
    if vm not in ("qemu", "kvm"):
        parser.error("preemption confirmation is limited to an isolated QEMU/KVM guest")
    if other_power_experiments():
        parser.error("refusing concurrent lerwattd/lerwatt-sim experiments")
    if STATE.read_text().strip() != "disabled":
        parser.error("refusing an existing sched_ext scheduler")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = ["src/shared/model.h", "src/bpf/scx_lertick.bpf.c", "src/userspace/scx_lertick.c",
               "benchmarks/phase2_workload.c", "benchmarks/burst_profiles.h", "benchmarks/run_phase2.py",
               "benchmarks/run_model_confirmation.py", "build/scx_lertick", "build/phase2_workload"]
    hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources}
    evidence = {"started_unix": time.time(), "kernel": os.uname().release, "virtualization": vm,
                "original_preemption": PREEMPT.read_text(), "sessions": sessions, "sha256": hashes,
                "completed": False, "performance_claim": False}
    save(output / "confirmation.json", evidence)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, interrupt)
    try:
        for session in sessions:
            name = session["mode"] + ("-prefault" if session["prefault_samples"] else "-demand")
            destination = output / name
            if {key: hashlib.sha256((ROOT / key).read_bytes()).hexdigest() for key in hashes} != hashes:
                raise RuntimeError("source/binary changed after confirmation began")
            command = [sys.executable, str(ROOT / "benchmarks/run_phase2.py"), "--output", str(destination),
                       "--policies", ",".join(session["policies"]), "--workloads", ",".join(session["workloads"]),
                       "--repetitions", "5", "--duration", "30", "--warmup", "5", "--cpus", "0,1,2,3",
                       "--seed", str(args.seed), "--burst-seed", str(args.burst_seed)]
            if session["prefault_samples"]:
                command.append("--prefault-samples")
            print(f"START {name}: {args.stage}", flush=True)
            with temporary_preemption(session["mode"]):
                process = subprocess.Popen(command, cwd=ROOT)
                try:
                    code = process.wait()
                finally:
                    stop_owned(process, destination)
                if code:
                    raise RuntimeError(f"benchmark exited {code}; preserve {destination}")
            print(f"PASS {name}; restored {selected(PREEMPT.read_text())}", flush=True)
        evidence["completed"] = True
    except BaseException as error:
        evidence["failure"] = repr(error)
        raise
    finally:
        evidence.update(finished_unix=time.time(), final_preemption=PREEMPT.read_text(), final_state=STATE.read_text().strip())
        save(output / "confirmation.json", evidence)


if __name__ == "__main__":
    main()
