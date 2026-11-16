#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Print a local, allowlisted diagnostic report. Never upload or run a loader."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import subprocess
import time

LIMIT = 256 * 1024
STATS = frozenset("""local_enqueue global_enqueue runs runtime_ns completed_bursts
censored_bursts training_updates inferences adaptive_slices slice_changes fault_stalls
counter_errors alloc_failures snapshot_failures prediction_ns_sum selected_slice_ns_sum
matching_dispatch_slices continuation_slices continuation_pending_ns init_tasks exit_tasks
training_time_ns inference_time_ns prediction_samples linear_abs_error_ns ewma_abs_error_ns
wake_samples wake_to_run_ns wake_to_run_max_ns cpuperf_sets cpuperf_idle_sets contract_applied
contract_stale live_tasks""".split())
PROPERTIES = ("LoadState", "ActiveState", "SubState", "UnitFileState", "Result",
              "ExecMainCode", "ExecMainStatus", "NRestarts")
CONFIG = ("CONFIG_BPF", "CONFIG_BPF_SYSCALL", "CONFIG_BPF_JIT", "CONFIG_DEBUG_INFO_BTF",
          "CONFIG_SCHED_CLASS_EXT")


def read_text(path, limit=LIMIT):
    try:
        with open(path, "rb") as stream:
            data = stream.read(limit + 1)
        if len(data) > limit:
            return None
        return data.decode("utf-8", errors="replace")
    except OSError:
        return None


def digest(path, limit=64 * 1024 * 1024):
    try:
        result = hashlib.sha256()
        count = 0
        with open(path, "rb") as stream:
            while True:
                data = stream.read(65536)
                if not data:
                    return result.hexdigest()
                count += len(data)
                if count > limit:
                    return None
                result.update(data)
    except OSError:
        return None


def run_bounded(args, timeout=5, limit=LIMIT):
    """Drain at most limit bytes and reap only our own subprocess on timeout."""
    try:
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                   env={**os.environ, "SYSTEMD_PAGER": "cat", "LC_ALL": "C"})
    except OSError:
        return {"text": "", "available": False, "truncated": False}
    output = bytearray()
    truncated = False
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                truncated = True
                break
            if not selector.select(remaining):
                truncated = True
                break
            chunk = os.read(process.stdout.fileno(), min(65536, limit + 1 - len(output)))
            if not chunk:
                break
            output.extend(chunk)
            if len(output) > limit:
                truncated = True
                break
    if truncated:
        process.kill()
    elif process.poll() is None:
        # Even after EOF a broken subprocess may remain alive. Keep the report bounded.
        try:
            process.wait(timeout=max(0.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            process.kill()
    process.wait()
    process.stdout.close()
    return {"text": bytes(output[:limit]).decode("utf-8", errors="replace"),
            "available": process.returncode == 0, "truncated": truncated}


def aggregate(text):
    latest = None
    for line in text.splitlines():
        try:
            entry = json.loads(line)
        except (ValueError, RecursionError):
            continue
        if not isinstance(entry, dict) or entry.get("event") != "snapshot":
            continue
        stats = entry.get("stats")
        if not isinstance(stats, dict):
            continue
        latest = {key: value for key, value in stats.items()
                  if key in STATS and type(value) is int and -(2**64) < value < 2**64}
    return latest


def collect(prefix=Path("/usr/local"), include_logs=False, root=Path("/"), runner=run_bounded):
    def at(path):
        return root / path.lstrip("/")

    uname = os.uname()
    report = {"schema": 1, "privacy": {"raw_logs_included": include_logs,
              "automatic_upload": False}, "system": {"kernel": uname.release,
              "architecture": uname.machine, "logical_cpus": os.cpu_count()}, "notes": []}
    os_release = read_text(at("/etc/os-release"), 16384)
    if os_release:
        for line in os_release.splitlines():
            key, separator, value = line.partition("=")
            if separator and key in ("NAME", "VERSION_ID"):
                try:
                    fields = shlex.split(value)
                    report["system"][key.lower()] = " ".join(fields)[:120]
                except ValueError:
                    pass
    config = read_text(at("/boot/config-" + uname.release)) if "/" not in uname.release else None
    if config is None:
        try:
            with gzip.open(at("/proc/config.gz"), "rt") as stream:
                config = stream.read(LIMIT + 1)
            if len(config) > LIMIT:
                config = None
        except (OSError, EOFError):
            pass
    if config:
        report["system"]["kernel_config"] = {
            key: ("y" if key + "=y" in config.splitlines() else
                  "m" if key + "=m" in config.splitlines() else "unset_or_unknown") for key in CONFIG}
    btf = digest(at("/sys/kernel/btf/vmlinux"))
    if btf:
        report["system"]["btf_sha256"] = btf
    state = (read_text(at("/sys/kernel/sched_ext/state"), 128) or "unavailable").strip()
    ops = (read_text(at("/sys/kernel/sched_ext/root/ops"), 128) or "").strip()
    report["scheduler"] = {"state": state if state in ("enabled", "disabled", "enabling", "disabling") else "unavailable",
                           "lertick_attached": state == "enabled" and ops == "scx_lertick"}
    store = prefix / "lib/lertick"
    releases = {}
    for marker in ("current", "previous"):
        revision = (read_text(store / marker, 128) or "").strip()
        if not re.fullmatch("[a-f0-9]{64}", revision):
            continue
        manifest = store / "releases" / revision / "manifest.sha256"
        item = {"bundle_sha256": revision, "manifest_matches_id": digest(manifest) == revision}
        content = read_text(manifest, 4096) or ""
        expected = {}
        for line in content.splitlines():
            record = re.fullmatch(r"([a-f0-9]{64})  (scx_lertick|lertick|service_manager\.sh|lertick\.service|collect_report\.py)", line)
            if record:
                expected[record.group(2)] = record.group(1)
            match = re.fullmatch(r"([a-f0-9]{64})  scx_lertick", line)
            if match:
                item["loader_sha256"] = match.group(1)
                if marker == "current":
                    item["installed_loader_matches"] = digest(prefix / "sbin/scx_lertick") == match.group(1)
        if marker == "current":
            paths = {"scx_lertick": prefix / "sbin/scx_lertick", "lertick": prefix / "bin/lertick",
                     "service_manager.sh": prefix / "libexec/lertick/service_manager.sh",
                     "collect_report.py": prefix / "libexec/lertick/collect_report.py",
                     "lertick.service": at("/etc/systemd/system/lertick.service")}
            item["installed_bundle_matches"] = (item["manifest_matches_id"] and
                {"scx_lertick", "lertick", "service_manager.sh", "lertick.service"}.issubset(expected) and
                len(expected) == len(content.splitlines()) and
                all(digest(paths[name]) == value for name, value in expected.items()))
        releases[marker] = item
    report["releases"] = releases
    service = runner(["systemctl", "show", "lertick.service", "--no-pager"] +
                     ["--property=" + field for field in PROPERTIES])
    report["service"] = {}
    if service["available"]:
        for line in service["text"].splitlines():
            key, _, value = line.partition("=")
            if key in PROPERTIES and re.fullmatch(r"[a-zA-Z0-9_-]{0,64}", value):
                report["service"][key] = value
    else:
        report["notes"].append("Service status unavailable; no privilege elevation attempted.")
    journal = runner(["journalctl", "-u", "lertick.service", "-b", "-n", "200", "--no-pager", "--output=cat"])
    report["latest_aggregate"] = aggregate(journal["text"]) if journal["available"] else None
    if not journal["available"] or report["latest_aggregate"] is None:
        report["notes"].append("Recent aggregate unavailable; journal access may require sudo or journal group membership.")
    if journal["truncated"]:
        report["notes"].append("Recent journal output reached the size or time limit.")
    if include_logs:
        report["raw_logs"] = journal["text"]
        report["notes"].append("Raw logs can contain task names, PIDs and identifying text. Review before sharing.")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, default=Path("/usr/local"))
    parser.add_argument("--include-logs", action="store_true", help="include bounded raw journal messages; review before sharing")
    args = parser.parse_args()
    if not args.prefix.is_absolute():
        parser.error("--prefix must be absolute")
    print(json.dumps(collect(args.prefix, args.include_logs), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
