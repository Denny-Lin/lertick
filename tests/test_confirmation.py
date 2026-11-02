#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise restoration on exceptions without touching kernel configuration."""
import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "benchmarks"))
from run_model_confirmation import plan, temporary_preemption


class KernelMode:
    def __init__(self):
        self.mode = "lazy"
        self.writes = []
        self.fail_first_write = False

    def read_text(self):
        return "full (lazy)" if self.mode == "lazy" else "(full) lazy"

    def write_text(self, text):
        self.mode = text.strip()
        self.writes.append(self.mode)
        if self.fail_first_write and len(self.writes) == 1:
            raise OSError("write completed but completion reported an error")


class State:
    def __init__(self, text="disabled"):
        self.text = text

    def read_text(self):
        return self.text


class Confirmation(unittest.TestCase):
    def test_child_failure_restores_original(self):
        kernel = KernelMode()
        with self.assertRaisesRegex(ValueError, "child failed"):
            with temporary_preemption("full", kernel, State()):
                self.assertEqual(kernel.mode, "full")
                raise ValueError("child failed")
        self.assertEqual(kernel.writes, ["full", "lazy"])
        self.assertEqual(kernel.mode, "lazy")

    def test_write_error_also_restores_original(self):
        kernel = KernelMode()
        kernel.fail_first_write = True
        with self.assertRaises(OSError):
            with temporary_preemption("full", kernel, State()):
                self.fail("failed write entered the benchmark")
        self.assertEqual(kernel.mode, "lazy")

    def test_active_scheduler_refused_before_mutation(self):
        kernel = KernelMode()
        with self.assertRaisesRegex(RuntimeError, "active scheduler"):
            with temporary_preemption("full", kernel, State("enabled")):
                self.fail("active scheduler entered the benchmark")
        self.assertEqual(kernel.writes, [])

    def test_plan_keeps_five_repetitions_and_observer_factor(self):
        args = argparse.Namespace(stage="observer-lazy", workloads=None, modes=None,
                                  seed=2026110201, burst_seed=2026110211)
        sessions = plan(args)
        self.assertEqual([s["prefault_samples"] for s in sessions], [False, True])
        self.assertTrue(all(s["repetitions"] == 5 and s["mode"] == "lazy" for s in sessions))
        args.workloads = "memory"
        with self.assertRaises(ValueError):
            plan(args)


if __name__ == "__main__":
    unittest.main()
