#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run deployment evidence and short comparisons inside an expendable Linux VM."""

import argparse
import json
import os
from pathlib import Path
import random
import shlex
import signal
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = Path("/sys/kernel/sched_ext/state")
OPS = Path("/sys/kernel/sched_ext/root/ops")


def read_state():
    return STATE.read_text().strip()


def wait_state(wanted, process=None, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if read_state() == wanted:
            return
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"loader exited {process.returncode} before state {wanted}")
        time.sleep(0.05)
    raise RuntimeError(f"state is {read_state()}, expected {wanted}")


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


class Validation:
    def __init__(self, args):
        self.args = args
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.loader = (ROOT / "build/scx_lertick").resolve()
        self.workload = (ROOT / "build/burst_workload").resolve()
        self.owned = []
        self.handles = []
        self.commands = []
        self.result = {"kernel": os.uname().release, "architecture": os.uname().machine,
                       "tests": {}, "comparison": [], "performance_claim": False}

    def launch(self, command, name):
        self.commands.append(shlex.join(str(value) for value in command))
        handle = (self.output / f"{name}.log").open("w")
        self.handles.append(handle)
        process = subprocess.Popen([str(value) for value in command], stdout=handle,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        self.owned.append(process)
        return process

    def finish(self, process, expected=0, timeout=15):
        code = process.wait(timeout=timeout)
        if expected is not None and code != expected:
            raise RuntimeError(f"process {process.args} exited {code}, expected {expected}")
        return code

    def scheduler(self, name, mode="online", duration=30, extra=()):
        if read_state() != "disabled":
            raise RuntimeError("Refusing to replace an already active sched_ext scheduler")
        process = self.launch([self.loader, "--mode", mode, "--duration", str(duration),
                               "--sample-ms", "250", "--stats-json",
                               self.output / f"{name}.jsonl", *extra], name)
        wait_state("enabled", process)
        if OPS.read_text().strip() != "scx_lertick":
            raise RuntimeError("registered ops do not match scx_lertick")
        return process

    def run_workload(self, name, role="burst", duration=4, phase=False, ext=False):
        command = [self.workload, "--duration", str(duration), "--role", role,
                   "--cpu", "0", "--burst-us", "1000", "--sleep-us", "2000"]
        if phase:
            command += ["--phase-burst-us", "6000"]
        if ext:
            command += ["--policy-ext"]
        return self.launch(command, name)

    def stop(self, process, sig=signal.SIGTERM, expected=0):
        process.send_signal(sig)
        self.finish(process, expected=expected)
        wait_state("disabled")

    def learning(self):
        for mode in ("fixed", "shadow", "online"):
            name = f"evidence-{mode}"
            loader = self.scheduler(name, mode=mode, duration=15)
            worker = self.run_workload(name + "-workload", duration=6, phase=True)
            self.finish(worker)
            self.stop(loader)
            events = rows(self.output / f"{name}.jsonl")
            assert any(event.get("verifier_accepted") for event in events)
            assert any(event.get("event") == "registered" for event in events)
            final = next(event for event in reversed(events) if event.get("phase") == "final")
            task = next(task for task in final["tasks"] if task["comm"] == "lertick_burst")
            assert task["runs"] > 0 and task["total_runtime_ns"] > 0
            assert task["completed_bursts"] > 50
            assert task["matching_dispatch_slices"] > 0
            assert final["stats"]["counter_errors"] == 0
            assert final["stats"]["alloc_failures"] == 0
            assert events[-1]["default_restored"]
            if mode == "fixed":
                assert task["training_updates"] == 0 and task["adaptive_slices"] == 0
                assert task["min_selected_slice_ns"] == task["max_selected_slice_ns"] == 2000000
            else:
                assert task["training_updates"] > 50
                assert task["training_updates"] == task["completed_bursts"]
                assert task["inferences"] >= task["training_updates"]
                assert task["model_weight_q16"] != [16384, 0, 0, 0, 0]
                if mode == "shadow":
                    assert task["adaptive_slices"] == 0
                    assert task["min_selected_slice_ns"] == task["max_selected_slice_ns"] == 2000000
                else:
                    assert task["adaptive_slices"] > 0 and task["adaptive_runs"] > 0
                    assert 500000 <= task["min_adaptive_slice_ns"] <= task["max_adaptive_slice_ns"] <= 5000000
                    assert task["slice_changes"] > 0
                    history = [task for event in events for task in event.get("tasks", [])
                               if task["comm"] == "lertick_burst"]
                    assert any(task["selected_slice_ns"] == task["observed_running_slice_ns"] != 2000000
                               and 500000 <= task["observed_running_slice_ns"] <= 5000000
                               and task["training_updates"] >= 8 and task["adaptive_runs"] > 0
                               for task in history)
            self.result["tests"][mode] = {"passed": True, "task": task,
                                            "stats": final["stats"]}
            self.save()
            print(f"PASS {mode}: runs={task['runs']} updates={task['training_updates']} "
                  f"adaptive_runs={task['adaptive_runs']}", flush=True)

    def recovery(self):
        for label, sig in (("sigint", signal.SIGINT), ("sigterm", signal.SIGTERM),
                           ("sigkill", signal.SIGKILL)):
            loader = self.scheduler(f"recovery-{label}", mode="fixed")
            worker = self.run_workload(f"recovery-{label}-workload", duration=1)
            self.finish(worker)
            start = time.monotonic()
            self.stop(loader, sig, expected=-signal.SIGKILL if sig == signal.SIGKILL else 0)
            self.result["tests"][label] = {"passed": True, "state": read_state(),
                                          "recovery_seconds": time.monotonic() - start}
            self.save()
            print(f"PASS {label} restoration", flush=True)

        loader = self.scheduler("recovery-sysrq", mode="fixed")
        start = time.monotonic()
        Path("/proc/sysrq-trigger").write_text("S")
        wait_state("disabled")
        code = self.finish(loader, expected=None)
        events = rows(self.output / "recovery-sysrq.jsonl")
        exit_event = events[-1]
        assert exit_event["default_restored"] and "sysrq" in exit_event["reason"].lower()
        self.result["tests"]["sysrq"] = {"passed": True, "returncode": code,
            "recovery_seconds": time.monotonic() - start, "exit": exit_event}
        self.save()
        print("PASS explicit SysRq-S recovery", flush=True)

        # Create a stopped process before loading so the exact fault TGID is known.
        # It opts into SCHED_EXT only after SIGCONT. SSH and the runner remain fair-class.
        # The burst worker genuinely sleeps after calibration and wakes again.
        # A never-blocking current task may be kept running by the kernel even
        # when dispatch finds no task, so it cannot establish a watchdog stall.
        workload_command = shlex.join([str(self.workload), "--role", "burst", "--duration", "1",
                                      "--policy-ext", "--cpu", "0", "--burst-us", "1000",
                                      "--sleep-us", "2000"])
        worker = self.launch(["/bin/bash", "-c", f"kill -STOP $$; exec {workload_command}"],
                             "recovery-watchdog-workload")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            status = Path(f"/proc/{worker.pid}/status").read_text()
            if "State:\tT" in status:
                break
            time.sleep(0.01)
        else:
            raise RuntimeError("watchdog target did not enter stopped state")
        loader = self.scheduler("recovery-watchdog", mode="fixed", duration=10,
            extra=("--partial", "--fault-stall", "--stall-tgid", str(worker.pid),
                   "--dangerous-vm-test", "--expect-watchdog"))
        start = time.monotonic()
        worker.send_signal(signal.SIGCONT)
        wait_state("disabled", timeout=12)
        self.finish(loader)
        self.finish(worker)
        events = rows(self.output / "recovery-watchdog.jsonl")
        assert events[-1]["watchdog_observed"] and events[-1]["default_restored"]
        final = next(event for event in reversed(events) if event.get("phase") == "final")
        assert final["stats"]["fault_stalls"] > 0
        self.result["tests"]["watchdog"] = {"passed": True, "target_pid": worker.pid,
            "recovery_seconds": time.monotonic() - start, "exit": events[-1],
            "stats": final["stats"]}
        self.save()
        print("PASS real stalled-task watchdog fallback", flush=True)

    def compare(self):
        rng = random.Random(20261009)
        for repetition in range(self.args.repetitions):
            policies = ["default", "scx_simple", "fixed", "online"]
            rng.shuffle(policies)
            for policy in policies:
                name = f"comparison-{repetition}-{policy}"
                loader = None
                if policy == "scx_simple":
                    if not self.args.reference:
                        raise RuntimeError("--reference must identify built official scx_simple")
                    loader = self.launch([self.args.reference.resolve()], name + "-scheduler")
                    wait_state("enabled", loader)
                    assert OPS.read_text().strip() == "simple"
                elif policy != "default":
                    loader = self.scheduler(name + "-scheduler", mode=policy,
                                            duration=self.args.seconds + 15)
                assert read_state() == ("disabled" if policy == "default" else "enabled")
                cpus = [self.run_workload(name + f"-cpu{index}", role="cpu",
                                         duration=self.args.seconds + 1) for index in range(2)]
                worker = self.run_workload(name + "-burst", duration=self.args.seconds)
                self.finish(worker)
                for process in cpus:
                    self.finish(process)
                if loader:
                    self.stop(loader)
                    if policy in ("fixed", "online"):
                        events = rows(self.output / f"{name}-scheduler.jsonl")
                        final = next(event for event in reversed(events) if event.get("phase") == "final")
                        controls = [task for task in final["tasks"] if task["comm"] == "lertick_cpu"]
                        assert len(controls) == 2
                        assert all(task["runs"] > 0 and task["total_runtime_ns"] > 0 for task in controls)
                        assert all(task["completed_bursts"] == 0 and task["training_updates"] == 0
                                   for task in controls), "CPU-only controls produced completed training labels"
                data = rows(self.output / f"{name}-burst.log")
                summary = next(event for event in data if event["event"] == "summary")
                samples = [event for event in data if event["event"] == "sample"]
                assert len(samples) > 20
                result = {"repetition": repetition, "policy": policy,
                    "duration_seconds": self.args.seconds, "samples": len(samples),
                    "burst_cpu_ns_mean": statistics.mean(event["cpu_ns"] for event in samples),
                    "completion_ns_p95": percentile([event["completion_ns"] for event in samples], 0.95),
                    "completion_ns_p99": percentile([event["completion_ns"] for event in samples], 0.99),
                    "throughput_units_per_s": summary["throughput_units_per_s"],
                    "default_restored": read_state() == "disabled"}
                self.result["comparison"].append(result)
                self.save()
                print(f"MEASURE {policy} repetition={repetition} samples={len(samples)}", flush=True)

    def save(self):
        (self.output / "validation.json").write_text(json.dumps(self.result, indent=2) + "\n")
        (self.output / "commands.txt").write_text("\n".join(self.commands) + "\n")

    def cleanup(self):
        for process in reversed(self.owned):
            if process.poll() is None:
                # Only children started by this runner, including a stopped watchdog child.
                os.killpg(process.pid, signal.SIGCONT)
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
        for handle in self.handles:
            handle.close()

    def execute(self):
        assert os.uname().sysname == "Linux" and os.geteuid() == 0
        assert read_state() == "disabled", "Refusing an existing scheduler"
        try:
            probe = self.launch([self.loader, "--check"], "preflight")
            self.finish(probe)
            self.result["tests"]["preflight"] = {"passed": True}
            self.learning()
            self.recovery()
            self.compare()
            self.result["completed"] = True
            self.result["final_state"] = read_state()
            self.save()
        except BaseException as error:
            self.result["completed"] = False
            self.result["failure"] = str(error)
            self.save()
            raise
        finally:
            self.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "results/local/phase1")
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--seconds", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.repetitions <= 20 or not 2 <= args.seconds <= 60:
        parser.error("repetitions must be 1..20; seconds must be 2..60")
    Validation(args).execute()


if __name__ == "__main__":
    main()
