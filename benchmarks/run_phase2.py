#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Randomized, blocked Linux CPU scheduling comparisons. Retain every raw trial."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import shlex
import shutil
import signal
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = Path("/sys/kernel/sched_ext/state")
OPS = Path("/sys/kernel/sched_ext/root/ops")
BPF_STATS = Path("/proc/sys/kernel/bpf_stats_enabled")
WORKLOADS = ("cpu", "shortburst", "wakeheavy", "mixed", "multicore", "affinity", "kernelcompile", "memory")


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def rows(path):
    with path.open() as handle:
        return [json.loads(line) for line in handle if line.strip().startswith("{")]


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    index = (len(values) - 1) * fraction
    lower = int(index)
    return values[lower] + (values[min(lower + 1, len(values) - 1)] - values[lower]) * (index - lower)


def quantiles(values):
    return {f"p{int(fraction * 100)}": percentile(values, fraction) for fraction in (0.5, 0.95, 0.99)}


def bootstrap_median(values, seed=12039):
    """Resample trial-level observations, never treat within-trial jobs as trials."""
    if not values:
        return None
    rng = random.Random(seed)
    draws = [statistics.median(rng.choices(values, k=len(values))) for _ in range(10000)]
    return {"n": len(values), "median": statistics.median(values),
            "minimum": min(values), "maximum": max(values),
            "iqr": percentile(values, .75) - percentile(values, .25),
            "bootstrap_median_ci95": [percentile(draws, .025), percentile(draws, .975)]}


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def cpu_ticks():
    fields = Path("/proc/stat").read_text().splitlines()[0].split()[1:]
    return [int(value) for value in fields[:8]]  # Exclude guest fields already counted in user/nice.


def psi():
    return {name: read_optional(f"/proc/pressure/{name}") for name in ("cpu", "memory", "io")}


def pressure_totals(text):
    if not text:
        return {}
    result = {}
    for line in text.splitlines():
        fields = line.split()
        values = dict(field.split("=") for field in fields[1:])
        result[fields[0]] = int(values["total"])
    return result


def vm_counters():
    counters = {}
    for line in Path("/proc/vmstat").read_text().splitlines():
        name, value = line.split()
        if name in ("pswpin", "pswpout", "pgmajfault", "pgscan_direct", "pgscan_kswapd", "allocstall_normal"):
            counters[name] = int(value)
    return counters


def perf_counters(path):
    counters = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split(";")
        if len(fields) < 3:
            continue
        value, unit, name = fields[:3]
        if "<" in value:
            raise RuntimeError(f"perf counter {name} unavailable: {value}")
        counters[name] = {"value": float(value), "unit": unit}
    for name in ("task-clock", "context-switches", "cpu-migrations", "page-faults"):
        if name not in counters:
            raise RuntimeError(f"missing perf software counter {name}")
    return counters


def programs():
    result = subprocess.run(["bpftool", "-j", "prog", "show"], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"bpftool prog show failed: {result.stderr}")
    return {item["id"]: item for item in json.loads(result.stdout)}


def wait_state(wanted, process=None, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if STATE.read_text().strip() == wanted:
            return
        if process is not None and process.poll() is not None:
            raise RuntimeError(f"scheduler exited {process.returncode} before {wanted}")
        time.sleep(.05)
    raise RuntimeError(f"sched_ext did not become {wanted}")


def summarize_workload(path):
    events = rows(path)
    window = next(event for event in events if event["event"] == "window")
    workers = [event for event in events if event["event"] == "worker"]
    samples = [event for event in events if event["event"] == "sample"]
    bursts = [event for event in samples if event["role"] == "burst"]
    cpu_workers = [event for event in workers if event["role"] == "cpu"]
    shares = [event["cpu_service_ns"] for event in cpu_workers]
    fairness = sum(shares) ** 2 / (len(shares) * sum(value ** 2 for value in shares)) if shares and sum(shares) else None
    duration = window["duration_seconds"]
    affinity_verified = False
    if all(event.get("initial_allowed_cpus") != [-1] for event in workers):
        selected = set(window["selected_cpus"])
        for worker in workers:
            allowed = set(worker["initial_allowed_cpus"])
            if window["workload"] == "affinity":
                if len(allowed) != 1 or not allowed <= selected:
                    raise RuntimeError("initial migrating-worker affinity differs from configured CPUs")
            elif allowed != selected:
                raise RuntimeError("worker affinity differs from configured CPUs")
        if any(event["cpu"] not in selected for event in samples):
            raise RuntimeError("worker executed on an unconfigured CPU")
        affinity_verified = True
    if any(event["dropped_samples"] for event in workers):
        raise RuntimeError("workload sample buffer overflow: raw results would be incomplete")
    if any(event["samples"] == 0 or event["cpu_service_ns"] == 0 for event in workers):
        raise RuntimeError("workload made no progress: possible starvation")
    # Compute-only work units can be compared within a workload; memory access
    # iterations intentionally do not count as equivalent compute operations.
    return {"window": window, "workers": workers, "initial_affinity_verified": affinity_verified,
            "throughput_units_per_s": sum(event["units"] for event in workers if event["role"] != "memory") / duration,
            "burst_jobs_per_s": len(bursts) / duration,
            "deadline_to_run_ns": quantiles([event["wake_lateness_ns"] for event in bursts]),
            "burst_completion_ns": quantiles([event["completion_ns"] for event in bursts]),
            "burst_cpu_service_ns": quantiles([event["cpu_ns"] for event in bursts]),
            "jain_cpu_service_fairness": fairness,
            "minimum_cpu_service_share": min(shares) / sum(shares) if shares else None,
            "cpu_worker_max_progress_gap_ns": max((event["max_progress_gap_ns"] for event in cpu_workers), default=None),
            "all_workers_max_progress_gap_ns": max(event["max_progress_gap_ns"] for event in workers),
            "observed_migrations": sum(event["observed_migrations"] for event in workers),
            "affinity_changes": sum(event["affinity_changes"] for event in workers),
            "voluntary_context_switches": sum(event["voluntary_switches"] for event in workers),
            "involuntary_context_switches": sum(event["involuntary_switches"] for event in workers),
            "worker_cpu_utilization_core_equivalents": sum(event["process_cpu_ns"] for event in workers) / (duration * 1e9),
            "minor_faults": sum(event["minor_faults"] for event in workers),
            "major_faults": sum(event["major_faults"] for event in workers),
            "starvation_observed": False}


class Benchmark:
    def __init__(self, args):
        self.args = args
        self.output = args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.handles = []
        self.owned = []
        self.commands = []
        if args.resume and (self.output / "commands.json").exists():
            self.commands = json.loads((self.output / "commands.json").read_text())
        self.results = []
        self.extra = dict(args.extra_scheduler)
        self.policies = args.policies.split(",")
        self.workloads = args.workloads.split(",")
        self.stats_before = BPF_STATS.read_text() if BPF_STATS.exists() else None
        self.stats_enabled = False
        self.registered_ops = None

    def launch(self, command, output, error=None):
        self.commands.append(shlex.join(str(part) for part in command))
        stdout = output.open("w")
        self.handles.append(stdout)
        if error is None:
            stderr = subprocess.STDOUT
        else:
            stderr = error.open("w")
            self.handles.append(stderr)
        process = subprocess.Popen([str(part) for part in command], stdout=stdout,
                                   stderr=stderr, start_new_session=True,
                                   env={**os.environ, "LC_ALL": "C"})
        self.owned.append(process)
        return process

    def finish(self, process, timeout, expected=0):
        code = process.wait(timeout=timeout)
        if code != expected:
            raise RuntimeError(f"command exited {code}: {shlex.join(process.args)}")

    def stop(self, process):
        if process.poll() is None:
            # Ctrl-C is the shared documented shutdown path for official C,
            # Rust sched_ext loaders and bpftrace. Other signals are exercised
            # separately by the correctness/recovery suite.
            process.send_signal(signal.SIGINT)
        self.finish(process, 20)

    def scheduler(self, policy, directory, workload):
        if STATE.read_text().strip() != "disabled":
            raise RuntimeError("Refusing to replace an existing sched_ext scheduler")
        if policy == "default":
            return None
        if policy in ("fixed", "shadow", "online", "ewma"):
            command = [ROOT / "build/scx_lertick", "--mode", policy, "--duration", "3600",
                       "--sample-ms", "1000", "--stats-json", directory / "scheduler.jsonl"]
            if self.args.profile:
                command += ["--profile"]
            if workload == "kernelcompile":
                command += ["--track-all"]
            wanted = "scx_lertick"
        elif policy == "scx_simple":
            if not self.args.reference:
                raise RuntimeError("scx_simple requires --reference PATH")
            command, wanted = [self.args.reference.resolve()], "simple"
        elif policy in self.extra:
            command, wanted = [self.extra[policy].resolve()], policy.removeprefix("scx_")
        else:
            raise RuntimeError(f"unknown policy {policy}")
        process = self.launch(command, directory / "scheduler.log")
        wait_state("enabled", process)
        actual = OPS.read_text().strip()
        self.registered_ops = actual
        accepted = actual == wanted if policy in ("fixed", "shadow", "online", "ewma") else actual in (wanted, "scx_" + wanted)
        if policy in self.extra and actual.startswith(wanted + "_"):
            accepted = True
        if not accepted:
            raise RuntimeError(f"registered {actual}, expected {wanted}")
        return process

    def observer(self, directory):
        if self.args.without_wake_observer:
            return None
        process = self.launch(["bpftrace", "-q", ROOT / "benchmarks/wake_latency.bt"],
                              directory / "wake.jsonl", directory / "wake.log")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("wake observer failed: " + (directory / "wake.log").read_text())
            if "observer_ready" in (directory / "wake.jsonl").read_text():
                return process
            time.sleep(.1)
        raise RuntimeError("wake observer did not attach")

    def kernel_compile(self, directory):
        source, build = self.args.kernel_source, self.args.kernel_build
        if not source or not build or not (build / ".config").exists():
            raise RuntimeError("kernelcompile requires --kernel-source and preconfigured --kernel-build")
        target = "kernel/sched/built-in.a"
        command = ["taskset", "--cpu-list", self.args.cpus, "make", "-C", source.resolve(),
                   f"O={build.resolve()}", f"-j{len(self.args.cpus.split(','))}", target]
        # Build once for generated headers and a warm file cache, then rebuild
        # every scheduler object for each completed compilation observation.
        for object_path in (build / "kernel/sched").glob("*.o"):
            object_path.unlink()
        archive = build / target
        if archive.exists():
            archive.unlink()
        warm_started = time.monotonic()
        warm = self.launch(command, directory / "compile-warmup.log")
        self.finish(warm, 900)
        if time.monotonic() - warm_started < self.args.warmup:
            time.sleep(self.args.warmup - (time.monotonic() - warm_started))
        deadline = time.monotonic() + self.args.duration
        window_start = time.monotonic_ns()
        compile_times = []
        counters = []
        while time.monotonic() < deadline:
            objects = list((build / "kernel/sched").glob("*.o"))
            if not objects:
                raise RuntimeError("kernel compilation target produced no scheduler objects")
            for object_path in objects:
                object_path.unlink()
            archive = build / target
            if archive.exists():
                archive.unlink()
            started = time.monotonic_ns()
            perf_output = directory / f"compile-{len(compile_times)}-perf.csv"
            measured_command = [self.args.perf, "stat", "-x", ";", "--no-big-num", "-o", perf_output,
                                "-e", "task-clock,context-switches,cpu-migrations,page-faults", "--", *command]
            process = self.launch(measured_command, directory / f"compile-{len(compile_times)}.log")
            self.finish(process, 900)
            compile_times.append(time.monotonic_ns() - started)
            counters.append(perf_counters(perf_output))
        actual = (time.monotonic_ns() - window_start) / 1e9
        return {"kernel_compile_target": target, "completed_compilations": len(compile_times),
                "compile_elapsed_ns": compile_times, "completion_time_ns": quantiles(compile_times),
                "throughput_compilations_per_s": len(compile_times) / actual,
                "measured_duration_seconds": actual,
                "compile_perf_counters": counters,
                "voluntary_and_involuntary_context_switches": sum(item["context-switches"]["value"] for item in counters),
                "kernel_observed_migrations": sum(item["cpu-migrations"]["value"] for item in counters),
                "kernel_compile_page_faults": sum(item["page-faults"]["value"] for item in counters),
                "worker_cpu_utilization_core_equivalents": sum(item["task-clock"]["value"] for item in counters) / (actual * 1000),
                "window": {"measured_start_ns": window_start, "end_ns": time.monotonic_ns()},
                "warmup": "one actual target build; generated headers and warm file cache",
                "starvation_observed": None}

    def lertick_metrics(self, directory, result):
        telemetry = directory / "scheduler.jsonl"
        if not telemetry.exists():
            return None
        events = rows(telemetry)
        if not any(event.get("verifier_accepted") for event in events):
            raise RuntimeError("missing verifier acceptance evidence")
        if not events[-1].get("default_restored"):
            raise RuntimeError("loader did not confirm restoration")
        snapshots = [event for event in events if event.get("event") == "snapshot"]
        final = next(event for event in reversed(snapshots) if event.get("phase") == "final")
        start = result["window"]["measured_start_ns"]
        previous = [event for event in snapshots if event["monotonic_ns"] <= start]
        first = previous[-1] if previous else {"stats": {}, "tasks": []}
        delta = {name: value - first["stats"].get(name, 0) for name, value in final["stats"].items()}
        for name in ("counter_errors", "alloc_failures", "snapshot_failures", "fault_stalls"):
            if final["stats"].get(name, 0):
                raise RuntimeError(f"nonzero {name}: {final['stats'][name]}")
        initial_tasks = {task["pid"]: task for task in first["tasks"]}
        task_deltas = []
        for task in final["tasks"]:
            initial = initial_tasks.get(task["pid"], {})
            task_deltas.append({name: value - initial.get(name, 0) for name, value in task.items()
                                if isinstance(value, int) and name.endswith(("error_ns", "error_us2", "samples", "clipped_targets"))})
        error_sums = {name: sum(task.get(name, 0) for task in task_deltas)
                      for name in ("prediction_samples", "linear_abs_error_ns", "ewma_abs_error_ns",
                                   "linear_squared_error_us2", "ewma_squared_error_us2", "clipped_targets")}
        count = error_sums["prediction_samples"]
        prediction = {"samples": count, "target": "verified CPU episode, clipped to 10 ms",
                      "policy_prediction_mae_ns": error_sums["linear_abs_error_ns"] / count if count else None,
                      "linear_mae_ns": error_sums["linear_abs_error_ns"] / count if count and final["mode"] in ("shadow", "online") else None,
                      "ewma_mae_ns": error_sums["ewma_abs_error_ns"] / count if count else None,
                      "policy_prediction_rmse_ns": math.sqrt(error_sums["linear_squared_error_us2"] / count) * 1000 if count else None,
                      "ewma_rmse_ns": math.sqrt(error_sums["ewma_squared_error_us2"] / count) * 1000 if count else None,
                      "clipped_target_count": error_sums["clipped_targets"]}
        timing = {}
        for prefix in ("training", "inference", "selection"):
            total = delta.get(prefix + "_time_ns", delta.get(prefix + "_ns", 0))
            calls = delta.get(prefix + "_timed_calls", delta.get(prefix + "_calls", delta.get(
                "training_updates" if prefix == "training" else "inferences" if prefix == "inference" else "selection_calls", 0)))
            timing[prefix + "_mean_ns"] = total / calls if calls else None
            timing[prefix + "_timed_calls"] = calls
        return {"measured_counter_delta": delta, "counter_window_note":
                "last 1-second snapshot before measured window through final unload snapshot; approximate boundary",
                "prediction": prediction, "internal_profile": timing if self.args.profile else None,
                "final_stats": final["stats"]}

    def trial(self, repetition, workload, policy):
        directory = self.output / f"r{repetition:02d}-{workload}-{policy}"
        directory.mkdir(exist_ok=True)
        summary = directory / "trial.json"
        if self.args.resume and summary.exists():
            result = json.loads(summary.read_text())
            if result.get("passed"):
                self.results.append(result)
                return
        scheduler = observer = None
        self.registered_ops = None
        try:
            before_ids = programs() if self.stats_enabled else {}
            scheduler = self.scheduler(policy, directory, workload)
            scheduler_ids = set(programs()) - set(before_ids) if self.stats_enabled else set()
            before_programs = programs() if self.stats_enabled else {}
            observer = self.observer(directory)
            utilization_start, pressure_start, vm_start = cpu_ticks(), psi(), vm_counters()
            started = time.monotonic()
            if workload == "kernelcompile":
                result = self.kernel_compile(directory)
            else:
                command = [ROOT / "build/phase2_workload", "--workload", workload,
                           "--duration", str(self.args.duration), "--warmup", str(self.args.warmup),
                           "--cpus", self.args.cpus, "--memory-mib", str(self.args.memory_mib)]
                worker = self.launch(command, directory / "workload.jsonl", directory / "workload.log")
                self.finish(worker, self.args.duration + self.args.warmup + 120)
                result = summarize_workload(directory / "workload.jsonl")
            utilization_end, pressure_end, vm_end = cpu_ticks(), psi(), vm_counters()
            after_programs = programs() if self.stats_enabled else {}
            if observer:
                self.stop(observer)
                observer = None
                observer_log = (directory / "wake.log").read_text()
                if "lost" in observer_log.lower():
                    raise RuntimeError("wake observer reported lost events")
                wake_events = [event for event in rows(directory / "wake.jsonl")
                               if event.get("event") == "wake_run" and
                               result["window"]["measured_start_ns"] <= event["monotonic_ns"] <= result["window"]["end_ns"]]
                result["wake_to_run_ns"] = quantiles([event["latency_ns"] for event in wake_events])
                result["wake_to_run_sample_count"] = len(wake_events)
                result["wake_to_run_sampling"] = "every 16th wake per lertick TID; every matching make/cc1/cc1plus/as/gcc wake"
                if workload not in ("cpu", "affinity", "kernelcompile") and not wake_events:
                    raise RuntimeError("wake observer produced no measured wake samples")
            else:
                result["wake_to_run_ns"] = None
            if scheduler:
                self.stop(scheduler)
                scheduler = None
                wait_state("disabled")
            delta_ticks = [end - start for end, start in zip(utilization_end, utilization_start)]
            pressure_delta = {name: {key: value - pressure_totals(pressure_start[name]).get(key, 0)
                                    for key, value in pressure_totals(pressure_end[name]).items()}
                              for name in pressure_start}
            vm_delta = {name: value - vm_start.get(name, 0) for name, value in vm_end.items()}
            result.update({"passed": True, "repetition": repetition, "workload": workload, "policy": policy,
                           "elapsed_seconds_including_warmup": time.monotonic() - started,
                           "system_cpu_utilization_fraction": (sum(delta_ticks) - sum(delta_ticks[3:5])) / sum(delta_ticks),
                           "system_cpu_utilization_window": "includes calibration, warmup, and raw output",
                           "pressure_before": pressure_start, "pressure_after": pressure_end,
                           "pressure_total_delta_us": pressure_delta, "vmstat_delta": vm_delta,
                           "memory_pressure_observed": bool(pressure_delta["memory"].get("some", 0)),
                           "swap_interference_observed": vm_delta.get("pswpin", 0) > 0,
                           "profile_enabled": self.args.profile, "bpf_stats_enabled": self.stats_enabled,
                           "wake_observer_enabled": not self.args.without_wake_observer})
            result["registered_ops"] = self.registered_ops
            result["lertick"] = self.lertick_metrics(directory, result)
            bpf = []
            for identifier in sorted(scheduler_ids):
                first, last = before_programs.get(identifier), after_programs.get(identifier)
                if not first or not last:
                    continue
                calls = last.get("run_cnt", 0) - first.get("run_cnt", 0)
                runtime = last.get("run_time_ns", 0) - first.get("run_time_ns", 0)
                bpf.append({"id": identifier, "name": last["name"], "calls": calls,
                            "runtime_ns": runtime, "mean_ns": runtime / calls if calls else None})
            result["bpf_program_runtime"] = bpf if self.stats_enabled else None
            result["bpf_runtime_total_ns"] = sum(item["runtime_ns"] for item in bpf) if self.stats_enabled else None
            result["bpf_callback_calls"] = sum(item["calls"] for item in bpf) if self.stats_enabled else None
            save(summary, result)
            self.results.append(result)
            save(self.output / "progress.json", {"completed_trials": len(self.results), "last":
                 {"repetition": repetition, "workload": workload, "policy": policy}})
            print(f"PASS r{repetition} {workload} {policy}", flush=True)
        except BaseException as error:
            save(directory / "failure.json", {"error": str(error), "policy": policy, "workload": workload,
                                              "registered_ops": self.registered_ops})
            raise
        finally:
            for process in (observer, scheduler):
                if process and process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
            for handle in self.handles:
                handle.close()
            self.handles.clear()
            save(self.output / "commands.json", self.commands)

    def report(self):
        # Per-trial percentile medians preserve trial as the independent unit.
        fields = ("throughput_units_per_s", "burst_jobs_per_s", "jain_cpu_service_fairness",
                  "cpu_worker_max_progress_gap_ns", "observed_migrations",
                  "voluntary_context_switches", "involuntary_context_switches",
                  "worker_cpu_utilization_core_equivalents", "throughput_compilations_per_s",
                  "voluntary_and_involuntary_context_switches", "kernel_observed_migrations",
                  "bpf_runtime_total_ns", "bpf_callback_calls")
        groups = []
        for workload in self.workloads:
            for policy in self.policies:
                trials = [trial for trial in self.results if trial["workload"] == workload and trial["policy"] == policy]
                if not trials:
                    continue
                metrics = {field: bootstrap_median([trial[field] for trial in trials if trial.get(field) is not None]) for field in fields}
                for family in ("burst_completion_ns", "deadline_to_run_ns", "wake_to_run_ns", "completion_time_ns"):
                    for quantile in ("p50", "p95", "p99"):
                        metrics[family + "." + quantile] = bootstrap_median([
                            trial[family][quantile] for trial in trials
                            if trial.get(family) and trial[family].get(quantile) is not None])
                for model in ("linear", "ewma"):
                    metrics[model + "_mae_ns"] = bootstrap_median([
                        trial["lertick"]["prediction"][model + "_mae_ns"] for trial in trials
                        if trial.get("lertick") and trial["lertick"]["prediction"][model + "_mae_ns"] is not None])
                groups.append({"workload": workload, "policy": policy, "trials": len(trials), "metrics": metrics})
        paired = []
        for workload in self.workloads:
            for comparator in ("default", "fixed", "ewma", "scx_simple", *self.extra):
                for metric in ("throughput_units_per_s", "throughput_compilations_per_s", "burst_completion_ns.p99", "wake_to_run_ns.p99"):
                    changes = []
                    for repetition in range(self.args.repetitions):
                        members = {trial["policy"]: trial for trial in self.results if trial["workload"] == workload and trial["repetition"] == repetition}
                        try:
                            left, right = members["online"], members[comparator]
                            for part in metric.split("."):
                                left, right = left[part], right[part]
                            if right:
                                changes.append((left / right - 1) * 100)
                        except (KeyError, TypeError):
                            continue
                    if changes:
                        paired.append({"workload": workload, "comparator": comparator, "metric": metric,
                                       "online_percent_change": bootstrap_median(changes),
                                       "note": "positive is higher; throughput and latency have opposite desirability"})
        correlations = []
        for workload in self.workloads:
            for policy in ("shadow", "online", "ewma"):
                trials = [trial for trial in self.results if trial["workload"] == workload and
                          trial["policy"] == policy and trial.get("burst_completion_ns") and
                          trial.get("lertick") and trial["lertick"]["prediction"]["policy_prediction_mae_ns"] is not None]
                xs = [trial["lertick"]["prediction"]["policy_prediction_mae_ns"] for trial in trials]
                ys = [trial["burst_completion_ns"]["p99"] for trial in trials]
                coefficient = None
                if len(xs) >= 3:
                    mx, my = statistics.mean(xs), statistics.mean(ys)
                    divisor = math.sqrt(sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys))
                    if divisor:
                        coefficient = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / divisor
                correlations.append({"workload": workload, "policy": policy, "n": len(xs),
                                     "mae_vs_p99_completion_pearson": coefficient,
                                     "note": "descriptive within-policy trial correlation; five trials cannot establish causation"})
        save(self.output / "summary.json", {"groups": groups, "paired_online_comparisons": paired,
             "prediction_latency_correlations": correlations,
             "completed_trials": len(self.results), "planned_trials": self.args.repetitions * len(self.workloads) * len(self.policies),
             "method": "trial-level median; seeded 10,000 bootstrap resamples; paired randomized repetition blocks",
             "limitations": "five repetitions give coarse uncertainty; many workload comparisons require confirmation; ARM64 QEMU results are not bare-metal claims"})

    def run(self):
        if os.uname().sysname != "Linux" or os.geteuid():
            raise RuntimeError("run as root inside the isolated Linux test VM")
        if self.args.repetitions < 5 and not self.args.smoke:
            raise RuntimeError("credible comparisons require >=5 repetitions; use --smoke only for runner checks")
        if (self.args.duration < 10 or self.args.warmup < 2) and not self.args.smoke:
            raise RuntimeError("credible comparisons require >=10 s measured and >=2 s warmup")
        if not self.args.without_wake_observer and not shutil.which("bpftrace"):
            raise RuntimeError("bpftrace is required for kernel wake latency; install it or explicitly select --without-wake-observer")
        if "kernelcompile" in self.workloads and not shutil.which(str(self.args.perf)):
            raise RuntimeError("kernelcompile requires a working perf executable; provide --perf PATH")
        if any(workload not in WORKLOADS for workload in self.workloads):
            raise RuntimeError("unknown workload category")
        if self.args.repetitions < 1 or not math.isfinite(self.args.duration) or not math.isfinite(self.args.warmup) or self.args.duration <= 0 or self.args.warmup < 0:
            raise RuntimeError("duration and repetition count must be positive; warmup must be finite and nonnegative")
        if set(self.policies) - {"default", "scx_simple", "fixed", "shadow", "online", "ewma", *self.extra}:
            raise RuntimeError("unknown policy")
        manifest = {"kernel": os.uname().release, "architecture": os.uname().machine,
                    "arguments": {key: str(value) for key, value in vars(self.args).items()},
                    "environment": {"cpuinfo": read_optional("/proc/cpuinfo"), "meminfo": read_optional("/proc/meminfo"),
                                    "cmdline": read_optional("/proc/cmdline"), "os_release": read_optional("/etc/os-release")},
                    "swap": read_optional("/proc/swaps"),
                    "programs": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                                 (ROOT / "build/phase2_workload", ROOT / "build/scx_lertick", *self.extra.values(),
                                  *([self.args.reference] if self.args.reference else [])) if path.exists()},
                    "benchmark_sources": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                                          (ROOT / "benchmarks/phase2_workload.c", ROOT / "benchmarks/run_phase2.py", ROOT / "benchmarks/wake_latency.bt")},
                    "compiler": subprocess.run(["cc", "--version"], capture_output=True, text=True).stdout,
                    "smoke_only": self.args.smoke, "source_git_commit": subprocess.run(
                        ["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()}
        manifest_path = self.output / "environment.json"
        if manifest_path.exists():
            if not self.args.resume:
                raise RuntimeError("output already contains an experiment; use a fresh directory or --resume")
            original = json.loads(manifest_path.read_text())
            for key in ("kernel", "architecture", "programs", "benchmark_sources"):
                if original.get(key) != manifest[key]:
                    raise RuntimeError(f"cannot resume after changing {key}; use a fresh directory")
            for key, value in original["arguments"].items():
                if key not in ("resume", "output") and manifest["arguments"].get(key) != value:
                    raise RuntimeError(f"cannot resume after changing {key}; use a fresh directory")
        else:
            save(manifest_path, manifest)
        rng = random.Random(self.args.seed)
        plan = []
        for repetition in range(self.args.repetitions):
            workloads = self.workloads[:]
            rng.shuffle(workloads)
            for workload in workloads:
                policies = self.policies[:]
                rng.shuffle(policies)
                plan.extend((repetition, workload, policy) for policy in policies)
        save(self.output / "plan.json", plan)
        try:
            if self.args.bpf_runtime:
                if self.stats_before is None:
                    raise RuntimeError("kernel.bpf_stats_enabled is unavailable")
                BPF_STATS.write_text("1\n")
                self.stats_enabled = True
            for repetition, workload, policy in plan:
                self.trial(repetition, workload, policy)
            self.report()
        finally:
            for process in self.owned:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
            if self.stats_enabled:
                BPF_STATS.write_text(self.stats_before)
            for handle in self.handles:
                handle.close()
            save(self.output / "commands.json", self.commands)


def scheduler_argument(value):
    name, separator, path = value.partition("=")
    if not separator or not name or not path or "/" in name:
        raise argparse.ArgumentTypeError("expected NAME=PATH")
    return name, Path(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--extra-scheduler", type=scheduler_argument, action="append", default=[])
    parser.add_argument("--policies", default="default,scx_simple,fixed,shadow,ewma,online")
    parser.add_argument("--workloads", default=",".join(WORKLOADS))
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--warmup", type=float, default=2)
    parser.add_argument("--seed", type=int, default=20261009)
    parser.add_argument("--cpus", default=",".join(str(cpu) for cpu in sorted(os.sched_getaffinity(0))) if hasattr(os, "sched_getaffinity") else "0")
    parser.add_argument("--memory-mib", type=int, default=3600)
    parser.add_argument("--kernel-source", type=Path)
    parser.add_argument("--kernel-build", type=Path)
    parser.add_argument("--perf", default="perf")
    parser.add_argument("--bpf-runtime", action="store_true")
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--without-wake-observer", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--smoke", action="store_true", help="explicitly non-performance smoke experiment")
    args = parser.parse_args()
    Benchmark(args).run()


if __name__ == "__main__":
    main()
