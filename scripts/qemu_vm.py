#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare and control an isolated Ubuntu ARM64 VM; never run BPF on the host."""

import argparse
import hashlib
from pathlib import Path
import platform
import shlex
import shutil
import socket
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VM = ROOT / "build/vm"
IMAGE = "ubuntu-24.04-server-cloudimg-arm64.img"
IMAGE_URL = f"https://cloud-images.ubuntu.com/releases/noble/release-20260926/{IMAGE}"
IMAGE_SHA256 = "1d6bffe64b848468ac97f821d369a4846d983de1800ccf6b5ec8853e85cefc55"
PACKAGES = (
    "linux-generic-hwe-24.04", "linux-tools-generic-hwe-24.04", "clang-18",
    "llvm-18", "libbpf-dev", "libelf-dev", "zlib1g-dev", "libzstd-dev",
    "make", "pkg-config", "build-essential", "python3", "git",
)


def run(args):
    return subprocess.run([str(arg) for arg in args], check=True)


def ssh_args(port):
    return ["ssh", "-i", str(VM / "id_ed25519"), "-p", str(port),
            "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
            "-o", "StrictHostKeyChecking=accept-new", "-o",
            f"UserKnownHostsFile={VM / 'known_hosts'}", "ubuntu@127.0.0.1"]


def prepare():
    VM.mkdir(parents=True, exist_ok=True)
    image = VM / IMAGE
    if not image.exists():
        print(f"Downloading {IMAGE_URL}", flush=True)
        partial = image.with_suffix(".download")
        urllib.request.urlretrieve(IMAGE_URL, partial)
        partial.rename(image)
    with image.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != IMAGE_SHA256:
        raise RuntimeError(f"Image checksum mismatch: {digest}; do not boot it")
    print(f"Verified image SHA256 {digest}")
    key = VM / "id_ed25519"
    if not key.exists():
        run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C",
             "lertick-local-vm", "-f", key])
    seed = VM / "seed"
    seed.mkdir(exist_ok=True)
    public_key = key.with_suffix(".pub").read_text().strip()
    (seed / "meta-data").write_text(
        "instance-id: lertick-phase1-01\nlocal-hostname: lertick-vm\n")
    (seed / "user-data").write_text(
        "#cloud-config\nusers:\n  - name: ubuntu\n    groups: [sudo]\n"
        "    shell: /bin/bash\n    sudo: ALL=(ALL) NOPASSWD:ALL\n"
        "    lock_passwd: true\n    ssh_authorized_keys:\n      - " + public_key +
        "\nssh_pwauth: false\npackage_update: true\npackages:\n" +
        "".join(f"  - {package}\n" for package in PACKAGES) +
        "runcmd:\n  - [sh, -c, 'touch /var/tmp/lertick-provisioned']\n")
    iso = VM / "seed.iso"
    if shutil.which("hdiutil"):
        # hdiutil appends .iso only when it is absent.
        if iso.exists():
            iso.unlink()
        run(["hdiutil", "makehybrid", "-iso", "-joliet",
             "-default-volume-name", "cidata", "-o", iso, seed])
    elif shutil.which("xorriso"):
        run(["xorriso", "-as", "mkisofs", "-V", "cidata", "-J", "-r",
             "-o", iso, seed])
    else:
        raise RuntimeError("Need macOS hdiutil or Linux xorriso to make NoCloud seed")
    disk = VM / "experiment.qcow2"
    if not disk.exists():
        run(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2", "-b",
             image, disk, "20G"])
    print("Base image is read-only; guest writes go to the experiment overlay.")


def firmware_paths():
    for prefix in (Path("/opt/homebrew/share/qemu"), Path("/usr/local/share/qemu"),
                   Path("/usr/share/qemu")):
        code = prefix / "edk2-aarch64-code.fd"
        variables = prefix / "edk2-arm-vars.fd"
        if code.is_file() and variables.is_file():
            return code, variables
    raise RuntimeError("QEMU ARM64 EDK2 firmware not found; specify a supported installation")


def start(port, accelerator):
    if (VM / "monitor.sock").exists():
        raise RuntimeError("VM monitor already exists; inspect status before starting another VM")
    binary = shutil.which("qemu-system-aarch64")
    if not binary:
        raise RuntimeError("qemu-system-aarch64 is unavailable")
    if accelerator == "auto":
        native = platform.machine() in ("arm64", "aarch64")
        if platform.system() == "Darwin" and native:
            accelerator = "hvf"
        elif platform.system() == "Linux" and native and Path("/dev/kvm").exists():
            accelerator = "kvm"
        else:
            accelerator = "tcg"
    code, variables = firmware_paths()
    writable_vars = VM / "efi-vars.fd"
    if not writable_vars.exists():
        shutil.copyfile(variables, writable_vars)
    args = [binary, "-name", "lertick-phase1", "-machine", "virt",
            "-accel", accelerator, "-cpu", "host" if accelerator != "tcg" else "max",
            "-smp", "4", "-m", "4096", "-drive",
            f"if=pflash,format=raw,readonly=on,file={code}", "-drive",
            f"if=pflash,format=raw,file={writable_vars}", "-drive",
            f"if=virtio,format=qcow2,file={VM / 'experiment.qcow2'}", "-drive",
            f"if=virtio,format=raw,readonly=on,file={VM / 'seed.iso'}", "-netdev",
            f"user,id=net0,hostfwd=tcp:127.0.0.1:{port}-:22", "-device",
            "virtio-net-pci,netdev=net0", "-display", "none", "-serial",
            f"file:{VM / 'console.log'}", "-monitor",
            f"unix:{VM / 'monitor.sock'},server=on,wait=off", "-pidfile",
            str(VM / "qemu.pid"), "-daemonize"]
    (VM / "launch-command.txt").write_text(shlex.join(args) + "\n")
    run(args)
    print(f"Started Ubuntu ARM64 using {accelerator}; localhost SSH port {port}")
    print("Wait for cloud-init, then reboot into the installed HWE kernel.")


def stop():
    # Address only this project's monitor, not arbitrary QEMU processes.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.connect(str(VM / "monitor.sock"))
        monitor.sendall(b"system_powerdown\n")
    print("Requested guest shutdown; inspect qemu.pid and serial log before restarting.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "start", "status", "ssh", "stop"))
    parser.add_argument("--port", type=int, default=22222)
    parser.add_argument("--accelerator", choices=("auto", "hvf", "kvm", "tcg"), default="auto")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.action == "prepare":
        prepare()
    elif args.action == "start":
        start(args.port, args.accelerator)
    elif args.action == "stop":
        stop()
    elif args.action == "status":
        run(ssh_args(args.port) + ["uname -a; cloud-init status; "
                                  "cat /sys/kernel/sched_ext/state 2>/dev/null || true"])
    elif args.action == "ssh":
        if not args.command:
            parser.error("ssh requires a remote command")
        command = args.command[1:] if args.command[0] == "--" else args.command
        # A single argument is an explicit shell command; argv otherwise quotes safely.
        run(ssh_args(args.port) + [command[0] if len(command) == 1 else shlex.join(command)])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"VM setup failed: {error}", file=sys.stderr)
        sys.exit(1)
