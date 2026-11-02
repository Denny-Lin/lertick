#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Loader deployment checks. Attaching a scheduler requires explicit VM opt-in.

The default checks only parse arguments and reject pre-existing output paths.
--live additionally probes/verifies and exercises maintenance without attaching.
--allow-attach additionally changes scheduling in an idle disposable QEMU/KVM VM.
"""
import argparse
import atexit
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time

STATE = Path("/sys/kernel/sched_ext/state")
PINS = Path("/sys/fs/bpf/lertick")


def wait_for(predicate, message, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(message)


def records(path):
    if not path.exists():
        return []
    text = path.read_text()
    return [json.loads(line) for line in text.splitlines(keepends=True)
            if line.endswith("\n") and line.strip()]


def vm_detected():
    for name in ["/sys/class/dmi/id/product_name", "/sys/class/dmi/id/sys_vendor",
                 "/sys/hypervisor/type", "/proc/device-tree/model"]:
        try:
            text = Path(name).read_text().lower()
        except OSError:
            continue
        if any(marker in text for marker in ("qemu", "kvm", "linux,dummy-virt")):
            return True
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", type=Path, default=Path("build/scx_lertick"))
    parser.add_argument("--live", action="store_true", help="allow BPF verification and locked maintenance, never attach")
    parser.add_argument("--allow-attach", action="store_true", help="explicitly allow short scheduling/recovery tests in a disposable VM")
    args = parser.parse_args()
    loader = str(args.loader.resolve())

    def invoke(*options, **kwargs):
        return subprocess.run([loader, *map(str, options)], capture_output=True,
                              text=True, timeout=30, **kwargs)

    assert "--verify" in invoke("--help").stdout
    result = invoke("--version")
    assert result.returncode == 0 and result.stdout.startswith("scx_lertick ")
    for options in [ ["--duration", "0"], ["--duration", "-1"],
                     ["--duration", "4294967297"], ["--duration", "10junk"],
                     ["--sample-ms", "99"], ["--check", "--verify"],
                     ["--maintenance"], ["--maintenance", "/bin/true"],
                     ["--maintenance", "--", "relative-command"],
                     ["--verify", "--maintenance", "--", "/bin/true"],
                     ["--min-slice-us", "5000", "--fixed-slice-us", "1000"],
                     ["unexpected"], ["--fault-stall"] ]:
        assert invoke(*options).returncode == 2, options

    with tempfile.TemporaryDirectory(prefix="lertick-loader-deployment-") as directory:
        work = Path(directory)
        victim = work / "original"
        victim.write_text("keep this unchanged\n")
        symlink = work / "symlink"
        symlink.symlink_to(victim)
        hardlink = work / "hardlink"
        os.link(victim, hardlink)
        fifo = work / "fifo"
        os.mkfifo(fifo)
        for output in [victim, symlink, hardlink, fifo, work]:
            started = time.monotonic()
            result = invoke("--check", "--stats-json", output)
            assert result.returncode != 0, output
            assert time.monotonic() - started < 3, f"blocked on output {output}"
            assert victim.read_text() == "keep this unchanged\n"

        if not (args.live or args.allow_attach):
            print("loader deployment: CLI and private-output rejection passed; no BPF load/attach requested")
            return
        assert os.uname().sysname == "Linux" and os.geteuid() == 0, "--live requires root on Linux"
        assert STATE.read_text().strip() == "disabled", "refusing an existing scheduler"
        result = invoke("--check")
        assert result.returncode == 0, result.stderr
        previous_pins = {str(path): path.stat().st_ino for path in PINS.glob("*")}
        for mode in ("fixed", "shadow", "online", "ewma"):
            output = work / f"verify-{mode}.jsonl"
            result = invoke("--verify", "--mode", mode, "--stats-json", output)
            assert result.returncode == 0, result.stderr
            data = records(output)
            assert any(row.get("verifier_accepted") for row in data)
            assert any(row.get("event") == "verified" and row["attached"] is False for row in data)
            assert not any(row.get("event") == "registered" for row in data)
            assert output.stat().st_mode & 0o777 == 0o600
            assert STATE.read_text().strip() == "disabled"
        result = invoke("--verify", "--lerwatt", "--cpuperf", "util")
        assert result.returncode == 0, result.stderr
        assert previous_pins == {str(path): path.stat().st_ino for path in PINS.glob("*")}, "verify changed pins"

        # A broken stdout must fail before attach instead of dying via SIGPIPE.
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        process = subprocess.Popen([loader, "--check"], stdout=write_fd, stderr=subprocess.PIPE)
        os.close(write_fd)
        process.communicate(timeout=10)
        assert process.returncode > 0 and STATE.read_text().strip() == "disabled"

        entered, release = work / "maintenance-entered", work / "maintenance-release"
        command = work / "maintenance.sh"
        command.write_text('#!/bin/sh\n: > "$1"\nwhile [ ! -e "$2" ]; do sleep .02; done\n')
        command.chmod(0o700)
        process = subprocess.Popen([loader, "--maintenance", "--", str(command), str(entered), str(release)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            wait_for(entered.exists, "maintenance command did not start")
            for options in (["--verify"], ["--maintenance", "--", "/bin/true"]):
                result = invoke(*options)
                assert result.returncode != 0 and "lock" in result.stderr.lower(), result.stderr
            process.kill()
            process.wait(timeout=5)
            result = invoke("--maintenance", "--", "/bin/true")
            assert result.returncode != 0 and "lock" in result.stderr.lower()
        finally:
            release.touch()
            if process.poll() is None:
                process.wait(timeout=5)
        wait_for(lambda: invoke("--maintenance", "--", "/bin/true").returncode == 0,
                 "maintenance child did not release the lock")
        assert invoke("--maintenance", "--", "/bin/false").returncode == 1

        if not args.allow_attach:
            print("loader deployment: CLI, output safety, no-attach verifier and maintenance lock passed")
            return
        assert vm_detected(), "scheduler attachment is restricted to an explicitly opted-in disposable QEMU/KVM VM"
        assert STATE.read_text().strip() == "disabled", "refusing an existing scheduler"

        owned = []

        def cleanup_owned():
            for process in owned:
                if process.poll() is not None:
                    continue
                process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)

        atexit.register(cleanup_owned)

        def start(name, **kwargs):
            assert STATE.read_text().strip() == "disabled", "refusing an existing scheduler"
            output = work / f"{name}.jsonl"
            process = subprocess.Popen([loader, "--mode", "fixed", "--duration", "30",
                                        "--sample-ms", "10000", "--stats-json", str(output)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, **kwargs)
            owned.append(process)
            try:
                wait_for(lambda: any(row.get("event") == "registered" for row in records(output)),
                         f"{name}: scheduler did not register")
            except BaseException:
                if process.poll() is None:
                    process.terminate()
                _, error = process.communicate(timeout=10)
                raise AssertionError(f"{name}: registration failed: {error!r}")
            return process, output

        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGKILL):
            process, output = start(f"signal-{sig.name}")
            process.send_signal(sig)
            _, error = process.communicate(timeout=10)
            assert process.returncode == (-sig if sig == signal.SIGKILL else 0), error
            wait_for(lambda: STATE.read_text().strip() == "disabled", "signal did not restore default scheduling")
            if sig != signal.SIGKILL:
                assert records(output)[-1]["default_restored"] is True

        # Exercise the existing all-task eligibility switch without changing
        # any model prior, update rule, dispatch policy or slice defaults.
        output = work / "online-all-tasks.jsonl"
        result = invoke("--mode", "online", "--track-all", "--duration", "2",
                        "--sample-ms", "10000", "--stats-json", output)
        assert result.returncode == 0, result.stderr
        data = records(output)
        registration = next(row for row in data if row.get("event") == "registered")
        final = next(row for row in data if row.get("phase") == "final")
        assert registration["mode"] == "online" and registration["track_all"] is True
        assert final["stats"]["training_updates"] > 0 and final["stats"]["inferences"] > 0
        assert data[-1]["default_restored"] is True and STATE.read_text().strip() == "disabled"
        print(f"online eligibility: {final['stats']['training_updates']} updates, "
              f"{final['stats']['inferences']} inferences; defaults unchanged")

        # Ignore the resource-limit signal so fwrite observes EFBIG and the
        # loader must follow its ordinary I/O-error cleanup path.
        def file_limit():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        output = work / "write-failure.jsonl"
        result = subprocess.run([loader, "--mode", "fixed", "--duration", "2",
                                 "--stats-json", str(output)], capture_output=True,
                                timeout=10, preexec_fn=file_limit)
        assert result.returncode == 1, result.stderr
        assert b"telemetry" in result.stderr or b"registration output" in result.stderr, result.stderr
        assert STATE.read_text().strip() == "disabled", "output failure left scheduler attached"
        print("loader deployment: explicit VM attachment, signals, SIGKILL and I/O-error detach passed")


if __name__ == "__main__":
    main()
