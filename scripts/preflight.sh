#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
    echo "BLOCKER: sched_ext requires an actual Linux kernel. Run this inside the QEMU Ubuntu guest." >&2
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
printf 'sched_ext state: %s\n' "$(cat /sys/kernel/sched_ext/state)"
for tool in "${CLANG:-clang-18}" "${BPFTOOL:-bpftool}" cc make pkg-config; do
    command -v "$tool" >/dev/null || { printf 'BLOCKER: build tool %s is missing.\n' "$tool" >&2; exit 1; }
done
pkg-config --exists libbpf || { echo 'BLOCKER: libbpf development package is missing.' >&2; exit 1; }
"${BPFTOOL:-bpftool}" btf dump file /sys/kernel/btf/vmlinux format raw | awk "/STRUCT 'sched_ext_ops'/ { found = 1 } END { exit !found }" || {
    echo 'BLOCKER: sched_ext_ops missing from kernel BTF.' >&2; exit 1;
}
printf 'libbpf: %s\n' "$(pkg-config --modversion libbpf)"
printf 'Preflight passed. Compilation alone does not demonstrate verifier acceptance or registration.\n'
