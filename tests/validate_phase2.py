#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 2A correctness, lifecycle, fairness and repeated recovery in a Linux VM.

Reuses Phase 1 process ownership and VM-guarded recovery routines. All service
oracles below come from the workload's CPU clock and procfs, not BPF predictions.
"""

import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

from validate_linux import ROOT, Validation, read_state, rows


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def snapshots(path):
    """Ignore a concurrently written final partial line, retain complete records."""
    if not path.exists():
        return []
    text = path.read_text()
    lines = text.splitlines()
    if text and not text.endswith("\n"):
        lines = lines[:-1]
    return [json.loads(line) for line in lines if line.strip()]


def latest_snapshot(path):
    events = [event for event in snapshots(path) if event.get("event") == "snapshot"]
    require(events, f"no scheduler snapshots in {path}")
    return events[-1]


class Phase2Validation(Validation):
    def __init__(self, args):
        super().__init__(args)
        self.lifecycle_binary = self.output / "lifecycle_workload"
        self.cpu_ids = sorted(os.sched_getaffinity(0))[:4]
        require(len(self.cpu_ids) == 4,
                "Phase 2A fairness protocol requires four available Linux CPUs")
        self.cpu_text = ",".join(str(cpu) for cpu in self.cpu_ids)
        self.result.update(phase="2A", cpu_ids=self.cpu_ids,
                           accuracy_tolerance="max(20ms, 2% of independent CPU service)",
                           procfs_tolerance="max(5ms, 1% of independent CPU service)",
                           lifecycle=[], fairness=[], recovery_repetitions=[])

    def build_workload(self):
        process = self.launch(["cc", "-O2", "-std=gnu11", "-Wall", "-Wextra",
                               "-Werror", "-pthread", ROOT / "tests/lifecycle_workload.c",
                               "-o", self.lifecycle_binary], "lifecycle-build")
        self.finish(process)
        self.result["tests"]["lifecycle_build"] = {"passed": True}
        self.save()

    def start_lifecycle(self, name, scenario, duration=2, workers=8, iterations=32, index=0):
        return self.launch([self.lifecycle_binary, "--scenario", scenario,
                            "--duration", str(duration), "--workers", str(workers),
                            "--iterations", str(iterations), "--index", str(index),
                            "--cpus", self.cpu_text], name)

    def check_counters(self, final, context):
        stats = final["stats"]
        for counter in ("counter_errors", "alloc_failures", "snapshot_failures"):
            require(stats[counter] == 0, f"{context}: {counter}={stats[counter]}")
        require(len(final["tasks"]) <= 4096, f"{context}: retained task map exceeds bound")
        require(stats["live_tasks"] == 0, f"{context}: live task storage remains after detach")
        require(stats["init_tasks"] == stats["exit_tasks"],
                f"{context}: init/exit accounting does not balance after detach")

    def owned_maps(self, context, loader):
        command = ["bpftool", "-j", "map", "show"]
        self.commands.append(" ".join(command))
        maps = json.loads(subprocess.check_output(command, text=True))
        # Persist the complete metadata before any assertion. Names alone are
        # not ownership: another loaded but unattached object can use the same
        # names. The loader's map/program FDs establish precise ownership.
        map_fds, prog_fds = set(), set()
        for path in Path(f"/proc/{loader.pid}/fdinfo").iterdir():
            try:
                content = path.read_text()
            except FileNotFoundError:
                continue  # An unrelated telemetry/file FD may close concurrently.
            for key, value in re.findall(r"^(map_id|prog_id):\s*(\d+)\s*$", content, re.MULTILINE):
                (map_fds if key == "map_id" else prog_fds).add(int(value))
        metadata = {"loader_pid": loader.pid, "map_fd_ids": sorted(map_fds),
                    "program_fd_ids": sorted(prog_fds), "maps": maps}
        evidence = self.output / f"{context}-maps-before.json"
        evidence.write_text(json.dumps(metadata, indent=2) + "\n")
        ops = [entry for entry in maps if entry.get("name") == "lertick_ops"
               and entry["id"] in map_fds]
        require(len(ops) == 1, f"{context}: cannot identify this loader's unique struct_ops map; see {evidence}")
        require(ops[0]["type"] == "struct_ops", f"{context}: ops ownership has wrong map type")
        btf_id = ops[0].get("btf_id", 0)
        if btf_id:
            selected = [entry for entry in maps if entry.get("name") in
                        ("task_snapshots", "task_ctx_stor") and entry.get("btf_id") == btf_id
                        and entry["id"] in map_fds]
            metadata["ownership_method"] = "loader map FDs and struct_ops BTF ID"
            metadata["ops_btf_id"] = btf_id
        else:
            command = ["bpftool", "-j", "prog", "show"]
            self.commands.append(" ".join(command))
            programs = json.loads(subprocess.check_output(command, text=True))
            held = [program for program in programs if program["id"] in prog_fds]
            associated = {map_id for program in held for map_id in program.get("map_ids", [])}
            metadata["programs"] = programs
            metadata["ownership_method"] = "loader program FDs and program map_ids"
            metadata["associated_map_ids"] = sorted(associated)
            evidence.write_text(json.dumps(metadata, indent=2) + "\n")
            require(held and associated,
                    f"{context}: BTF unavailable and no owned program/map association; see {evidence}")
            selected = [entry for entry in maps if entry.get("name") in
                        ("task_snapshots", "task_ctx_stor") and entry["id"] in associated
                        and entry["id"] in map_fds]
        metadata["selected_map_ids"] = [entry["id"] for entry in selected]
        evidence.write_text(json.dumps(metadata, indent=2) + "\n")
        require(len(selected) == 2, f"{context}: cannot identify unique scheduler task maps")
        retained = next(entry for entry in selected if entry["name"] == "task_snapshots")
        require(retained["type"] == "lru_hash" and retained["max_entries"] == 4096,
                f"{context}: retained snapshots are not a bounded 4096-entry LRU map")
        return [{"id": entry["id"], "name": entry["name"], "type": entry["type"],
                 "max_entries": entry["max_entries"], "btf_id": entry.get("btf_id", 0)}
                for entry in selected]

    def maps_released(self, owned, context):
        command = ["bpftool", "-j", "map", "show"]
        owned_ids = {entry["id"] for entry in owned}
        started = time.monotonic()
        deadline = started + 5
        evidence = self.output / f"{context}-maps-after.json"
        inventories = {"timeout_seconds": 5, "owned_map_ids": sorted(owned_ids),
                       "initial": None, "final": None, "polls": 0}
        # Closing the loader's FDs and struct_ops attachment need not destroy
        # maps synchronously. Kernel program/map references can drain through
        # deferred (RCU) cleanup. Bound that drain; never accept a persistent map.
        while True:
            if inventories["polls"] and time.monotonic() >= deadline:
                inventories["elapsed_seconds"] = time.monotonic() - started
                evidence.write_text(json.dumps(inventories, indent=2) + "\n")
                break
            self.commands.append(" ".join(command))
            remaining_time = max(0.001, deadline - time.monotonic())
            metadata = json.loads(subprocess.check_output(command, text=True,
                                                         timeout=remaining_time))
            if inventories["initial"] is None:
                inventories["initial"] = metadata
            inventories["final"] = metadata
            inventories["polls"] += 1
            inventories["elapsed_seconds"] = time.monotonic() - started
            remaining = owned_ids.intersection(entry["id"] for entry in metadata)
            inventories["remaining_owned_ids"] = sorted(remaining)
            evidence.write_text(json.dumps(inventories, indent=2) + "\n")
            if not remaining or time.monotonic() >= deadline:
                break
            time.sleep(min(0.05, max(0, deadline - time.monotonic())))
        require(not remaining,
                f"{context}: owned task maps {sorted(remaining)} remain after bounded 5s drain; see {evidence}")
        return {"elapsed_seconds": inventories["elapsed_seconds"],
                "polls": inventories["polls"],
                "initially_remaining_owned_ids": sorted(owned_ids.intersection(
                    entry["id"] for entry in inventories["initial"])),
                "remaining_owned_ids": []}

    def compare_service(self, measured, task, context):
        cpu = measured["cpu_ns"]
        proc = measured["schedstat_ns"]
        bpf = task["total_runtime_ns"]
        require(cpu > 0 and proc > 0 and bpf > 0,
                f"{context}: zero independent or BPF service")
        proc_tolerance = max(5000000, cpu * 0.01)
        tolerance = max(20000000, cpu * 0.02)
        require(abs(cpu - proc) <= proc_tolerance,
                f"{context}: CPU clock {cpu} and proc schedstat {proc} differ excessively")
        require(abs(cpu - bpf) <= tolerance and abs(proc - bpf) <= tolerance,
                f"{context}: BPF {bpf} disagrees with CPU clock {cpu}/schedstat {proc}")
        require(task["exited"] == 1, f"{context}: exited workload retains live snapshot")
        require(task["runs"] > 0, f"{context}: workload did not execute through BPF")
        require(measured["affinity_violations"] == 0, f"{context}: CPU affinity violated")
        return {"tid": measured["tid"], "role": measured["role"],
                "clock_cpu_ns": cpu, "proc_schedstat_ns": proc, "bpf_runtime_ns": bpf,
                "bpf_absolute_error_ns": abs(cpu - bpf), "tolerance_ns": tolerance,
                "exited": True, "completed_bursts": task["completed_bursts"],
                "training_updates": task["training_updates"]}

    def validate_task_policy(self, task, mode, context, negative=False, setup_labels=0):
        require(500000 <= task["min_selected_slice_ns"] <=
                task["max_selected_slice_ns"] <= 5000000,
                f"{context}: newly selected slices violate bounds")
        if negative:
            require(task["completed_bursts"] <= setup_labels,
                    f"{context}: runnable/yield/exit task produced labels beyond setup")
            if setup_labels and task["completed_bursts"]:
                # A CPU pthread wakes from the barrier and then deliberately
                # sleeps for 1ms before the oracle interval. This is one REAL
                # wake-to-sleep label, with only a few microseconds of service.
                # A long CPU episode or additional label would be a failure.
                require(task["last_burst_ns"] < 500000 and task["max_burst_ns"] < 500000,
                        f"{context}: purported setup label contains substantial CPU service")
            expected_updates = task["completed_bursts"] if mode != "fixed" else 0
            require(task["training_updates"] == expected_updates,
                    f"{context}: runnable control trained beyond its real setup labels")
        if mode in ("fixed", "shadow"):
            require(task["min_selected_slice_ns"] == task["max_selected_slice_ns"] == 2000000,
                    f"{context}: fixed/shadow mode changed its slice")
            require(task["adaptive_slices"] == 0, f"{context}: unexpected adaptive slice")
        if mode == "fixed":
            require(task["training_updates"] == 0, f"{context}: fixed mode trained")
        elif not negative:
            require(task["training_updates"] == task["completed_bursts"],
                    f"{context}: one training update per label violated")

    def case(self, mode, scenario, duration=2, workers=8, iterations=32):
        name = f"lifecycle-{mode}-{scenario}"
        loader = self.scheduler(name, mode=mode, duration=120, extra=("--sample-ms", "1000"))
        owned_maps = self.owned_maps(name, loader)
        time.sleep(1.05)
        before = latest_snapshot(self.output / f"{name}.jsonl")
        process = self.start_lifecycle(name + "-workload", scenario, duration, workers, iterations)
        self.finish(process, timeout=90)
        time.sleep(1.05)
        drained = latest_snapshot(self.output / f"{name}.jsonl")
        self.stop(loader)
        map_release = self.maps_released(owned_maps, name)
        final = latest_snapshot(self.output / f"{name}.jsonl")
        self.check_counters(final, name)
        oracles = [event for event in rows(self.output / f"{name}-workload.log")
                   if event.get("event") == "worker"]
        expected = workers if scenario == "threads" else iterations if scenario in ("forkexec", "churn") else 1
        require(len(oracles) == expected, f"{name}: missing workload completions")
        tasks = {task["pid"]: task for task in final["tasks"]}
        result = {"mode": mode, "scenario": scenario, "passed": True,
                  "workers_completed": len(oracles), "retained_tasks": len(final["tasks"]),
                  "maps": owned_maps, "maps_released": True,
                  "map_release": map_release,
                  "live_before": before["stats"]["live_tasks"],
                  "live_drained": drained["stats"]["live_tasks"],
                  "final_stats": final["stats"], "service_checks": []}
        # Live totals include unrelated VM tasks. Exact target exit bits and final
        # init/exit balance provide the stronger no-leak check at attachment end.
        require(drained["stats"]["live_tasks"] <= before["stats"]["live_tasks"] + 16,
                f"{name}: live task count grew after workload drain")
        if scenario == "churn":
            lifecycle = next(event for event in rows(self.output / f"{name}-workload.log")
                             if event.get("event") == "lifecycle")
            require(lifecycle["created"] == lifecycle["reaped"] == iterations,
                    "fork/exec churn did not reap every child")
            require(len({event["tid"] for event in oracles}) > 4096,
                    "churn did not exceed snapshot-map capacity with distinct tasks")
            recent = [event for event in oracles[-workers:] if event["tid"] in tasks]
            require(len(recent) >= workers // 2, "LRU snapshots did not retain recent children")
            require(all(tasks[event["tid"]]["exited"] == 1 for event in recent),
                    "recent churn child snapshots did not record exit")
            result["retained_recent_children"] = len(recent)
            result["eviction_expected"] = True
        else:
            for measured in oracles:
                tid = measured["tid"]
                require(tid in tasks, f"{name}: no BPF evidence for TID {tid}")
                task = tasks[tid]
                context = f"{name}/TID{tid}"
                result["service_checks"].append(self.compare_service(measured, task, context))
                # Threaded CPU workers first block on a barrier. Their final
                # runnable episode must still be censored on exit, never labeled.
                # exec can contain genuine blocking loader/page-fault work
                # outside the oracle interval. Direct CPU/yield/exit paths and
                # synchronized CPU threads are the false-label controls.
                negative = measured["role"] in ("cpu", "yield", "affinity", "lateoptin") and scenario != "forkexec"
                setup_labels = 1 if scenario == "threads" and measured["role"] == "cpu" else 0
                self.validate_task_policy(task, mode, context, negative, setup_labels=setup_labels)
                if negative:
                    require(measured["sleeps"] == 0, f"{context}: CPU control slept inside oracle interval")
                if setup_labels:
                    result["service_checks"][-1].update(
                        setup_label_limit=1,
                        setup_label_cause="barrier wake followed by explicit pre-oracle 1ms sleep",
                        setup_burst_ns=task["last_burst_ns"])
                if measured["role"] == "yield":
                    require(measured["yields"] > 50, f"{context}: yield path not exercised")
                if measured["role"] == "affinity":
                    require(measured["affinity_changes"] > 20 and measured["migrations"] > 10,
                            f"{context}: forced affinity migration not exercised")
                if measured["role"] == "burst":
                    require(task["completed_bursts"] > 20,
                            f"{context}: concurrent sleeping/wakeup path lacks labels")
        self.result["lifecycle"].append(result)
        self.save()
        print(f"PASS {name}: completed={len(oracles)} retained={len(final['tasks'])}", flush=True)

    def check_continuation(self):
        name = "evidence-online"
        events = rows(self.output / f"{name}.jsonl")
        witnesses = []
        for event in events:
            for task in event.get("tasks", []):
                if task.get("comm") != "lertick_burst" or not task.get("continuation_slices"):
                    continue
                pending = task["continuing_pending_ns"]
                consumed = task["continuing_consumed_ns"]
                predicted = task["continuing_prediction_ns"]
                selected = task["continuing_selected_slice_ns"]
                if pending <= 0 or task["continuing_model_updates"] < 8 or not task["continuing_episode_valid"]:
                    continue
                expected = min(5000000, max(500000, predicted - consumed)) if predicted > consumed else 2000000
                require(consumed >= pending, "continuation pending service was not included")
                require(selected == expected,
                        f"continuation slice {selected} does not match service-aware expected {expected}")
                committed = consumed - pending
                legacy_expected = min(5000000, max(500000, predicted - committed)) if predicted > committed else 2000000
                witnesses.append({"tid": task["pid"], "pending_ns": pending,
                                  "consumed_ns": consumed, "prediction_ns": predicted,
                                  "selected_ns": selected, "expected_ns": expected,
                                  "committed_ns": committed,
                                  "legacy_expected_ns": legacy_expected,
                                  "pending_changed_slice": selected != legacy_expected})
        require(witnesses, "no real continuation with pending CPU service was observed")
        require(any(witness["pending_changed_slice"] for witness in witnesses),
                "pending service was observed but no selected slice differs from the legacy decision")
        self.result["tests"]["continuation_accounting"] = {"passed": True,
                                                          "witnesses": witnesses}
        self.save()
        print(f"PASS pending-service continuation accounting: {len(witnesses)} witnesses", flush=True)

    def lifecycle(self):
        # Reuse the actual phase-changing six-second labels and slice tests.
        self.learning()
        self.check_continuation()
        for mode in ("fixed", "shadow", "online"):
            for scenario, duration in (("cpu", 3), ("yield", 2), ("affinity", 2), ("lateoptin", 2),
                                       ("threads", 3), ("forkexec", 0.02)):
                self.case(mode, scenario, duration=duration)
        self.case("fixed", "churn", duration=0.0001, workers=8, iterations=4200)

    def concurrent_fork_churn(self):
        """Stress unlocked init_task callers sharing one CPU under preemption.

        Parent/child completion counts are independent of BPF counters. This
        complements the original single-parent churn case; LRU eviction means
        that observing all children through retained snapshots is not required.
        The caller controls the guest's preemption mode and preserves it in
        this evidence rather than silently changing kernel policy here.
        """
        name = "concurrent-fork-fixed"
        parent_count, children_per_parent, children_per_batch = 8, 1000, 8
        expected_children = parent_count * children_per_parent
        selected_cpu = self.cpu_ids[0]
        preemption_path = Path("/sys/kernel/debug/sched/preempt")
        try:
            preemption_mode = preemption_path.read_text().strip()
        except OSError:
            preemption_mode = None
        if preemption_path.exists():
            require(preemption_mode is not None and "(full)" in preemption_mode,
                    f"{name}: selected full preemption is required; observed {preemption_mode!r}. "
                    "Inside the disposable VM, select it with "
                    "`printf full | sudo tee /sys/kernel/debug/sched/preempt`, "
                    "rerun this test, then restore the previous mode. Bare `full` "
                    "in the available-mode list is not the selected mode.")
        loader = self.scheduler(name, mode="fixed", duration=240,
                                extra=("--sample-ms", "1000"))
        owned_maps = self.owned_maps(name, loader)
        time.sleep(1.05)
        before = latest_snapshot(self.output / f"{name}.jsonl")
        # Pin the parents as well as their children. The C fixture applies
        # --cpus to workers, while taskset constrains the actual fork callers.
        processes = [self.launch([
            "taskset", "--cpu-list", str(selected_cpu), self.lifecycle_binary,
            "--scenario", "churn", "--duration", "0.0001",
            "--workers", str(children_per_batch),
            "--iterations", str(children_per_parent), "--cpus", str(selected_cpu),
        ], name + f"-parent{index}") for index in range(parent_count)]
        active_parents = sum(process.poll() is None for process in processes)
        require(active_parents == parent_count,
                f"{name}: all eight fork callers did not overlap")
        deadline = time.monotonic() + 180
        for process in processes:
            self.finish(process, timeout=max(0.001, deadline - time.monotonic()))
        time.sleep(1.05)
        drained = latest_snapshot(self.output / f"{name}.jsonl")
        self.stop(loader)
        map_release = self.maps_released(owned_maps, name)
        final = latest_snapshot(self.output / f"{name}.jsonl")
        self.check_counters(final, name)
        require(final["stats"]["training_updates"] == 0 and
                final["stats"]["adaptive_slices"] == 0,
                f"{name}: fixed counter stress unexpectedly changed policy")
        parent_results, children = [], []
        for index in range(parent_count):
            events = rows(self.output / f"{name}-parent{index}.log")
            completions = [event for event in events if event.get("event") == "worker"]
            counts = [event for event in events if event.get("event") == "lifecycle"]
            require(len(counts) == 1 and
                    counts[0]["created"] == counts[0]["reaped"] == children_per_parent and
                    counts[0]["exec"] is True,
                    f"{name}: parent {index} did not create/exec/reap all children")
            require(len(completions) == children_per_parent,
                    f"{name}: parent {index} lacks independent child completion records")
            require(all(child["role"] == "cpu" and child["cpu_ns"] > 0 and
                        child["schedstat_ns"] > 0 and child["units"] > 0 and
                        child["affinity_violations"] == 0 for child in completions),
                    f"{name}: parent {index} has an invalid child execution record")
            children.extend(completions)
            parent_results.append({"parent_index": index, "pid": processes[index].pid,
                                   "created": counts[0]["created"],
                                   "reaped": counts[0]["reaped"],
                                   "completion_records": len(completions)})
        unique_children = {child["tid"] for child in children}
        require(len(children) == len(unique_children) == expected_children,
                f"{name}: expected 8000 distinct completed child tasks")
        # Every parent and child must create an association after the baseline.
        # Other VM tasks can also initialize; they cannot reduce this lower bound.
        initialized = final["stats"]["init_tasks"] - before["stats"]["init_tasks"]
        require(initialized >= expected_children + parent_count,
                f"{name}: init counter omitted known parent/child associations")
        tasks = {task["pid"]: task for task in final["tasks"]}
        retained = [tid for tid in unique_children if tid in tasks]
        require(retained and all(tasks[tid]["exited"] == 1 for tid in retained),
                f"{name}: retained children lack final exit evidence")
        self.result["tests"]["concurrent_fork_churn"] = {
            "passed": True, "mode": "fixed", "pinned_cpu": selected_cpu,
            "parents": parent_results, "overlapping_parents": active_parents,
            "children_per_parent": children_per_parent,
            "children_per_batch": children_per_batch,
            "children_created": expected_children, "children_reaped": expected_children,
            "independent_completion_records": len(children),
            "distinct_child_tids": len(unique_children),
            "preemption_mode": preemption_mode,
            "preemption_mode_path": str(preemption_path),
            "retained_child_snapshots": len(retained),
            "retained_tasks": len(final["tasks"]), "eviction_expected": True,
            "live_before": before["stats"]["live_tasks"],
            "live_drained": drained["stats"]["live_tasks"],
            "known_minimum_new_associations": expected_children + parent_count,
            "observed_new_init_associations": initialized,
            "final_stats": final["stats"], "maps": owned_maps,
            "maps_released": True, "map_release": map_release,
            "proof_scope": "finite same-CPU concurrent fork/exec with exact independent completion counts",
        }
        self.save()
        print(f"PASS {name}: parents={parent_count} children={expected_children} "
              f"init={final['stats']['init_tasks']} exit={final['stats']['exit_tasks']} "
              f"preemption={preemption_mode}", flush=True)

    def fairness(self):
        for mode in ("fixed", "shadow", "online"):
            name = f"fairness-{mode}"
            loader = self.scheduler(name, mode=mode, duration=self.args.fairness_seconds + 15,
                                    extra=("--sample-ms", "1000"))
            owned_maps = self.owned_maps(name, loader)
            processes = [self.start_lifecycle(name + f"-cpu{index}", "cpu",
                                              duration=self.args.fairness_seconds, index=index)
                         for index in range(8)]
            for process in processes:
                self.finish(process, timeout=self.args.fairness_seconds + 10)
            self.stop(loader)
            map_release = self.maps_released(owned_maps, name)
            final = latest_snapshot(self.output / f"{name}.jsonl")
            self.check_counters(final, name)
            tasks = {task["pid"]: task for task in final["tasks"]}
            measurements = [next(event for event in rows(self.output / f"{name}-cpu{index}.log")
                                  if event.get("event") == "worker") for index in range(8)]
            service = [event["cpu_ns"] for event in measurements]
            jain = sum(service) ** 2 / (len(service) * sum(value ** 2 for value in service))
            gap = max(event["max_run_gap_ns"] for event in measurements)
            require(all(value > 0 for value in service), f"{name}: zero-service competitor")
            require(jain > 0.9, f"{name}: Jain fairness index {jain} <= 0.9")
            require(gap < 1000000000, f"{name}: observed runnable gap {gap} >= 1s")
            checks = []
            for measured in measurements:
                require(measured["tid"] in tasks, f"{name}: missing competitor snapshot")
                task = tasks[measured["tid"]]
                checks.append(self.compare_service(measured, task, name))
                self.validate_task_policy(task, mode, name, negative=True)
            self.result["fairness"].append({"mode": mode, "passed": True,
                "competitors": 8, "available_cpus": 4, "duration_seconds": self.args.fairness_seconds,
                "jain_index": jain, "max_observed_runnable_gap_ns": gap,
                "maps": owned_maps, "maps_released": True,
                "map_release": map_release,
                "service_checks": checks, "proof_scope": "finite equal-weight VM workload"})
            self.save()
            print(f"PASS {name}: Jain={jain:.6f} max_gap_ms={gap / 1000000:.3f}", flush=True)

    def repeated_recovery(self):
        root_output, root_result = self.output, self.result
        for repetition in range(self.args.recovery_repetitions):
            self.output = root_output / f"recovery-{repetition}"
            self.output.mkdir()
            self.result = {"kernel": os.uname().release, "tests": {}, "performance_claim": False}
            try:
                # Includes guarded exact-TGID watchdog fault in partial mode.
                super().recovery()
                evidence = {"repetition": repetition, "passed": True,
                            "tests": self.result["tests"]}
            finally:
                self.output, self.result = root_output, root_result
            self.result["recovery_repetitions"].append(evidence)
            self.save()
            print(f"PASS complete recovery repetition {repetition}", flush=True)

    def execute(self):
        require(os.uname().sysname == "Linux" and os.geteuid() == 0,
                "Phase 2A runner requires root inside the Linux guest")
        require(read_state() == "disabled", "Refusing an existing active scheduler")
        require(not os.environ.get("PYTHONOPTIMIZE"), "Disable PYTHONOPTIMIZE: inherited assertions are required")
        try:
            probe = self.launch([self.loader, "--check"], "preflight")
            self.finish(probe)
            self.result["tests"]["preflight"] = {"passed": True}
            self.build_workload()
            if "lifecycle" in self.args.tests:
                self.lifecycle()
            if "concurrent-fork" in self.args.tests:
                self.concurrent_fork_churn()
            if "fairness" in self.args.tests:
                self.fairness()
            if "recovery" in self.args.tests:
                self.repeated_recovery()
            self.result["completed"] = True
            self.result["final_state"] = read_state()
            require(read_state() == "disabled", "suite ended with an active scheduler")
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
    parser.add_argument("--output", type=Path, default=ROOT / "results/local/phase2a")
    parser.add_argument("--tests", nargs="+", choices=("lifecycle", "concurrent-fork", "fairness", "recovery"),
                        default=["lifecycle", "concurrent-fork", "fairness", "recovery"])
    parser.add_argument("--fairness-seconds", type=int, default=9)
    parser.add_argument("--recovery-repetitions", type=int, default=3)
    args = parser.parse_args()
    if not 9 <= args.fairness_seconds <= 60 or not 3 <= args.recovery_repetitions <= 10:
        parser.error("fairness-seconds must be 9..60; recovery-repetitions must be 3..10")
    Phase2Validation(args).execute()


if __name__ == "__main__":
    main()
