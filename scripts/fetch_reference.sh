#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ref="$root/build/reference/scx-c-examples"
commit=370b1cea95c6bb668d5b9261f069d433142b9986
url=https://github.com/sched-ext/scx-c-examples.git
mkdir -p "$(dirname "$ref")"
if [[ ! -d "$ref/.git" ]]; then
    git clone --no-checkout "$url" "$ref"
fi
[[ $(git -C "$ref" remote get-url origin) == "$url" ]] || {
    echo "Refusing unexpected reference remote in $ref" >&2; exit 1;
}
git -C "$ref" diff --quiet
git -C "$ref" diff --cached --quiet
git -C "$ref" cat-file -e "$commit^{commit}" 2>/dev/null || git -C "$ref" fetch origin "$commit"
git -C "$ref" checkout --detach "$commit"
printf 'Official reference: %s at %s\n' "$url" "$(git -C "$ref" rev-parse HEAD)"
