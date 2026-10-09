#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only report analysis. Reads immutable Linux artifacts; executes no workloads."""
import argparse
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import random
import statistics

POLICIES = ("default", "scx_simple", "fixed", "shadow", "ewma", "online", "scx_bpfland")
WORKLOADS = ("cpu", "shortburst", "wakeheavy", "mixed", "multicore", "affinity", "kernelcompile", "memory")
CAUTIONS = [
    "Each independent observation is one policy trial. Within-trial jobs are not independent repetitions.",
    "Five repetitions give coarse bootstrap intervals. Many comparisons are exploratory and require independent confirmation.",
    "Throughput uses compute iterations/s for custom workloads and completed kernel/sched/built-in.a rebuilds/s for kernelcompile; these cannot be pooled.",
    "Burst completion is timer-deadline-to-completion; deadline-to-run is a userspace timer proxy. wake_to_run uses kernel wakeup-to-next-switch timestamps.",
    "Reported percentiles are medians of per-trial percentiles, not a percentile of pooled jobs.",
    "Kernel wake latency samples every sixteenth custom-task wake; per-trial sample counts are retained and p99 may have few tail observations.",
    "BPF runtime and calls include calibration, warmup, and raw-output time. Runtime/call is a callback mean and excludes kernel C scheduler work and userspace-loader cost.",
    "Whole-callback means have different callback mixes across policies. Callback-name breakdowns help interpret these means.",
    "Prediction/timing deltas extend from the final one-second snapshot before measurement through final unload; boundaries are approximate.",
    "Prequential prediction errors compare wake-time predictions against subsequent verified CPU episodes capped at 10 ms; clipped targets are reported.",
    "EWMA mode policy_prediction_mae_ns is EWMA accuracy; linear_mae_ns is only valid in shadow and online modes.",
    "Retained LRU task snapshots may omit episodes after eviction. Live counters and task snapshots are read without a transaction, so small baseline-boundary mismatches also occur without eviction. Incomplete retained RMSE is labeled accordingly.",
    "Global MAE uses global abs-error sums/global prediction count and avoids retained-entry eviction, but its live baseline is also approximate. Kernelcompile --track-all includes observer, runner, and unrelated guest tasks, not only compiler descendants.",
    "Memory PSI and swap deltas include initial prefaults/warmup, so they prove trial-level pressure, not that every measured second was under pressure.",
    "Custom-workload major/minor page-fault counts use each thread's measured execution window and provide additional evidence of measured interference.",
    "Observed custom-workload migrations are sampled at chunk boundaries and may miss intermediate migrations. Kernel compile uses inherited perf software counters.",
    "Fairness compares equal continuous CPU workers. Progress gaps are observed outcomes, not mathematical starvation bounds.",
    "Cross-session overhead comparisons are unpaired; repeated index alone does not create a matched experimental block.",
    "Profile timers and kernel BPF runtime accounting add observer cost. A timer mean is not an uninstrumented function runtime.",
    "ARM64 QEMU/HVF measurements do not establish bare-metal or general Linux scheduler superiority.",
]


def read_json(path):
    if path.suffix == ".gz":
        with gzip.open(path, "rt") as handle:
            return json.load(handle)
    return json.loads(path.read_text())


def get(value, path):
    try:
        for component in path.split("."):
            value = value[component]
    except (KeyError, TypeError):
        return None
    if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
        return value
    return None


def percentile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (index - lower)


_cache = {}


def summarize(values):
    values = tuple(value for value in values if value is not None and math.isfinite(value))
    if not values:
        return None
    if values in _cache:
        return _cache[values]
    rng = random.Random(12039)
    draws = [statistics.median(rng.choices(values, k=len(values))) for _ in range(10000)]
    result = {"n": len(values), "median": statistics.median(values), "minimum": min(values),
              "maximum": max(values), "iqr": percentile(values, .75) - percentile(values, .25),
              "bootstrap_median_ci95": [percentile(draws, .025), percentile(draws, .975)]}
    _cache[values] = result
    return result


def pearson(xs, ys):
    if len(xs) < 3:
        return None
    mx, my = statistics.mean(xs), statistics.mean(ys)
    denominator = math.sqrt(sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys))
    if not denominator:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / denominator


def bool_arg(manifest, name):
    return manifest.get("arguments", {}).get(name) == "True"


def retained_truth(trial_path, trial):
    telemetry = trial_path.parent / "scheduler.jsonl"
    if not telemetry.exists():
        telemetry = trial_path.parent / "scheduler.jsonl.gz"
    if not telemetry.exists():
        return {"telemetry_available": False}
    opener = gzip.open if telemetry.suffix == ".gz" else open
    first, final = None, None
    with opener(telemetry, "rt") as handle:
        for line in handle:
            if not line.startswith("{"):
                continue
            event = json.loads(line)
            if event.get("event") != "snapshot":
                continue
            if event["monotonic_ns"] <= trial["window"]["measured_start_ns"]:
                first = event
            if event.get("phase") == "final":
                final = event
    if final is None:
        return {"telemetry_available": True, "final_snapshot_available": False}
    initial = {task["pid"]: task for task in (first or {}).get("tasks", [])}
    total = sum(task.get("raw_burst_ns_sum", 0) - initial.get(task["pid"], {}).get("raw_burst_ns_sum", 0)
                for task in final["tasks"])
    count = (trial.get("lertick") or {}).get("prediction", {}).get("samples", 0)
    return {"telemetry_available": True, "final_snapshot_available": True,
            "retained_raw_burst_ns_sum": total,
            "retained_raw_burst_mean_ns": total / count if count else None,
            "snapshot_entries_initial": len(initial), "snapshot_entries_final": len(final["tasks"])}


def check_accuracy(trial):
    lertick = trial.get("lertick") or {}
    prediction, counters = lertick.get("prediction", {}), lertick.get("measured_counter_delta", {})
    retained, global_count = prediction.get("samples"), counters.get("prediction_samples")
    count_complete = retained == global_count if retained is not None and global_count is not None else None
    sums_match = {}
    for retained_key, global_key in (("policy_prediction_mae_ns", "linear_abs_error_ns"), ("ewma_mae_ns", "ewma_abs_error_ns")):
        if retained and prediction.get(retained_key) is not None and counters.get(global_key) is not None:
            sums_match[global_key] = math.isclose(prediction[retained_key] * retained, counters[global_key], rel_tol=1e-12, abs_tol=.5)
    complete = bool(count_complete and all(sums_match.values())) if count_complete is not None else None
    trial["global_prediction_samples"] = global_count
    trial["retained_prediction_samples"] = retained
    trial["retained_prediction_sample_deficit"] = global_count - retained if global_count is not None and retained is not None else None
    trial["global_policy_prediction_mae_ns"] = counters["linear_abs_error_ns"] / global_count if global_count and "linear_abs_error_ns" in counters else None
    trial["global_ewma_mae_ns"] = counters["ewma_abs_error_ns"] / global_count if global_count and "ewma_abs_error_ns" in counters else None
    trial["global_linear_mae_ns"] = trial["global_policy_prediction_mae_ns"] if trial["policy"] in ("shadow", "online") else None
    linear, ewma = trial["global_linear_mae_ns"], trial["global_ewma_mae_ns"]
    trial["linear_vs_ewma_mae_percent_change"] = (linear / ewma - 1) * 100 if linear is not None and ewma else None
    trial["accuracy_retention"] = {"retained_samples": retained, "global_samples": global_count,
                                   "count_complete": count_complete, "error_sums_match": sums_match,
                                   "retained_accuracy_complete": complete,
                                   "tracking_scope": "all guest tasks" if trial["workload"] == "kernelcompile" else "lertick comm-prefix opted-in tasks",
                                   "exited_flag_meaning": "SCX association ended, not proof of OS process death"}
    return trial["accuracy_retention"]


def collect(directory, label):
    manifest = read_json(directory / "environment.json")
    paths = sorted(directory.rglob("trial.json")) + sorted(directory.rglob("trial.json.gz"))
    trials = []
    seen = set()
    for path in paths:
        trial = read_json(path)
        if not trial.get("passed"):
            raise RuntimeError(f"failed trial cannot enter analysis: {path}")
        key = (trial["repetition"], trial["workload"], trial["policy"])
        if key in seen:
            raise RuntimeError(f"duplicate trial {key}: {path}")
        seen.add(key)
        if trial.get("bpf_callback_calls"):
            trial["whole_bpf_callback_mean_ns"] = trial["bpf_runtime_total_ns"] / trial["bpf_callback_calls"]
        else:
            trial["whole_bpf_callback_mean_ns"] = None
        prediction = (trial.get("lertick") or {}).get("prediction", {})
        linear, ewma = prediction.get("linear_mae_ns"), prediction.get("ewma_mae_ns")
        trial["linear_vs_ewma_mae_percent_change"] = (linear / ewma - 1) * 100 if linear is not None and ewma else None
        trial["memory_some_pressure_delta_us"] = get(trial, "pressure_total_delta_us.memory.some")
        trial["memory_full_pressure_delta_us"] = get(trial, "pressure_total_delta_us.memory.full")
        trial["cpu_some_pressure_delta_us"] = get(trial, "pressure_total_delta_us.cpu.some")
        trial["total_context_switches"] = trial.get("voluntary_and_involuntary_context_switches")
        if trial["total_context_switches"] is None:
            trial["total_context_switches"] = trial.get("voluntary_context_switches", 0) + trial.get("involuntary_context_switches", 0)
        trial["migration_count"] = trial.get("kernel_observed_migrations", trial.get("observed_migrations"))
        trial["throughput"] = trial.get("throughput_compilations_per_s", trial.get("throughput_units_per_s"))
        trial["throughput_unit"] = "scheduler-target builds/s" if trial["workload"] == "kernelcompile" else "compute iterations/s"
        trial["completion_p50_ns"] = get(trial, "completion_time_ns.p50") if trial["workload"] == "kernelcompile" else get(trial, "burst_completion_ns.p50")
        trial["completion_p95_ns"] = get(trial, "completion_time_ns.p95") if trial["workload"] == "kernelcompile" else get(trial, "burst_completion_ns.p95")
        trial["completion_p99_ns"] = get(trial, "completion_time_ns.p99") if trial["workload"] == "kernelcompile" else get(trial, "burst_completion_ns.p99")
        trial["source_trial"] = str(path.relative_to(directory))
        check_accuracy(trial)
        if trial.get("lertick"):
            trial["retained_truth"] = retained_truth(path, trial)
        trials.append(trial)
    expected_workloads = manifest["arguments"]["workloads"].split(",")
    expected_policies = manifest["arguments"]["policies"].split(",")
    repetitions = int(manifest["arguments"]["repetitions"])
    missing = [(rep, workload, policy) for rep in range(repetitions) for workload in expected_workloads
               for policy in expected_policies if (rep, workload, policy) not in seen]
    return {"label": label, "directory": str(directory), "manifest": manifest, "trials": trials,
            "missing_trials": missing, "complete": not missing,
            "expected_trials": repetitions * len(expected_workloads) * len(expected_policies),
            "flags": {name: bool_arg(manifest, name) for name in ("profile", "bpf_runtime", "without_wake_observer")},
            "artifact_sha256": {str(path.relative_to(directory)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}}


METRICS = (
    "throughput", "burst_jobs_per_s", "completion_p50_ns", "completion_p95_ns", "completion_p99_ns",
    "wake_to_run_ns.p50", "wake_to_run_ns.p95", "wake_to_run_ns.p99", "deadline_to_run_ns.p50",
    "deadline_to_run_ns.p95", "deadline_to_run_ns.p99", "jain_cpu_service_fairness",
    "minimum_cpu_service_share", "cpu_worker_max_progress_gap_ns", "all_workers_max_progress_gap_ns",
    "worker_cpu_utilization_core_equivalents", "system_cpu_utilization_fraction", "total_context_switches",
    "migration_count", "affinity_changes", "wake_to_run_sample_count", "measured_duration_seconds",
    "elapsed_seconds_including_warmup", "completed_compilations", "bpf_runtime_total_ns", "bpf_callback_calls",
    "whole_bpf_callback_mean_ns", "lertick.prediction.samples", "lertick.prediction.policy_prediction_mae_ns",
    "global_prediction_samples", "retained_prediction_samples", "retained_prediction_sample_deficit",
    "global_policy_prediction_mae_ns", "global_linear_mae_ns", "global_ewma_mae_ns",
    "retained_truth.retained_raw_burst_mean_ns",
    "lertick.prediction.linear_mae_ns", "lertick.prediction.ewma_mae_ns",
    "lertick.prediction.policy_prediction_rmse_ns", "lertick.prediction.ewma_rmse_ns",
    "lertick.prediction.clipped_target_count", "linear_vs_ewma_mae_percent_change",
    "lertick.internal_profile.training_mean_ns", "lertick.internal_profile.inference_mean_ns",
    "lertick.internal_profile.training_timed_calls", "lertick.internal_profile.inference_timed_calls",
    "lertick.measured_counter_delta.training_updates", "lertick.measured_counter_delta.inferences",
    "lertick.measured_counter_delta.adaptive_slices", "lertick.measured_counter_delta.continuation_slices",
    "memory_some_pressure_delta_us", "memory_full_pressure_delta_us", "cpu_some_pressure_delta_us",
    "vmstat_delta.pswpin", "vmstat_delta.pswpout", "vmstat_delta.pgmajfault",
    "vmstat_delta.pgscan_direct", "vmstat_delta.pgscan_kswapd",
    "major_faults", "minor_faults",
)


def analyze_dataset(data):
    groups, callbacks, correlations = [], [], []
    for workload in WORKLOADS:
        for policy in POLICIES:
            trials = sorted((trial for trial in data["trials"] if trial["workload"] == workload and trial["policy"] == policy), key=lambda t: t["repetition"])
            if not trials:
                continue
            groups.append({"workload": workload, "policy": policy, "trials": len(trials),
                           "throughput_unit": trials[0]["throughput_unit"],
                           "metrics": {metric: summarize(get(trial, metric) for trial in trials) for metric in METRICS},
                           "memory_pressure_trials": sum(trial.get("memory_pressure_observed", False) for trial in trials),
                           "swap_interference_trials": sum(trial.get("swap_interference_observed", False) for trial in trials),
                           "affinity_verified_trials": sum(trial.get("initial_affinity_verified", False) for trial in trials),
                           "registered_ops": sorted({trial["registered_ops"] for trial in trials if trial.get("registered_ops")}),
                           "retained_accuracy_complete_trials": sum(trial["accuracy_retention"]["retained_accuracy_complete"] is True for trial in trials),
                           "retained_accuracy_incomplete_trials": sum(trial["accuracy_retention"]["retained_accuracy_complete"] is False for trial in trials),
                           "starvation_status": [trial.get("starvation_observed") for trial in trials]})
            names = sorted({program["name"] for trial in trials for program in trial.get("bpf_program_runtime") or []})
            for name in names:
                means, calls, runtimes = [], [], []
                for trial in trials:
                    programs = [program for program in trial.get("bpf_program_runtime") or [] if program["name"] == name]
                    count, runtime = sum(item["calls"] for item in programs), sum(item["runtime_ns"] for item in programs)
                    if count:
                        means.append(runtime / count)
                        calls.append(count)
                        runtimes.append(runtime)
                callbacks.append({"workload": workload, "policy": policy, "name": name,
                                  "mean_runtime_ns_per_trial": summarize(means), "calls_per_trial": summarize(calls),
                                  "pooled_mean_ns": sum(runtimes) / sum(calls) if calls else None,
                                  "pooled_calls": sum(calls)})
            if policy in ("shadow", "online", "ewma"):
                for latency in ("completion_p99_ns", "wake_to_run_ns.p99"):
                    for error in ("global_policy_prediction_mae_ns", "global_ewma_mae_ns"):
                        pairs = [(get(trial, error), get(trial, latency)) for trial in trials]
                        pairs = [(x, y) for x, y in pairs if x is not None and y is not None]
                        correlations.append({"workload": workload, "policy": policy, "latency": latency, "error": error,
                                             "n": len(pairs), "pearson": pearson([x for x, _ in pairs], [y for _, y in pairs]),
                                             "causation": False})
    comparisons = []
    for workload in WORKLOADS:
        for treatment, control in (("online", "default"), ("online", "scx_simple"), ("online", "fixed"),
                                   ("online", "shadow"), ("online", "ewma"), ("online", "scx_bpfland"),
                                   ("shadow", "fixed")):
            for metric in ("throughput", "completion_p50_ns", "completion_p95_ns", "completion_p99_ns", "wake_to_run_ns.p99",
                           "jain_cpu_service_fairness", "whole_bpf_callback_mean_ns", "bpf_runtime_total_ns"):
                values = []
                for repetition in range(int(data["manifest"]["arguments"]["repetitions"])):
                    members = {trial["policy"]: trial for trial in data["trials"] if trial["workload"] == workload and trial["repetition"] == repetition}
                    left, right = get(members.get(treatment), metric), get(members.get(control), metric)
                    if left is not None and right:
                        values.append((left / right - 1) * 100)
                if values:
                    comparisons.append({"workload": workload, "treatment": treatment, "control": control, "metric": metric,
                                        "percent_change": summarize(values), "paired_by_randomized_repetition_block": True,
                                        "positive_meaning": "higher value; desirability depends on metric"})
    return {key: data[key] for key in ("label", "directory", "manifest", "missing_trials", "complete", "expected_trials", "flags", "artifact_sha256")} | {
        "completed_trials": len(data["trials"]), "groups": groups, "comparisons": comparisons,
        "callback_breakdown": callbacks, "prediction_latency_correlations": correlations,
        "accuracy_retention_checks": [{"workload": trial["workload"], "policy": trial["policy"], "repetition": trial["repetition"],
                                       **trial["accuracy_retention"], "retained_truth": trial.get("retained_truth")} for trial in data["trials"] if trial.get("lertick")],
        "accuracy_retention_mismatches": [{"workload": trial["workload"], "policy": trial["policy"], "repetition": trial["repetition"],
                                            **trial["accuracy_retention"]} for trial in data["trials"] if trial.get("lertick") and trial["accuracy_retention"]["retained_accuracy_complete"] is False]}


def independent_ratio_ci(study_values, primary_values):
    if not study_values or not primary_values or not statistics.median(primary_values):
        return None
    rng = random.Random(99121)
    draws = []
    for _ in range(10000):
        denominator = statistics.median(rng.choices(primary_values, k=len(primary_values)))
        if denominator:
            draws.append((statistics.median(rng.choices(study_values, k=len(study_values))) / denominator - 1) * 100)
    return {"primary_n": len(primary_values), "study_n": len(study_values),
            "percent_change_of_medians": (statistics.median(study_values) / statistics.median(primary_values) - 1) * 100,
            "independent_bootstrap_ci95": [percentile(draws, .025), percentile(draws, .975)]}


def dataset_contrasts(reference, treatment):
    contrasts = []
    flag_changes = {name: {"reference": reference["flags"][name], "treatment": treatment["flags"][name]}
                    for name in reference["flags"] if reference["flags"][name] != treatment["flags"][name]}
    for workload in WORKLOADS:
        for policy in POLICIES:
            a = [trial for trial in treatment["trials"] if trial["workload"] == workload and trial["policy"] == policy]
            b = [trial for trial in reference["trials"] if trial["workload"] == workload and trial["policy"] == policy]
            if not a or not b:
                continue
            for metric in ("throughput", "completion_p99_ns", "wake_to_run_ns.p99", "whole_bpf_callback_mean_ns"):
                av, bv = [get(trial, metric) for trial in a], [get(trial, metric) for trial in b]
                av, bv = [value for value in av if value is not None], [value for value in bv if value is not None]
                estimate = independent_ratio_ci(av, bv)
                if estimate:
                    contrasts.append({"reference": reference["label"], "treatment": treatment["label"],
                                      "study": treatment["label"], "workload": workload, "policy": policy,
                                      "metric": metric, "changed_flags": flag_changes,
                                      "single_changed_flag": next(iter(flag_changes)) if len(flag_changes) == 1 else None,
                                      "estimate": estimate, "paired": False,
                                      "note": "separate-session comparison; overlapping CI and temporal drift limit attribution"})
    return contrasts


def study_contrasts(primary, studies):
    contrasts = []
    for study in studies:
        contrasts.extend(dataset_contrasts(primary, study))
    def enabled_observers(data):
        flags = data["flags"]
        return int(flags["profile"]) + int(flags["bpf_runtime"]) + int(not flags["without_wake_observer"])
    for index, first in enumerate(studies):
        for second in studies[index + 1:]:
            # For study pairs, the less instrumented session is the reference.
            reference, treatment = sorted((first, second), key=lambda data: (enabled_observers(data), data["label"]))
            contrasts.extend(dataset_contrasts(reference, treatment))
    return contrasts


def require_compatible(primary, study):
    for key in ("kernel", "architecture", "preemption_mode", "benchmark_sources"):
        if study["manifest"].get(key) != primary["manifest"].get(key):
            raise RuntimeError(f"study {study['label']} changes {key}; do not combine incompatible experiments")
    original = primary["manifest"].get("programs", {})
    subset = study["manifest"].get("programs", {})
    for path, digest in subset.items():
        if original.get(path) != digest:
            raise RuntimeError(f"study {study['label']} changes program {path}; do not combine incompatible binaries")
    required = {"phase2_workload", "scx_lertick"}
    if not required <= {Path(path).name for path in subset}:
        raise RuntimeError(f"study {study['label']} lacks the workload/loader binary fingerprints")
    for key in ("duration", "warmup", "cpus"):
        if study["manifest"]["arguments"].get(key) != primary["manifest"]["arguments"].get(key):
            raise RuntimeError(f"study {study['label']} changes {key}; observer comparison would be confounded")


def estimate_text(summary, scale=1, digits=3):
    if not summary:
        return "—"
    def f(number):
        return f"{number / scale:.{digits}f}"
    return f"{f(summary['median'])} [{f(summary['minimum'])}, {f(summary['maximum'])}]; CI {f(summary['bootstrap_median_ci95'][0])}–{f(summary['bootstrap_median_ci95'][1])}"


def render_markdown(report):
    lines = ["# Host analysis of Linux Phase 2 artifacts", "", "All cells: median [minimum, maximum]; trial-bootstrap 95% CI. Missing values are unavailable, not zero.", ""]
    for dataset in [report["primary"], *report["studies"]]:
        lines += [f"## {dataset['label']}", "", f"Trials: {dataset['completed_trials']}/{dataset['expected_trials']}; complete: {dataset['complete']}; flags: `{dataset['flags']}`.", ""]
        for workload in WORKLOADS:
            groups = [group for group in dataset["groups"] if group["workload"] == workload]
            if not groups:
                continue
            compile_workload = workload == "kernelcompile"
            throughput_unit = "builds/s" if compile_workload else "M iterations/s"
            completion_unit = "s" if compile_workload else "ms"
            lines += [f"### {workload}", "", f"| Policy | n | Throughput ({throughput_unit}) | Completion p99 ({completion_unit}) | Kernel wake p99 (ms) | Jain fairness | Max CPU progress gap (ms) | Whole BPF mean (us/call) |",
                      "| --- | --- | --- | --- | --- | --- | --- | --- |"]
            for group in groups:
                m = group["metrics"]
                row = [group["policy"], str(group["trials"]), estimate_text(m["throughput"], 1 if compile_workload else 1e6),
                       estimate_text(m["completion_p99_ns"], 1e9 if compile_workload else 1e6), estimate_text(m["wake_to_run_ns.p99"], 1e6),
                       estimate_text(m["jain_cpu_service_fairness"]), estimate_text(m["cpu_worker_max_progress_gap_ns"], 1e6),
                       estimate_text(m["whole_bpf_callback_mean_ns"], 1000)]
                lines.append("| " + " | ".join(row) + " |")
            lines.append("")
        lines += ["### Prediction accuracy and profiled operation costs", "",
                  "| Workload | Policy | Global samples | Retained samples | Complete retained trials | Global policy MAE (us) | Global linear MAE (us) | Global EWMA MAE (us) | Linear versus EWMA MAE (%) | Train mean (ns) | Infer mean (ns) |",
                  "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
        for group in dataset["groups"]:
            if group["policy"] not in ("shadow", "online", "ewma"):
                continue
            m = group["metrics"]
            row = [group["workload"], group["policy"], estimate_text(m["global_prediction_samples"], digits=0),
                   estimate_text(m["retained_prediction_samples"], digits=0), f"{group['retained_accuracy_complete_trials']}/{group['trials']}",
                   estimate_text(m["global_policy_prediction_mae_ns"], 1000), estimate_text(m["global_linear_mae_ns"], 1000),
                   estimate_text(m["global_ewma_mae_ns"], 1000), estimate_text(m["linear_vs_ewma_mae_percent_change"]),
                   estimate_text(m["lertick.internal_profile.training_mean_ns"]), estimate_text(m["lertick.internal_profile.inference_mean_ns"])]
            lines.append("| " + " | ".join(row) + " |")
        lines.append("")
    lines += ["## Paired primary online contrasts", "", "| Workload | Comparator | Metric | Online change (%) |", "| --- | --- | --- | --- |"]
    for contrast in report["primary"]["comparisons"]:
        if contrast["treatment"] != "online" or contrast["metric"] not in ("throughput", "completion_p99_ns", "wake_to_run_ns.p99"):
            continue
        lines.append(f"| {contrast['workload']} | {contrast['control']} | {contrast['metric']} | {estimate_text(contrast['percent_change'])} |")
    lines += ["", "## Separate-session observer/profile contrasts", "", "| Treatment / reference | Changed flags | Workload | Policy | Metric | Change of medians (%) | Independent bootstrap CI (%) |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for contrast in report["study_contrasts"]:
        e = contrast["estimate"]
        changed = ", ".join(contrast["changed_flags"]) or "none"
        lines.append(f"| {contrast['treatment']} / {contrast['reference']} | {changed} | {contrast['workload']} | {contrast['policy']} | {contrast['metric']} | {e['percent_change_of_medians']:.3f} | {e['independent_bootstrap_ci95'][0]:.3f}–{e['independent_bootstrap_ci95'][1]:.3f} |")
    lines += ["", "## Prediction error versus latency", "", "| Workload | Policy | Error | Latency | n | Pearson r |", "| --- | --- | --- | --- | --- | --- |"]
    for correlation in report["primary"]["prediction_latency_correlations"]:
        value = f"{correlation['pearson']:.3f}" if correlation["pearson"] is not None else "—"
        lines.append(f"| {correlation['workload']} | {correlation['policy']} | {correlation['error']} | {correlation['latency']} | {correlation['n']} | {value} |")
    lines += ["", "## Cautions", ""] + ["- " + caution for caution in CAUTIONS]
    return "\n".join(lines) + "\n"


def study_argument(value):
    label, separator, path = value.partition("=")
    if not separator or not label or not path:
        raise argparse.ArgumentTypeError("expected LABEL=PATH")
    return label, Path(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--primary", type=Path, required=True)
    parser.add_argument("--study", type=study_argument, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-partial", action="store_true")
    args = parser.parse_args()
    primary = collect(args.primary.resolve(), "primary")
    studies = [collect(path.resolve(), label) for label, path in args.study]
    for study in studies:
        require_compatible(primary, study)
    if not args.allow_partial:
        if not primary["complete"] or any(not study["complete"] for study in studies):
            raise RuntimeError("incomplete experiment; wait for all planned trials or explicitly use --allow-partial")
        if len(primary["trials"]) != 280 or set(primary["manifest"]["arguments"]["policies"].split(",")) != set(POLICIES) or set(primary["manifest"]["arguments"]["workloads"].split(",")) != set(WORKLOADS) or int(primary["manifest"]["arguments"]["repetitions"]) != 5:
            raise RuntimeError("primary must contain 8 workloads × 7 policies × 5 trials")
    report = {"primary": analyze_dataset(primary), "studies": [analyze_dataset(study) for study in studies],
              "study_contrasts": study_contrasts(primary, studies), "cautions": CAUTIONS,
              "analysis": {"method": "10,000 seeded trial-level bootstrap resamples of the median; separate-session comparisons use independent resampling",
                           "code_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    (args.output / "report.md").write_text(render_markdown(report))
    with (args.output / "trials.csv").open("w", newline="") as handle:
        columns = ["dataset", "repetition", "workload", "policy", "throughput_unit", *METRICS]
        writer = csv.DictWriter(handle, fieldnames=columns, lineterminator="\n")
        writer.writeheader()
        for dataset in [primary, *studies]:
            for trial in dataset["trials"]:
                writer.writerow({"dataset": dataset["label"], "repetition": trial["repetition"], "workload": trial["workload"],
                                 "policy": trial["policy"], "throughput_unit": trial["throughput_unit"],
                                 **{metric: get(trial, metric) for metric in METRICS}})
    print(f"Analyzed {len(primary['trials'])} primary trials and {sum(len(study['trials']) for study in studies)} study trials -> {args.output}")


if __name__ == "__main__":
    main()
