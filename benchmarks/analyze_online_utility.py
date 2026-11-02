#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Paired candidate utility versus fixed, EWMA and original online controls."""
import argparse
import json
import math
from pathlib import Path

from run_phase2 import bootstrap_median, LERTICK_POLICIES

CANDIDATE = "online-ewma-carry"
CONTROLS = ("fixed", "ewma", "online")
FIELDS = ("throughput_units_per_s", "wake_to_run_ns.p99", "burst_completion_ns.p99")


def values(trial):
    result = {}
    for field in FIELDS:
        value = trial
        for component in field.split("."):
            value = value[component]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError("missing, negative or non-finite utility metric")
        result[field] = value
    return result


def analyze(source):
    environment = json.loads((source / "environment.json").read_text())
    if environment.get("smoke_only") is not False:
        raise ValueError("utility evidence requires a non-smoke experiment")
    plan = json.loads((source / "plan.json").read_text())
    trials = {}
    for repetition, workload, policy in plan:
        key = repetition, workload, policy
        if key in trials:
            raise ValueError("duplicate planned trial")
        trial = json.loads((source / f"r{repetition:02d}-{workload}-{policy}" / "trial.json").read_text())
        if trial.get("passed") is not True or (trial.get("repetition"), trial.get("workload"), trial.get("policy")) != key:
            raise ValueError("failed or mismatched trial")
        if policy not in LERTICK_POLICIES:
            raise ValueError("unknown policy")
        if (trial["lertick"]["model_init"], trial["lertick"]["model_update"]) != LERTICK_POLICIES[policy][1:]:
            raise ValueError("recorded model configuration does not match policy")
        trials[key] = values(trial)
    groups, pairs = [], []
    for workload in sorted({w for _, w, _ in trials}):
        repetitions = sorted({r for r, w, _ in trials if w == workload})
        if len(repetitions) < 5:
            raise ValueError("at least five complete paired repetitions required")
        for control in CONTROLS:
            matched = []
            for repetition in repetitions:
                if any((repetition, workload, p) not in trials for p in (control, CANDIDATE)):
                    raise ValueError("every declared control/candidate pair is required")
                left, right = trials[repetition, workload, control], trials[repetition, workload, CANDIDATE]
                row = {"workload": workload, "control": control, "repetition": repetition,
                       "baseline": left, "candidate": right,
                       "paired_percent_change": {k: 100 * (right[k] / left[k] - 1) if left[k] else None for k in FIELDS}}
                matched.append(row)
                pairs.append(row)
            metrics = {}
            for field in FIELDS:
                changes = [r["paired_percent_change"][field] for r in matched if r["paired_percent_change"][field] is not None]
                metrics[field] = {"baseline": bootstrap_median([r["baseline"][field] for r in matched]),
                                  "candidate": bootstrap_median([r["candidate"][field] for r in matched]),
                                  "paired_percent_change": bootstrap_median(changes),
                                  "defined_percent_pairs": len(changes), "total_pairs": len(matched)}
            groups.append({"workload": workload, "control": control, "metrics": metrics})
    if not groups:
        raise ValueError("empty utility experiment")
    return {"candidate": CANDIDATE, "complete_trials": len(trials), "groups": groups, "paired_trials": pairs,
            "environment": environment,
            "limits": ["Positive means higher; throughput and latency have opposite desirability.",
                       "Repetition is the unit; five paired trials give coarse exploratory intervals.",
                       "Zero denominators retain absolute values and undefined percentage counts.",
                       "One ARM64 VM and custom workloads; no hardware or general Linux superiority claim."]}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    a.output.write_text(json.dumps(analyze(a.input), indent=2) + "\n")


if __name__ == "__main__":
    main()
