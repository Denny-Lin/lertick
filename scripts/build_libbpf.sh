#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the matching libbpf locally; never overwrite distribution libraries.
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then
  echo 'Run this dependency build on native Linux.' >&2
  exit 1
fi
if [[ $EUID == 0 ]]; then
  echo 'Build dependencies as your normal user; this script never downloads or builds as root.' >&2
  exit 1
fi
task_root=$(cd "$(dirname "$0")/.." && pwd)
task_deps=${LERTICK_DEPS_DIR:-"$task_root/build/deps"}
task_prefix=${LERTICK_LIBBPF_PREFIX:-"$task_deps/install"}
task_commit=f5dcbae736e5d7f83a35718e01be1a8e3010fa39
mkdir -p "$task_deps"
if [[ ! -d "$task_deps/libbpf/.git" ]]; then
  printf 'Downloading pinned libbpf v1.7.0 into %s (no system libraries are replaced).\n' "$task_deps/libbpf" >&2
  git clone --depth 1 --branch v1.7.0 https://github.com/libbpf/libbpf.git "$task_deps/libbpf"
fi
if [[ $(git -C "$task_deps/libbpf" rev-parse HEAD) != "$task_commit" ]]; then
  echo 'Refusing an unpinned libbpf source checkout.' >&2
  exit 1
fi
make -C "$task_deps/libbpf/src" BUILD_STATIC_ONLY=1 \
  PREFIX="$task_prefix" LIBDIR="$task_prefix/lib" install
printf 'Local libbpf ready. Build with PKG_CONFIG_PATH=%s/lib/pkgconfig make\n' "$task_prefix"
