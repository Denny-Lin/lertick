#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "benchmarks"))
from compact_confirmation import export


class CompactEvidence(unittest.TestCase):
    def source(self, root):
        source = root / "source"
        source.mkdir()
        (source / "environment.json").write_text('{"smoke_only": false}\n')
        (source / "plan.json").write_text('[[0, "wakeheavy", "shadow"]]\n')
        trial_dir = source / "r00-wakeheavy-shadow"
        trial_dir.mkdir()
        trial = {"passed": True, "repetition": 0, "workload": "wakeheavy", "policy": "shadow",
                 "window": {"measured_start_ns": 10},
                 "lertick": {"measured_counter_delta": {"prediction_samples": 2, "linear_abs_error_ns": 120}}}
        (trial_dir / "trial.json").write_text(json.dumps(trial) + "\n")
        events = [{"verifier_accepted": True}, {"event": "registered"},
                  {"event": "snapshot", "phase": "periodic", "monotonic_ns": 5,
                   "stats": {"prediction_samples": 1, "linear_abs_error_ns": 10}},
                  {"event": "snapshot", "phase": "final", "monotonic_ns": 15,
                   "stats": {"prediction_samples": 3, "linear_abs_error_ns": 130}},
                  {"event": "exit", "default_restored": True, "status": 0}]
        (trial_dir / "scheduler.jsonl").write_text("".join(json.dumps(e) + "\n" for e in events))
        return source, trial_dir

    def test_preserves_trial_bytes_and_auditable_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, trial_dir = self.source(root)
            output = root / "compact"
            result = export(source, output)
            self.assertEqual(result["complete_trials"], 1)
            compact = output / trial_dir.name
            self.assertEqual((compact / "trial.json").read_bytes(), (trial_dir / "trial.json").read_bytes())
            audit = json.loads((compact / "counter-boundaries.json").read_text())
            self.assertEqual(audit["final_stats"]["linear_abs_error_ns"] -
                             audit["baseline_stats"]["linear_abs_error_ns"], 120)

    def test_rejects_counter_tampering_before_creating_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, trial_dir = self.source(root)
            path = trial_dir / "trial.json"
            trial = json.loads(path.read_text())
            trial["lertick"]["measured_counter_delta"]["linear_abs_error_ns"] += 1
            path.write_text(json.dumps(trial))
            with self.assertRaisesRegex(ValueError, "raw boundaries"):
                export(source, root / "compact")
            self.assertFalse((root / "compact").exists())

    def test_refuses_missing_restoration_and_smoke_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, trial_dir = self.source(root)
            path = trial_dir / "scheduler.jsonl"
            path.write_text(path.read_text().replace('"default_restored": true', '"default_restored": false'))
            with self.assertRaisesRegex(ValueError, "restoration"):
                export(source, root / "compact")
            (source / "environment.json").write_text('{"smoke_only": true}\n')
            with self.assertRaisesRegex(ValueError, "smoke"):
                export(source, root / "compact")


if __name__ == "__main__":
    unittest.main()
