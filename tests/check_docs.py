#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate maintained documents and local Markdown/HTML links; no network I/O."""

from pathlib import Path
from html.parser import HTMLParser
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
    "README.md", "LICENSE", ".github/CONTRIBUTING.md",
    ".github/CODE_OF_CONDUCT.md", ".github/SECURITY.md", "docs/CHANGELOG.md", "docs/DEPLOYMENT.md",
    "src/README.md", "tests/README.md", "benchmarks/README.md",
    "examples/README.md", ".github/workflows/docs.yml", "docs/PHASE1_RESULTS.md",
    "docs/DECISIONS.md", "docs/NAMING.md", "docs/REBRAND_VALIDATION.md",
    "docs/planning/README.md", "docs/legal/THIRD_PARTY.md", "results/phase1/README.md",
    "docs/README.md", "docs/INSTALL.md", "docs/OVERVIEW.md", "docs/assets/lertick-overview.svg",
)
LINK = re.compile(r"!?\[[^\]\n]+\]\((<[^>]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\)")
FENCE = re.compile(r"^\s*(`{3,}|~{3,})")


class HtmlLinks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.targets = []

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if name in ("href", "src") and value:
                self.targets.append((self.getpos()[0], value))


def check_target(path, number, target, failures):
    parts = urlsplit(target)
    if parts.scheme or parts.netloc or not parts.path:
        return
    relative = path.relative_to(ROOT)
    destination = (path.parent / unquote(parts.path)).resolve()
    if not destination.is_relative_to(ROOT):
        failures.append(f"{relative}:{number}: link escapes repository: {target}")
    elif not destination.exists():
        failures.append(f"{relative}:{number}: missing local target: {target}")


def main():
    failures = []
    for name in (*REQUIRED, *(f"docs/planning/{name}" for name in PLANNING)):
        path = ROOT / name
        if not path.is_file() or not path.read_text(encoding="utf-8").strip():
            failures.append(f"{name}: required document missing or empty")

    count = 0
    for path in sorted(ROOT.rglob("*.md")):
        # Downloaded reference trees and generated VM/build artifacts are not
        # maintained project documents and may contain their own relative links.
        if path.relative_to(ROOT).parts[0] in (".git", "build", "vendor"):
            continue
        count += 1
        content = path.read_text(encoding="utf-8")
        relative = path.relative_to(ROOT)
        if not content.endswith("\n"):
            failures.append(f"{relative}: missing final newline")
        fence = None
        html_lines = []
        for number, line in enumerate(content.splitlines(), start=1):
            if line.rstrip() != line:
                failures.append(f"{relative}:{number}: trailing whitespace")
            match = FENCE.match(line)
            if match:
                html_lines.append("")
                marker = match.group(1)
                if fence is None:
                    fence = marker
                elif marker[0] == fence[0] and len(marker) >= len(fence):
                    fence = None
                continue
            if fence:
                html_lines.append("")
                continue
            html_lines.append(line)
            for match in LINK.finditer(line):
                target = match.group(1).strip("<>")
                check_target(path, number, target, failures)
        html = HtmlLinks()
        html.feed("\n".join(html_lines))
        for number, target in html.targets:
            check_target(path, number, target, failures)
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
