#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate the Phase 0 document set and local Markdown links; no network I/O."""

from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
PLANNING = (
    "RESEARCH.md", "PRIOR_ART.md", "PROJECT_COMPARISON.md", "ARCHITECTURE.md",
    "OPEN_SOURCE_STRATEGY.md", "LICENSE_ANALYSIS.md", "ROADMAP.md",
    "BENCHMARK_PLAN.md", "RECOMMENDATION.md",
)
REQUIRED = (
    "README.md", "README.zh-TW.md", "LICENSE", "CONTRIBUTING.md",
    "CODE_OF_CONDUCT.md", "SECURITY.md", "CHANGELOG.md", "docs/DEPLOYMENT.md",
    "src/README.md", "tests/README.md", "benchmarks/README.md",
    "examples/README.md", ".github/workflows/docs.yml",
)
LINK = re.compile(r"!?\[[^\]\n]+\]\((<[^>]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\)")
FENCE = re.compile(r"^\s*(`{3,}|~{3,})")


def main():
    failures = []
    for name in (*REQUIRED, *(f"docs/planning/{name}" for name in PLANNING)):
        path = ROOT / name
        if not path.is_file() or not path.read_text(encoding="utf-8").strip():
            failures.append(f"{name}: required document missing or empty")

    count = 0
    for path in sorted(ROOT.rglob("*.md")):
        if ".git" in path.relative_to(ROOT).parts:
            continue
        count += 1
        content = path.read_text(encoding="utf-8")
        relative = path.relative_to(ROOT)
        if not content.endswith("\n"):
            failures.append(f"{relative}: missing final newline")
        fence = None
        for number, line in enumerate(content.splitlines(), start=1):
            if line.rstrip() != line:
                failures.append(f"{relative}:{number}: trailing whitespace")
            match = FENCE.match(line)
            if match:
                marker = match.group(1)
                if fence is None:
                    fence = marker
                elif marker[0] == fence[0] and len(marker) >= len(fence):
                    fence = None
                continue
            if fence:
                continue
            for match in LINK.finditer(line):
                target = match.group(1).strip("<>")
                parts = urlsplit(target)
                if parts.scheme or parts.netloc or not parts.path:
                    continue
                destination = (path.parent / unquote(parts.path)).resolve()
                if not destination.is_relative_to(ROOT):
                    failures.append(f"{relative}:{number}: link escapes repository: {target}")
                elif not destination.exists():
                    failures.append(f"{relative}:{number}: missing local target: {target}")
        if fence:
            failures.append(f"{relative}: unclosed code fence")

    if failures:
        print("Documentation validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"Validated {count} Markdown files, 9 planning documents, and local links.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
