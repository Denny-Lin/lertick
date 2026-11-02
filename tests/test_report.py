#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location("report", Path(__file__).resolve().parents[1] / "scripts/collect_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class ReportTests(unittest.TestCase):
    def test_default_report_allowlists_sensitive_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "home/PRIVATE_USER/install"
            store = prefix / "lib/lertick"
            manifest = hashlib.sha256(b"loader").hexdigest() + "  scx_lertick\n"
            revision = hashlib.sha256(manifest.encode()).hexdigest()
            release = store / "releases" / revision
            release.mkdir(parents=True)
            (release / "manifest.sha256").write_text(manifest)
            (store / "current").write_text(revision)
            (store / "previous").write_text("PRIVATE_USER")
            (prefix / "sbin").mkdir()
            (prefix / "sbin/scx_lertick").write_bytes(b"loader")
            logs = json.dumps({"event": "snapshot", "stats": {"runs": 42, "PRIVATE_TASK": 7,
                              "runtime_ns": "PRIVATE_TEXT", "inferences": True},
                              "tasks": [{"pid": 987654, "comm": "PRIVATE_TASK"}]}) + "\nPRIVATE_ERROR\n"
            def runner(args):
                return {"available": True, "truncated": False,
                        "text": logs if args[0] == "journalctl" else "ActiveState=active\nMainPID=987654\nExecStart=/home/PRIVATE_USER/bin\n"}
            result = report.collect(prefix, root=root, runner=runner)
            serialized = json.dumps(result)
            for secret in ("PRIVATE_USER", "PRIVATE_TASK", "PRIVATE_TEXT", "PRIVATE_ERROR", "987654", "MainPID", "ExecStart"):
                self.assertNotIn(secret, serialized)
            self.assertEqual(result["latest_aggregate"], {"runs": 42})
            self.assertTrue(result["releases"]["current"]["installed_loader_matches"])
            self.assertTrue(result["releases"]["current"]["manifest_matches_id"])
            self.assertFalse(result["releases"]["current"]["installed_bundle_matches"])
            self.assertIn("PRIVATE_ERROR", report.collect(prefix, True, root=root, runner=runner)["raw_logs"])

    def test_no_systemd_or_permission_is_nonfatal(self):
        with tempfile.TemporaryDirectory() as directory:
            result = report.collect(Path(directory), root=Path(directory), runner=lambda _: {
                "available": False, "text": "SECRET_PERMISSION_ERROR", "truncated": False})
            self.assertIsNone(result["latest_aggregate"])
            self.assertNotIn("SECRET_PERMISSION_ERROR", json.dumps(result))

    def test_complete_bundle_detects_partial_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "opt/lertick"
            paths = {"scx_lertick": prefix / "sbin/scx_lertick", "lertick": prefix / "bin/lertick",
                     "service_manager.sh": prefix / "libexec/lertick/service_manager.sh",
                     "collect_report.py": prefix / "libexec/lertick/collect_report.py",
                     "lertick.service": root / "etc/systemd/system/lertick.service"}
            manifest = ""
            for name, path in paths.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(name)
                manifest += hashlib.sha256(name.encode()).hexdigest() + "  " + name + "\n"
            revision = hashlib.sha256(manifest.encode()).hexdigest()
            store = prefix / "lib/lertick"
            release = store / "releases" / revision
            release.mkdir(parents=True)
            (release / "manifest.sha256").write_text(manifest)
            (store / "current").write_text(revision)
            runner = lambda _: {"available": False, "text": "", "truncated": False}
            current = lambda: report.collect(prefix, root=root, runner=runner)["releases"]["current"]
            self.assertTrue(current()["installed_bundle_matches"])
            paths["service_manager.sh"].write_text("different release")
            self.assertFalse(current()["installed_bundle_matches"])
            self.assertTrue(current()["installed_loader_matches"])

    def test_bounded_subprocess(self):
        large = report.run_bounded([sys.executable, "-c", "print('x' * 1000000)"], limit=1024)
        self.assertEqual(len(large["text"]), 1024)
        self.assertTrue(large["truncated"])
        start = time.monotonic()
        slow = report.run_bounded([sys.executable, "-c", "import time; time.sleep(5)"], timeout=0.1)
        self.assertLess(time.monotonic() - start, 2)
        self.assertTrue(slow["truncated"])

    def test_only_latest_snapshot_and_integer_counters(self):
        logs = '\n'.join(["not json", "[]", '{"event":"snapshot","stats":{"runs":1}}',
                          '{"event":"snapshot","stats":{"runs":2,"pid":123,"inferences":1.2}}'])
        self.assertEqual(report.aggregate(logs), {"runs": 2})


if __name__ == "__main__":
    unittest.main()
