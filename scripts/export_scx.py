#!/usr/bin/env python3
"""Copy the shared scheduler sources into the sched-ext/scx port.

The policy (src/bpf/policy.h) and the in-kernel model (src/shared/model.h)
have one source. This script writes their upstream copies under
upstream/scx_lertick/src/bpf, dropping product-only regions of model.h
and policy.h (between "#ifndef LERTICK_UPSTREAM" and
"#endif /* !LERTICK_UPSTREAM */") and giving model.h the GPL-2.0 licence every scx scheduler uses.

  scripts/export_scx.py            update the upstream copies
  scripts/export_scx.py --check    fail if they are out of date (CI)
  scripts/export_scx.py --to DIR   also copy upstream/scx_lertick to DIR,
                                   e.g. an scx checkout's scheds/experimental/scx_lertick
"""
import argparse
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PORT = ROOT / "upstream" / "scx_lertick"
BEGIN = "#ifndef LERTICK_UPSTREAM\n"
END = "#endif /* !LERTICK_UPSTREAM */\n"


def strip_product(text):
    out, skip = [], False
    for line in text.splitlines(keepends=True):
        if line == BEGIN:
            if skip:
                raise SystemExit("nested LERTICK_UPSTREAM region")
            skip = True
        elif line == END:
            if not skip:
                raise SystemExit("unmatched LERTICK_UPSTREAM end")
            skip = False
        elif not skip:
            out.append(line)
    if skip:
        raise SystemExit("unterminated LERTICK_UPSTREAM region")
    return "".join(out)


def model_h():
    text = strip_product((ROOT / "src/shared/model.h").read_text())
    head = "/* SPDX-License-Identifier: GPL-2.0-only */\n"
    note_start = text.index("/*\n * Regions between")
    note_end = text.index("*/\n", note_start) + 3
    text = text[:note_start] + text[note_end:]
    assert text.startswith(head)
    return "/* SPDX-License-Identifier: GPL-2.0 */\n" + text[len(head):]


def policy_h():
    return strip_product((ROOT / "src/bpf/policy.h").read_text())


FILES = {"src/bpf/model.h": model_h, "src/bpf/policy.h": policy_h}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--to", type=pathlib.Path)
    args = ap.parse_args()
    stale = []
    for rel, make in FILES.items():
        path, want = PORT / rel, make()
        if path.exists() and path.read_text() == want:
            continue
        if args.check:
            stale.append(rel)
        else:
            path.write_text(want)
            print(f"updated upstream/scx_lertick/{rel}")
    if stale:
        print("out of date, run scripts/export_scx.py: " + ", ".join(stale), file=sys.stderr)
        return 1
    if args.to:
        shutil.copytree(PORT, args.to, dirs_exist_ok=True)
        print(f"copied upstream/scx_lertick to {args.to}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
