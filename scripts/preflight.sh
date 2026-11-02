#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
kernel_only=false
case ${1:-} in
    '') ;;
    --kernel-only) kernel_only=true; shift ;;
    *) echo 'Usage: scripts/preflight.sh [--kernel-only]' >&2; exit 1 ;;
esac
[[ $# == 0 ]] || { echo 'Unexpected preflight arguments.' >&2; exit 1; }
if [[ $(uname -s) != Linux ]]; then
    echo "BLOCKER: sched_ext requires a native Linux kernel." >&2
    exit 1
fi
kernel=$(uname -r)
printf 'Kernel: %s (%s)\n' "$kernel" "$(uname -m)"
if [[ -r /proc/config.gz ]]; then
    config=$(gzip -dc /proc/config.gz)
elif [[ -r /boot/config-$kernel ]]; then
    config=$(cat "/boot/config-$kernel")
else
    echo "BLOCKER: kernel configuration unavailable (/proc/config.gz or /boot/config-$kernel). Install the matching kernel package/config." >&2
    exit 1
fi
for flag in CONFIG_SCHED_CLASS_EXT CONFIG_BPF CONFIG_BPF_SYSCALL CONFIG_BPF_JIT CONFIG_DEBUG_INFO_BTF; do
    if ! printf '%s\n' "$config" | grep -x "$flag=y" >/dev/null; then
        printf 'BLOCKER: %s=y is required in kernel %s. Boot a compatible kernel.\n' "$flag" "$kernel" >&2
        exit 1
    fi
    printf '%s=y\n' "$flag"
done
[[ -r /sys/kernel/btf/vmlinux ]] || { echo 'BLOCKER: running-kernel BTF unavailable.' >&2; exit 1; }
[[ -r /sys/kernel/sched_ext/state ]] || { echo 'BLOCKER: sched_ext sysfs unavailable.' >&2; exit 1; }
state=$(cat /sys/kernel/sched_ext/state)
printf 'sched_ext state: %s\n' "$state"
[[ $state == disabled ]] || { echo 'NOT READY: another sched_ext scheduler is active; stop it through its own manager first.' >&2; exit 1; }
if $kernel_only; then
    printf 'Kernel interfaces available; BPF verifier acceptance is checked separately.\n'
    exit 0
fi
for tool in "${CLANG:-clang-18}" "${BPFTOOL:-bpftool}" "${CC:-cc}" make "${PKG_CONFIG:-pkg-config}"; do
    command -v "$tool" >/dev/null || { printf 'BLOCKER: build tool %s is missing.\n' "$tool" >&2; exit 1; }
done
"${PKG_CONFIG:-pkg-config}" --atleast-version=1.7.0 libbpf || { echo 'BLOCKER: libbpf >=1.7.0 is required; build scripts/build_libbpf.sh as your normal user and select its pkg-config directory.' >&2; exit 1; }
"${CLANG:-clang-18}" -print-targets 2>/dev/null | grep -Eq '^[[:space:]]+bpf' || { echo 'BLOCKER: selected Clang lacks a BPF backend.' >&2; exit 1; }
"${BPFTOOL:-bpftool}" version >/dev/null || { echo 'BLOCKER: bpftool cannot run; install tools matching the running kernel.' >&2; exit 1; }
"${BPFTOOL:-bpftool}" btf dump file /sys/kernel/btf/vmlinux format raw | awk "/STRUCT 'sched_ext_ops'/ { found = 1 } END { exit !found }" || {
    echo 'BLOCKER: sched_ext_ops missing from kernel BTF.' >&2; exit 1;
}
printf 'libbpf: %s\n' "$("${PKG_CONFIG:-pkg-config}" --modversion libbpf)"
printf 'Preflight passed. Compilation alone does not demonstrate verifier acceptance or registration.\n'
