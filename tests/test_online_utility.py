#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "benchmarks"))
from analyze_online_utility import analyze, CANDIDATE, CONTROLS


class OnlineUtility(unittest.TestCase):
    def fixture(self, root):
        (root / "environment.json").write_text(json.dumps({"smoke_only": False}))
        plan = []
        for r in range(5):
            for policy in (*CONTROLS, CANDIDATE):
                plan.append([r, "wakeheavy", policy])
                d = root / f"r{r:02d}-wakeheavy-{policy}"
                d.mkdir()
                candidate = policy == CANDIDATE
                # Baseline median is 1, candidate median is 20. Paired median
                # change is +100%, distinct from the +1900% ratio of medians.
                left = [1, 1, 1, 100, 100][r]
                right = [20, 20, .5, 200, 200][r]
                a = {"passed": True, "repetition": r, "workload": "wakeheavy", "policy": policy,
                     "throughput_units_per_s": right if candidate else left,
                     "wake_to_run_ns": {"p99": 5 if candidate else 10},
                     "burst_completion_ns": {"p99": 30 if candidate else 20},
                     "lertick": {"model_init": "ewma" if candidate else "bias",
                                 "model_update": "carry" if candidate else "trunc"}}
                (d / "trial.json").write_text(json.dumps(a))
        (root / "plan.json").write_text(json.dumps(plan))

    def test_paired_units_and_opposite_metric_directions(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); self.fixture(root)
            report = analyze(root)
            self.assertEqual(report["complete_trials"], 20)
            for group in report["groups"]:
                m = group["metrics"]
                self.assertEqual(m["throughput_units_per_s"]["paired_percent_change"]["median"], 100)
                self.assertEqual(m["wake_to_run_ns.p99"]["paired_percent_change"]["median"], -50)
                self.assertEqual(m["burst_completion_ns.p99"]["paired_percent_change"]["median"], 50)

    def test_zero_denominators_and_missing_pairs_remain_visible(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); self.fixture(root)
            p = root / "r00-wakeheavy-fixed/trial.json"
            a = json.loads(p.read_text()); a["throughput_units_per_s"] = 0; p.write_text(json.dumps(a))
            m = next(g for g in analyze(root)["groups"] if g["control"] == "fixed")["metrics"]["throughput_units_per_s"]
            self.assertEqual((m["defined_percent_pairs"], m["total_pairs"]), (4, 5))
            a["passed"] = False; p.write_text(json.dumps(a))
            with self.assertRaisesRegex(ValueError, "failed or mismatched"):
                analyze(root)
            a["passed"] = True; p.write_text(json.dumps(a))
            p = root / "plan.json"
            plan = json.loads(p.read_text()); plan.remove([0, "wakeheavy", "online"])
            p.write_text(json.dumps(plan))
            with self.assertRaisesRegex(ValueError, "every declared"):
                analyze(root)


if __name__ == "__main__":
    unittest.main()
