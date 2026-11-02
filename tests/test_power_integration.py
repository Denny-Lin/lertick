#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Explicit VM-only live Lertick/Lerwatt integration through real BPF maps.

Requires an idle disposable QEMU/KVM VM with systemd, sched_ext and at least
eight online CPUs. Only temporary simulated CPU power files are actuated.
Changes each side's model/policy option while its peer remains running, proving
interface compatibility rather than a performance or prediction improvement.
Creates uniquely named transient units; never starts either production unit,
stops another scheduler, writes physical CPUFreq controls or uses global SysRq.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid


STATE = Path("/sys/kernel/sched_ext/state")
PINS = Path("/sys/fs/bpf/lertick")


def call(*args, check=True, timeout=45):
    result = subprocess.run([str(arg) for arg in args], text=True,
                            capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(f"command failed: {args!r}\n{result.stdout}\n{result.stderr}")
    return result


def wait_for(predicate, message, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
        time.sleep(.05)
    raise AssertionError(message)


def vm_detected():
    for name in ("/sys/class/dmi/id/product_name", "/sys/class/dmi/id/sys_vendor",
                 "/sys/hypervisor/type", "/proc/device-tree/model"):
        try:
            value = Path(name).read_text().lower()
        except OSError:
            continue
        if any(marker in value for marker in ("qemu", "kvm", "linux,dummy-virt")):
            return True
    return False


def records(path):
    if not path.exists():
        return []
    result = []
    for line in path.read_text().splitlines(keepends=True):
        if line.endswith("\n") and line.strip():
            result.append(json.loads(line))
    return result


def unit_value(unit, name):
    return call("systemctl", "show", unit, "--property", name, "--value").stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-vm-integration", action="store_true")
    parser.add_argument("--loader", type=Path, default=Path("build/scx_lertick"))
    parser.add_argument("--lerwatt-repo", type=Path, required=True)
    parser.add_argument("--keep-results", action="store_true")
    args = parser.parse_args()
    if not (args.allow_vm_integration and sys.platform == "linux" and
            os.geteuid() == 0 and vm_detected()):
        parser.error("requires explicit --allow-vm-integration as root in a disposable QEMU/KVM VM")
    if not Path("/run/systemd/system").is_dir():
        parser.error("a running systemd manager is required")
    if not STATE.exists() or STATE.read_text().strip() != "disabled":
        parser.error("sched_ext must be available and idle; stop its owner through its own manager")
    if len(os.sched_getaffinity(0)) < 8:
        parser.error("the three-policy simulator requires at least eight usable online CPUs")
    if list(Path("/sys/devices/system/cpu/cpufreq").glob("policy*")):
        parser.error("this simulator test requires a VM without physical CPUFreq policies")
    for unit in ("lerwatt.service", "lertick.service"):
        if unit_value(unit, "ActiveState") in ("active", "activating", "deactivating", "reloading"):
            parser.error(f"{unit} must already be stopped; this test never stops production services")
    if any(PINS.glob("*")):
        parser.error("pre-existing Lertick pins require owner-managed cleanup before testing")
    if call("systemctl", "is-system-running", check=False).returncode not in (0, 1):
        parser.error("the systemd manager must be reachable")
    loader = args.loader.resolve()
    lwrepo = args.lerwatt_repo.resolve()
    daemon, simulator = lwrepo / "build/lerwattd", lwrepo / "build/lerwatt-sim"
    for binary in (loader, daemon, simulator):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"build the native executable first: {binary}")

    token = uuid.uuid4().hex[:12]
    work = Path(tempfile.mkdtemp(prefix="lertick-power-integration-", dir="/var/tmp"))
    work.chmod(0o700)
    simroot = work / "simulated-board"
    owned_units, owned_processes, streams = [], [], []
    lwunit = ltunit = None
    result = {"environment": {"kernel": platform.release(), "architecture": platform.machine(),
                              "os_release": Path("/etc/os-release").read_text(),
                              "systemd": call("systemd", "--version").stdout.splitlines()[0]},
              "hardware": "temporary RK3588 simulated CPU-control files; real sched_ext/BPF", "checks": {}}

    def start_unit(kind, command, watchdog=False):
        unit = f"ltlw-{token}-{kind}-{len(owned_units)}.service"
        owned_units.append(unit)
        options = ["systemd-run", "--unit", unit, "--service-type=notify",
                   "--property=NotifyAccess=main", "--property=Restart=no",
                   "--property=TimeoutStartSec=30s", "--property=TimeoutStopSec=10s",
                   "--property=KillMode=control-group", "--property=UMask=0077",
                   "--setenv=LERWATT_SIM_ROOT=" + str(simroot)]
        if watchdog:
            options.extend(["--property=WatchdogSec=5s", "--property=WatchdogSignal=SIGTERM",
                            "--property=TimeoutAbortSec=10s"])
        call(*options, "--", *command)
        wait_for(lambda: unit_value(unit, "ActiveState") == "active", f"{kind} unit did not become ready")
        pid = int(unit_value(unit, "MainPID"))
        cmdline = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        assert cmdline[cmdline.index(b"--duration") + 1] == b"0", "worker is not continuous"
        return unit

    def start_lerwatt(forecast="temporal"):
        return start_unit("lerwatt", [daemon, "--sysroot", simroot, "--duration", "0",
                                      "--policy", "mpc", "--period-ms", "100", "--soft-limit", "65",
                                      "--forecast", forecast, "--watchdog"], watchdog=True)

    def start_lertick(mode="online"):
        assert STATE.read_text().strip() == "disabled", "refusing another scheduler"
        path = work / f"lertick-{len(owned_units)}.jsonl"
        unit = start_unit("lertick", [loader, "--mode", mode, "--track-all", "--lerwatt",
                                      "--cpuperf", "util", "--duration", "0", "--notify",
                                      "--sample-ms", "100", "--summary-only", "--stats-json", path])
        row = wait_for(lambda: next((r for r in records(path) if r.get("event") == "registered"), None),
                       "registration record missing")
        assert row["contract_version"] == 2 and row["lerwatt_session_id"] > 0
        assert row["mode"] == mode
        return unit, path, row["lerwatt_session_id"]

    def stop_unit(unit):
        # A frozen loader is one of our intentional cases. Resume only the
        # identified transient unit before its ordinary stop/cleanup path.
        call("systemctl", "kill", "--kill-who=main", "--signal=SIGCONT", unit, check=False)
        call("systemctl", "stop", unit)
        assert unit_value(unit, "ActiveState") in ("inactive", "failed")

    def link(unit, state):
        return f"Lerwatt: {state}" in unit_value(unit, "StatusText") if "-lertick-" in unit else \
               f"Lertick: {state}" in unit_value(unit, "StatusText")

    def connected(stats_path):
        wait_for(lambda: link(lwunit, "connected") and link(ltunit, "connected"),
                 "the two controllers did not establish a compatible live connection")
        def connected_snapshot():
            snapshots = [row for row in records(stats_path) if row.get("phase") == "periodic"]
            return bool(snapshots and snapshots[-1].get("lerwatt_link") == "connected" and
                        snapshots[-1].get("lerwatt_matched_policies") == 3 and
                        snapshots[-1].get("contract_version") == 2)
        wait_for(connected_snapshot, "periodic diagnostics did not report three compatible policies")

    def applied(path):
        return max((row.get("stats", {}).get("contract_applied", 0) for row in records(path)), default=0)

    try:
        error = (work / "simulator.stderr").open("w")
        streams.append(error)
        process = subprocess.Popen([str(simulator), "--root", str(simroot), "--load", "pattern:full",
                                    "--ambient", "70", "--initial-temp", "75", "--duration", "0"],
                                   stdout=subprocess.DEVNULL, stderr=error)
        owned_processes.append(process)
        wait_for(lambda: (simroot / ".lerwatt-sim").exists() and
                 len(list((simroot / "sys/devices/system/cpu/cpufreq").glob("policy*/scaling_max_freq"))) == 3,
                 "simulator did not create its isolated control files")
        controls = list(simroot.glob("sys/devices/system/cpu/cpufreq/policy*/scaling_*_freq"))
        controls = [path for path in controls if path.name in ("scaling_min_freq", "scaling_max_freq")]
        controls += list(simroot.glob("sys/devices/system/cpu/cpu*/power/pm_qos_resume_latency_us"))
        original = {path: path.read_text().strip() for path in controls}

        def restored():
            return all(path.read_text().strip() == text for path, text in original.items()) and \
                not (simroot / "run/lerwatt/restore.journal").exists()

        # A modest owned workload guarantees that real BPF callbacks consume
        # the contract while summaries avoid task/PID telemetry in artifacts.
        worker = subprocess.Popen([sys.executable, "-c",
                                   "import time\nwhile True:\n sum(i*i for i in range(5000))\n time.sleep(.002)\n"],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        owned_processes.append(worker)
        lwunit = start_lerwatt()
        wait_for(lambda: link(lwunit, "waiting"), "power manager did not wait independently for its peer")
        ltunit, stats, first_session = start_lertick()
        connected(stats)
        before = applied(stats)
        wait_for(lambda: applied(stats) > before, "real scheduler never consumed a power contract")
        result["checks"]["lerwatt_first_continuous_connection"] = True
        print("integration: Lerwatt-first startup connected; BPF consumed contracts", flush=True)

        power_pid = unit_value(lwunit, "MainPID")
        stop_unit(ltunit)
        wait_for(lambda: STATE.read_text().strip() == "disabled", "Lertick stop did not restore scheduling")
        wait_for(lambda: link(lwunit, "waiting"), "Lerwatt retained stale demand after Lertick stopped")
        ltunit, stats, next_session = start_lertick(mode="ewma")
        assert next_session != first_session, "attachment session was reused"
        connected(stats)
        assert unit_value(lwunit, "MainPID") == power_pid, "scheduler policy change restarted its peer"
        before = applied(stats)
        wait_for(lambda: applied(stats) > before, "EWMA scheduler did not consume compatible power advice")
        result["checks"]["lertick_restart_new_session_reconnects"] = True
        result["checks"]["independent_scheduler_policy_change"] = True
        print("integration: online-to-EWMA scheduler change reconnected while Lerwatt stayed running", flush=True)

        # Stopping only the userspace publisher leaves the BPF scheduler alive;
        # the consumer must age demand out instead of reusing it indefinitely.
        call("systemctl", "kill", "--kill-who=main", "--signal=SIGSTOP", ltunit)
        try:
            wait_for(lambda: link(lwunit, "waiting"), "frozen publisher demand did not expire")
            assert STATE.read_text().strip() == "enabled", "publisher pause unexpectedly detached scheduler"
        finally:
            call("systemctl", "kill", "--kill-who=main", "--signal=SIGCONT", ltunit, check=False)
        connected(stats)
        result["checks"]["frozen_demand_expires_and_recovers"] = True
        print("integration: frozen demand expired and resumed without stale control", flush=True)

        scheduler_pid = unit_value(ltunit, "MainPID")
        scheduler_session = next_session
        stop_unit(lwunit)
        wait_for(restored, "stopping Lerwatt did not restore simulated controls")
        wait_for(lambda: link(ltunit, "waiting"), "Lertick retained expired Lerwatt constraints")
        assert STATE.read_text().strip() == "enabled", "power stop detached Lertick"
        lwunit = start_lerwatt(forecast="adaptive")
        connected(stats)
        assert unit_value(ltunit, "MainPID") == scheduler_pid, "power predictor change restarted its peer"
        registered = [row for row in records(stats) if row.get("event") == "registered"]
        assert len(registered) == 1 and registered[0]["lerwatt_session_id"] == scheduler_session
        before = applied(stats)
        wait_for(lambda: applied(stats) > before, "adaptive predictor restart did not resume consumed advice")
        result["checks"]["lerwatt_restart_restores_expires_reconnects"] = True
        result["checks"]["independent_power_predictor_change"] = True
        print("integration: temporal-to-adaptive predictor change preserved Lertick PID and session", flush=True)

        stop_unit(lwunit)
        wait_for(restored, "power controls did not restore")
        stop_unit(ltunit)
        wait_for(lambda: STATE.read_text().strip() == "disabled" and not any(PINS.glob("*")),
                 "scheduler/pins did not cleanly detach")
        ltunit, stats, _ = start_lertick()
        wait_for(lambda: link(ltunit, "waiting"), "scheduler-first start did not wait independently")
        lwunit = start_lerwatt()
        connected(stats)
        result["checks"]["lertick_first_connection"] = True
        print("integration: reverse startup order connected", flush=True)

        stop_unit(lwunit)
        wait_for(restored, "final simulated controls were not restored")
        stop_unit(ltunit)
        wait_for(lambda: STATE.read_text().strip() == "disabled" and not any(PINS.glob("*")),
                 "final scheduling state or pins were not restored")
        result["checks"]["final_control_restore_scheduler_fallback_pin_cleanup"] = True
        result["result"] = "passed"
        (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result), flush=True)
    finally:
        cleanup_errors = []
        for unit in reversed(owned_units):
            for command in (("kill", "--kill-who=main", "--signal=SIGCONT"),
                            ("stop",), ("reset-failed",)):
                try:
                    call("systemctl", *command, unit, check=False)
                except (OSError, subprocess.SubprocessError) as error:
                    cleanup_errors.append(str(error))
        for process in reversed(owned_processes):
            if process.poll() is None:
                try:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                except (OSError, subprocess.SubprocessError) as error:
                    cleanup_errors.append(str(error))
        for stream in streams:
            stream.close()
        if args.keep_results or result.get("result") != "passed" or cleanup_errors:
            print(f"integration artifacts: {work}", flush=True)
        else:
            shutil.rmtree(work)
        if cleanup_errors:
            raise RuntimeError("owned test cleanup incomplete: " + "; ".join(cleanup_errors))


if __name__ == "__main__":
    main()
