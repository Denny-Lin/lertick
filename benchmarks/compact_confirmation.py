#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Retain every trial and audit its counter boundaries without duplicating raw logs.

Full raw archives remain separately retained. This export preserves inputs to
the paired model analysis; it is not a replacement for per-job latency traces.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil


def read(path):
    return json.loads(path.read_text())


def boundaries(directory, trial):
    path = directory / "scheduler.jsonl"
    if not path.exists():
        path = directory / "scheduler.jsonl.gz"
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt") as f:
        text = f.read()
    if not text.endswith("\n"):
        raise ValueError(f"incomplete scheduler log: {path}")
    events = [json.loads(line) for line in text.splitlines() if line.strip().startswith("{")]
    if not events or not events[-1].get("default_restored") or events[-1].get("status") != 0:
        raise ValueError(f"missing successful scheduler restoration: {directory}")
    if not any(event.get("verifier_accepted") for event in events):
        raise ValueError(f"missing verifier acceptance: {directory}")
    snapshots = [event for event in events if event.get("event") == "snapshot"]
    final = next((event for event in reversed(snapshots) if event.get("phase") == "final"), None)
    if final is None:
        raise ValueError("missing final counter snapshot")
    previous = [event for event in snapshots if event["monotonic_ns"] <= trial["window"]["measured_start_ns"]]
    first = previous[-1] if previous else {"stats": {}, "monotonic_ns": None}
    delta = {key: value - first["stats"].get(key, 0) for key, value in final["stats"].items()}
    if delta != trial["lertick"]["measured_counter_delta"]:
        raise ValueError(f"global counter delta does not match its raw boundaries: {directory}")
    return {"baseline_monotonic_ns": first["monotonic_ns"], "baseline_stats": first["stats"],
            "final_monotonic_ns": final["monotonic_ns"], "final_stats": final["stats"],
            "registered": next((event for event in events if event.get("event") == "registered"), None),
            "exit": events[-1], "scheduler_jsonl_sha256": hashlib.sha256(text.encode()).hexdigest(),
            "window_note": "last one-second live snapshot before measured window through final unload; approximate"}


def export(source, output, archive=None):
    source = source.resolve()
    environment = read(source / "environment.json")
    if environment.get("smoke_only"):
        raise ValueError("smoke data cannot be exported as confirmation evidence")
    plan = read(source / "plan.json")
    if not plan:
        raise ValueError("no planned trials")
    prepared, seen = [], set()
    for repetition, workload, policy in plan:
        if (not isinstance(repetition, int) or repetition < 0 or
                not all(isinstance(s, str) and re.fullmatch(r"[a-z][a-z0-9_-]*", s) for s in (workload, policy))):
            raise ValueError("invalid trial identity in the source plan")
        key = repetition, workload, policy
        if key in seen:
            raise ValueError("duplicate trial in the source plan")
        seen.add(key)
        name = f"r{repetition:02d}-{workload}-{policy}"
        directory = source / name
        trial = read(directory / "trial.json")
        if not trial.get("passed") or (trial.get("repetition"), trial.get("workload"), trial.get("policy")) != key:
            raise ValueError(f"failed or mismatched trial: {directory}")
        audit = boundaries(directory, trial) if trial.get("lertick") else None
        prepared.append((name, trial, audit))
    output.mkdir(parents=True, exist_ok=False)
    for name in ("environment.json", "plan.json", "summary.json"):
        path = source / name
        if path.exists():
            shutil.copyfile(path, output / name)
    for name, trial, audit in prepared:
        directory = output / name
        directory.mkdir()
        shutil.copyfile(source / name / "trial.json", directory / "trial.json")
        if audit is not None:
            (directory / "counter-boundaries.json").write_text(json.dumps(audit, indent=2) + "\n")
    manifest = {"source_name": source.parent.name + "/" + source.name,
                "complete_trials": len(prepared), "performance_claim": False,
                "all_planned_trials_retained": True,
                "limits": "Full raw logs retained separately; per-job percentile recomputation needs those logs."}
    if archive:
        manifest["raw_archive"] = {"name": archive.name, "bytes": archive.stat().st_size,
                                   "sha256": hashlib.sha256(archive.read_bytes()).hexdigest()}
    (output / "compact.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--raw-archive", type=Path)
    args = parser.parse_args()
    print(json.dumps(export(args.input, args.output, args.raw_archive), indent=2))


if __name__ == "__main__":
    main()
