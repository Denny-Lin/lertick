#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Native build against the running kernel; no kernel or service changes.
set -eu
fail() { printf 'lertick: %s\n' "$*" >&2; exit 1; }
[ "$(uname -s)" = Linux ] || fail 'native deployment requires Linux'
[ "$(id -u)" -ne 0 ] || fail 'build as your normal user; dependency downloads/builds never run as root'
task_root=$(CDPATH= cd -P "$(dirname "$0")/.." && pwd)
task_cc=${CC:-cc}
task_pkg=${PKG_CONFIG:-pkg-config}
task_bpftool=${BPFTOOL:-bpftool}
for tool in "$task_cc" "$task_pkg" "$task_bpftool" make git bash sha256sum flock; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing build tool: $tool; install your distribution's development packages first"
done
"$task_bpftool" version >/dev/null 2>&1 || fail 'bpftool cannot run; install the Linux tools matching the running kernel'
task_clang=${CLANG:-}
if [ -z "$task_clang" ]; then
    for candidate in clang-18 clang-20 clang-19 clang; do
        if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -print-targets 2>/dev/null | grep -Eq '^[[:space:]]+bpf'; then
            task_clang=$candidate
            break
        fi
    done
fi
[ -n "$task_clang" ] && command -v "$task_clang" >/dev/null 2>&1 || fail 'install Clang/LLVM with a BPF backend (tested: clang-18)'
"$task_clang" -print-targets 2>/dev/null | grep -Eq '^[[:space:]]+bpf' || fail "selected compiler lacks a BPF backend: $task_clang"
CLANG=$task_clang BPFTOOL=$task_bpftool "$task_root/scripts/preflight.sh" --kernel-only
"$task_pkg" --exists libelf zlib || fail 'libelf/zlib development files are missing (Ubuntu: libelf-dev zlib1g-dev)'
mkdir -p "$task_root/build"
# Retain this lock file: unlinking it could create a second independent lock.
exec 9>"$task_root/build/.native-build.lock"
flock -n 9 || fail 'another native build owns this checkout; retry after it finishes'
printf 'lertick: preparing isolated pinned libbpf; the first build may download its source.\n' >&2
"$task_root/scripts/build_libbpf.sh"
task_deps=${LERTICK_DEPS_DIR:-"$task_root/build/deps"}
task_prefix=${LERTICK_LIBBPF_PREFIX:-"$task_deps/install"}
PKG_CONFIG_PATH="$task_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH
"$task_pkg" --atleast-version=1.7.0 libbpf || fail 'the isolated libbpf installation is unavailable or too old'
CLANG=$task_clang BPFTOOL=$task_bpftool PKG_CONFIG=$task_pkg "$task_root/scripts/preflight.sh"
task_btf=$(sha256sum /sys/kernel/btf/vmlinux)
task_btf=${task_btf%% *}
task_clang_version=$("$task_clang" --version | sed -n '1p')
task_bpftool_version=$("$task_bpftool" version | sed -n '1p')
task_libbpf_version=$("$task_pkg" --modversion libbpf)
task_cc_version=$("$task_cc" --version | sed -n '1p')
task_recipe_hash=$(sha256sum "$task_root/Makefile" "$task_root/scripts/native_build.sh")
task_fingerprint="$(uname -r) $(uname -m) $task_btf
$task_cc $task_cc_version
$task_clang $task_clang_version
$task_bpftool $task_bpftool_version
libbpf $task_libbpf_version
$task_recipe_hash
${CFLAGS:-} ${BPF_CFLAGS:-} ${CPPFLAGS:-}"
task_stamp=$task_root/build/.native-kernel
if [ ! -r "$task_stamp" ] || [ "$(cat "$task_stamp")" != "$task_fingerprint" ]; then
    printf 'lertick: kernel/BTF build fingerprint changed; rebuilding embedded BPF.\n' >&2
    rm -f "$task_root/build/vmlinux.h" "$task_root/build/scx_lertick.bpf.o" \
        "$task_root/build/scx_lertick.bpf.skel.h" "$task_root/build/scx_lertick"
fi
make -C "$task_root" BUILD=build CLANG="$task_clang" BPFTOOL="$task_bpftool" \
    CC="$task_cc" PKG_CONFIG="$task_pkg" build/scx_lertick
printf '%s\n' "$task_fingerprint" > "$task_stamp.tmp"
mv -f "$task_stamp.tmp" "$task_stamp"
