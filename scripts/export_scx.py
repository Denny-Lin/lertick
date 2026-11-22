#!/usr/bin/env python3
"""Copy the shared in-kernel model into the sched-ext/scx port.

The in-kernel model (src/shared/model.h) has one source. This script writes
its upstream copy to upstream/scx_lertick/src/bpf/model.h, dropping
product-only regions (between "#ifndef LERTICK_UPSTREAM" and
"#endif /* !LERTICK_UPSTREAM */") and giving it the GPL-2.0 licence every scx
scheduler uses. The scheduling policy is developed and tested in the port
(upstream/scx_lertick/src/bpf/policy.h); the product keeps its own copy in
src/bpf/policy.h.

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


FILES = {"src/bpf/model.h": model_h}


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
