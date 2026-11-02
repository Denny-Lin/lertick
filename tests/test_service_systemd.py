#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Opt-in real PID 1 service test in an idle disposable QEMU/KVM VM.

Uses the actual scheduler and shared-map paths, with a uniquely named validation
unit, prefix, configuration and deployment lock. Never starts production units,
forces a scheduler abort, changes a kernel or controls physical CPUFreq files.
There is no Lerwatt peer: an opted-in service must report waiting and continue
scheduling independently. This is a lifecycle test, not a long hardware run.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid


STATE = Path("/sys/kernel/sched_ext/state")
PIN_DIRECTORY = Path("/sys/fs/bpf/lertick")
PINS = tuple(PIN_DIRECTORY / name for name in ("lw_contract", "lt_demand"))


def call(*arguments, check=True, env=None):
    result = subprocess.run(list(map(str, arguments)), capture_output=True,
                            text=True, timeout=90, env=env)
    if check and result.returncode:
        raise AssertionError(f"command failed: {arguments!r}\n{result.stdout}\n{result.stderr}")
    return result


def wait_for(predicate, message, seconds=35):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ValueError):
            pass
        time.sleep(.05)
    raise AssertionError(message)


def virtual_machine():
    for name in ("/sys/class/dmi/id/product_name", "/sys/class/dmi/id/sys_vendor",
                 "/sys/hypervisor/type", "/proc/device-tree/model"):
        try:
            value = Path(name).read_text().lower()
        except OSError:
            continue
        if any(word in value for word in ("qemu", "kvm", "linux,dummy-virt")):
            return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", type=Path, default=Path("build/scx_lertick"))
    parser.add_argument("--allow-systemd-test", action="store_true")
    args = parser.parse_args()
    if not args.allow_systemd_test or sys.platform != "linux" or os.geteuid() != 0 or not virtual_machine():
        parser.error("requires explicit --allow-systemd-test as root in a disposable QEMU/KVM VM")
    if not Path("/run/systemd/system").is_dir() or Path("/proc/1/comm").read_text().strip() != "systemd":
        parser.error("a running systemd PID 1 is required")
    if not STATE.exists() or STATE.read_text().strip() != "disabled":
        parser.error("sched_ext must be idle; no existing scheduler will be stopped")
    if call("systemctl", "is-active", "--quiet", "lerwatt.service", check=False).returncode == 0:
        parser.error("this absent-peer test requires the production Lerwatt service to be inactive")
    if any(path.exists() for path in PINS) or (PIN_DIRECTORY.exists() and any(PIN_DIRECTORY.iterdir())):
        parser.error("existing Lertick pins must be removed by their owner before this test")
    if call("stat", "-f", "--format=%T", "/sys/fs/bpf").stdout.strip() != "bpf_fs":
        parser.error("/sys/fs/bpf must already be mounted as bpffs")
    if not shutil.which("bpftool"):
        parser.error("bpftool is required to identify the validation maps")
    source = Path(__file__).resolve().parents[1]
    loader = args.loader.resolve()
    if not loader.is_file() or not os.access(loader, os.X_OK):
        parser.error("--loader must identify an already-built native loader")
    token = uuid.uuid4().hex[:12]
    label = "lertick-validation-" + token
    unit = label + ".service"
    prefix = Path("/opt") / label
    work = Path("/var/lib") / label
    lock = Path("/run") / (label + "-deploy")
    config_dir = work / "configuration"
    config = config_dir / "integration.conf"
    unit_file = Path("/etc/systemd/system") / unit
    for path in (prefix, work, lock, unit_file):
        assert not path.exists(), f"validation target already exists: {path}"
    work.mkdir(mode=0o700)
    checkout = work / "source"
    checkout.mkdir()
    for directory in ("build", "scripts", "templates", "src/userspace"):
        (checkout / directory).mkdir(parents=True)
    shutil.copy2(loader, checkout / "build/scx_lertick")
    (checkout / "build/scx_lertick").chmod(0o755)
    (checkout / "Makefile").write_text("# test-only already-built source tree\n")
    (checkout / "src/userspace/scx_lertick.c").write_text("/* test-only source sentinel */\n")
    for name in ("deploy.sh", "scripts/service_manager.sh", "templates/lertick.service.in"):
        text = (source / name).read_text().replace("lertick.service", unit)
        if name == "scripts/service_manager.sh":
            assert "lock_dir=/run/lertick-deploy" in text and "config_dir=/etc/lertick" in text
            text = text.replace("lock_dir=/run/lertick-deploy", "lock_dir=" + str(lock))
            text = text.replace("config_dir=/etc/lertick", "config_dir=" + str(config_dir))
        (checkout / name).write_text(text)
        (checkout / name).chmod(0o755 if name.endswith(".sh") else 0o644)
    # The test-only unit-name replacement also changes the helpers' source
    # template filename and their fixed immutable-bundle unit filename.
    shutil.copy2(checkout / "templates/lertick.service.in", checkout / "templates" / (unit + ".in"))
    shutil.copy2(source / "scripts/collect_report.py", checkout / "scripts/collect_report.py")
    (checkout / "scripts/collect_report.py").chmod(0o755)
    environment = dict(os.environ, PREFIX=str(prefix), DESTDIR="")
    helper = checkout / "deploy.sh"
    installed_helper = prefix / "bin/lertick"

    def property_value(name):
        return call("systemctl", "show", unit, "--property", name, "--value").stdout.strip()

    def ready(previous=0, link="waiting"):
        pid = int(property_value("MainPID") or "0")
        return pid if (pid and pid != previous and property_value("ActiveState") == "active"
                       and "Lerwatt: " + link in property_value("StatusText")) else 0

    def command_line(pid):
        return Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")[:-1]

    def require_default():
        wait_for(lambda: STATE.read_text().strip() == "disabled", "Linux default scheduling was not restored")
        wait_for(lambda: all(not path.exists() for path in PINS), "owned shared maps were not unpinned")

    def map_ids():
        values = []
        for path in PINS:
            value = json.loads(call("bpftool", "-j", "map", "show", "pinned", path).stdout)
            if isinstance(value, list):
                value = value[0]
            values.append(value["id"])
        return values

    result = {}
    started = time.monotonic()
    try:
        call(helper, "install", env=environment)
        assert property_value("ActiveState") == "inactive"
        assert call("systemctl", "is-enabled", unit, check=False).stdout.strip() == "disabled"
        require_default()
        assert config.read_text() == "lerwatt=auto\n"
        result["install_does_not_start_or_enable"] = True

        assert property_value("ActiveState") == "inactive"
        require_default()
        call(installed_helper, "enable", env=environment)
        first_pid = wait_for(ready, "enabled integration service did not become ready/waiting")
        command = command_line(first_pid)
        assert command[command.index(b"--duration") + 1] == b"0" and b"--lerwatt-auto" in command
        assert "/lib/lertick/releases/" in os.readlink(f"/proc/{first_pid}/exe")
        assert STATE.read_text().strip() == "enabled"
        assert call("systemctl", "is-enabled", unit).stdout.strip() == "enabled"
        first_maps = map_ids()
        time.sleep(2)
        assert ready() == first_pid, "continuous service unexpectedly changed process"
        result["enabled_unlimited_ready_with_optional_peer_absent"] = True
        print("systemd: installed helper enabled an unlimited validated worker; absent peer reports waiting", flush=True)

        call(installed_helper, "integration", "off", env=environment)
        second_pid = wait_for(lambda: ready(first_pid, "off"), "active integration disable did not produce a ready worker")
        assert b"--lerwatt" not in command_line(second_pid)
        assert STATE.read_text().strip() == "enabled" and all(not path.exists() for path in PINS)
        assert call("systemctl", "is-enabled", unit).stdout.strip() == "enabled"
        call(installed_helper, "integration", "on", env=environment)
        third_pid = wait_for(lambda: ready(second_pid), "active integration enable did not produce a ready worker")
        assert b"--lerwatt" in command_line(third_pid) and map_ids() != first_maps
        result["active_integration_toggle_restarts_only_owned_service"] = True
        print("systemd: active off/on preserved scheduling and boot intent, and replaced shared maps", flush=True)

        call(installed_helper, "stop", env=environment)
        require_default()
        assert property_value("ActiveState") == "inactive"
        assert call("systemctl", "is-enabled", unit).stdout.strip() == "enabled"
        current_marker = prefix / "lib/lertick/current"
        old_revision = current_marker.read_text()
        with helper.open("a") as stream:
            stream.write("\n# test-only helper revision for complete-bundle update\n")
        call(helper, "update", env=environment)
        assert current_marker.read_text() != old_revision and config.read_text() == "lerwatt=on\n"
        assert property_value("ActiveState") == "inactive"
        assert call("systemctl", "is-enabled", unit).stdout.strip() == "enabled"
        require_default()
        call(installed_helper, "rollback", env=environment)
        assert current_marker.read_text() == old_revision and config.read_text() == "lerwatt=on\n"
        assert property_value("ActiveState") == "inactive"
        assert call("systemctl", "is-enabled", unit).stdout.strip() == "enabled"
        require_default()
        call(installed_helper, "start", env=environment)
        wait_for(lambda: ready(third_pid), "retained integration setting was not used after rollback/start")
        result["stop_update_rollback_preserve_configuration_and_intent"] = True
        print("systemd: stop/update/rollback preserved enabled-but-stopped intent and private integration setting", flush=True)

        call(installed_helper, "uninstall", env=environment)
        require_default()
        assert not unit_file.exists() and not installed_helper.exists() and not (prefix / "sbin/scx_lertick").exists()
        assert config.read_text() == "lerwatt=on\n"
        assert call("systemctl", "is-enabled", unit, check=False).returncode != 0
        result["uninstall_disables_detaches_removes_and_retains_config"] = True
        print("systemd: uninstall disabled/detached/removed the validation service; configuration retained", flush=True)
        environment_result = {
            "os_release": Path("/etc/os-release").read_text(),
            "architecture": os.uname().machine,
            "kernel": os.uname().release,
            "systemd": call("systemctl", "--version").stdout.splitlines()[0],
            "elapsed_seconds": round(time.monotonic() - started, 2),
        }
        print(json.dumps({"result": "passed", "checks": result, "environment": environment_result,
                          "scope": "real sched_ext attachment and PID 1 lifecycle in an opted-in VM; no physical power controls or long hardware run"}), flush=True)
    finally:
        # Only this UUID-named service is controlled. If fallback cannot be
        # confirmed, retain its files for repair instead of using global aborts.
        call("systemctl", "stop", unit, check=False)
        call("systemctl", "disable", unit, check=False)
        if STATE.read_text().strip() == "disabled" and all(not path.exists() for path in PINS):
            if unit_file.exists():
                assert unit_file.read_text().splitlines()[0] == "# Lertick managed unit; prefix=" + str(prefix)
                unit_file.unlink()
            call("systemctl", "daemon-reload", check=False)
            call("systemctl", "reset-failed", unit, check=False)
            for path in (prefix, work, lock):
                shutil.rmtree(path, ignore_errors=True)
        else:
            print(f"Validation cleanup retained files for repair: {prefix} {work} {unit_file}; no other scheduler was stopped", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
