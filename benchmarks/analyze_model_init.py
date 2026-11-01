#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Paired burst-model comparisons; trial, not episode, is the unit."""
import argparse
import json
from pathlib import Path

from run_phase2 import bootstrap_median, LERTICK_POLICIES


def read(path):
    return json.loads(path.read_text())


def metrics(trial):
    delta = trial["lertick"]["measured_counter_delta"]
    n = delta["prediction_samples"]
    if n <= 0:
        raise ValueError("no eligible prediction labels in a learning trial")
    mae = delta["linear_abs_error_ns"] / n
    ewma = delta["ewma_abs_error_ns"] / n
    return {"global_model_mae_ns": mae, "same_episode_ewma_mae_ns": ewma,
            "model_over_ewma": mae / ewma if ewma else None,
            "prediction_samples": n,
            "throughput_units_per_s": trial.get("throughput_units_per_s"),
            "wake_p99_ns": (trial.get("wake_to_run_ns") or {}).get("p99"),
            "completion_p99_ns": (trial.get("burst_completion_ns") or {}).get("p99")}


def analyze(source, mode, base=None, prior=None):
    environment = read(source / "environment.json")
    if environment.get("smoke_only"):
        raise ValueError("smoke runs are not performance evidence")
    plan = read(source / "plan.json")
    trials = {}
    for repetition, workload, policy in plan:
        if (repetition, workload, policy) in trials:
            raise ValueError("duplicate trial in the experiment plan")
        path = source / f"r{repetition:02d}-{workload}-{policy}" / "trial.json"
        trial = read(path)
        if not trial.get("passed"):
            raise ValueError(f"failed trial: {path}")
        if (trial.get("repetition"), trial.get("workload"), trial.get("policy")) != (repetition, workload, policy):
            raise ValueError(f"trial identity does not match its plan: {path}")
        trials[repetition, workload, policy] = trial
    base, prior = base or mode, prior or mode + "-ewma-prior"
    if any(policy not in LERTICK_POLICIES or LERTICK_POLICIES[policy][0] != mode for policy in (base, prior)):
        raise ValueError("both policies must use the requested scheduler mode")
    rows, groups = [], []
    workloads = sorted({workload for _, workload, policy in plan if policy in (base, prior)})
    if not workloads:
        raise ValueError("no requested policies in the experiment plan")
    for workload in workloads:
        matched = []
        repetitions = sorted({r for r, w, policy in trials if w == workload and policy in (base, prior)})
        if len(repetitions) < 5:
            raise ValueError("at least five complete paired repetitions are required")
        for repetition in repetitions:
            if any((repetition, workload, policy) not in trials for policy in (base, prior)):
                raise ValueError("comparison requires complete paired repetitions")
            a, b = trials[repetition, workload, base], trials[repetition, workload, prior]
            for trial, policy in [(a, base), (b, prior)]:
                expected = LERTICK_POLICIES[policy]
                if trial["lertick"]["model_init"] != expected[1] or trial["lertick"].get("model_update", "trunc") != expected[2]:
                    raise ValueError("recorded model configuration does not match policy label")
            left, right = metrics(a), metrics(b)
            change = {key: 100 * (right[key] / left[key] - 1)
                      for key in left if key != "prediction_samples" and
                      left[key] is not None and right[key] is not None and left[key] != 0}
            row = {"workload": workload, "repetition": repetition, "baseline": left,
                   "candidate": right, "paired_percent_change": change}
            rows.append(row)
            matched.append(row)
        groups.append({"workload": workload, "n": len(matched),
                       "baseline": {key: bootstrap_median([r["baseline"][key] for r in matched if r["baseline"][key] is not None])
                                for key in matched[0]["baseline"]},
                       "candidate": {key: bootstrap_median([r["candidate"][key] for r in matched if r["candidate"][key] is not None])
                                      for key in matched[0]["candidate"]},
                       "paired_percent_change": {key: bootstrap_median([r["paired_percent_change"][key]
                                                   for r in matched if key in r["paired_percent_change"]])
                                                 for key in matched[0]["paired_percent_change"]}})
    return {"mode": mode, "baseline_policy": base, "candidate_policy": prior,
            "source": str(source), "environment": environment,
            "complete_trials": len(trials), "groups": groups, "paired_trials": rows,
            "limits": ["One ARM64 VM; no bare-metal/general Linux superiority claim.",
                       "Global errors compare the same verified, clipped CPU episodes within each trial.",
                       "One-second counter snapshots give approximate measurement boundaries.",
                       "Repetition is the unit; within-trial episodes are correlated.",
                       "Five paired trials give coarse exploratory bootstrap intervals.",
                       "Shadow uses fixed slices, separating prediction changes from budget decisions."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--mode", choices=("shadow", "online"), default="shadow")
    parser.add_argument("--baseline")
    parser.add_argument("--candidate")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = analyze(args.input.resolve(), args.mode, args.baseline, args.candidate)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for group in report["groups"]:
        before = group["baseline"]["global_model_mae_ns"]["median"] / 1000
        after = group["candidate"]["global_model_mae_ns"]["median"] / 1000
        paired = group["paired_percent_change"]["global_model_mae_ns"]
        print(f"{group['workload']}: {before:.3f} -> {after:.3f} us; "
              f"paired change {paired['median']:.2f}% {paired['bootstrap_median_ci95']}")


if __name__ == "__main__":
    main()
