#!/usr/bin/env python3
"""Repository dialect gate.

Enforces the project-wide dialect rules (see standards/code-dialect.md):

  D1  No emoji anywhere outside UI resources.
  D2  C++ / CMake / Python / PowerShell comments must be ASCII English.
      (Non-ASCII inside string literals is allowed only where explicitly
      whitelisted, e.g. localized UI strings.)
  D3  Debug/log output must be ASCII so it survives any console codepage.

Why D3 matters in this project: a death test compares against a regular
expression in tests/CMakeLists.txt. A UTF-8 message read back under a GBK
console fails to match, producing a false gate failure. That happened.

Exit code: 0 pass, 1 violations found.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# Ranges that are banned **everywhere** (D1). These are pictographic: they carry
# no information in a technical document, and they are the characters most
# likely to be mangled by a console codepage.
#
# Note the boundaries. Arrows and box-drawing characters are deliberately NOT
# here even though Unicode classifies some of them as symbols: in a comment they
# express structure (data flow, a boxed warning) and are handled by D3 instead.
# Geometric shapes are excluded too, because U+25B2 and U+25BC are how a design
# note is allowed to say "monotonically increasing".
EMOJI_RANGES = [
    (0x2300, 0x23FF),   # misc technical: watch, hourglass, keyboard
    (0x2460, 0x24FF),   # enclosed alphanumerics: circled digits used as decoration
    (0x2700, 0x27BF),   # dingbats: check marks, cross marks, sparkles
    (0x2B00, 0x2BFF),   # misc symbols and arrows (star, large triangles)
    (0xFE0F, 0xFE0F),   # variation selector-16 (emoji presentation)
    (0x1F000, 0x1FAFF), # emoticons, pictographs, transport, supplemental
]

# Banned in **source** comments only (D3). In prose a diagram may legitimately
# use these; in a comment they are decoration, and they are exactly the
# characters that turn into `?` or mojibake in a non-UTF-8 console - which is
# the failure mode D3 exists to prevent. A comment that needs to show structure
# can show it in ASCII: `a -> b`, `+-- ... --+`.
DIAGRAM_RANGES = [
    (0x2190, 0x21FF),   # arrows
    (0x2500, 0x257F),   # box drawing
    (0x2580, 0x259F),   # block elements
]

EMOJI_RE = re.compile(
    "[" + "".join(f"\\U{lo:08x}-\\U{hi:08x}" for lo, hi in EMOJI_RANGES) + "]"
)
DIAGRAM_RE = re.compile(
    "[" + "".join(f"\\U{lo:08x}-\\U{hi:08x}" for lo, hi in DIAGRAM_RANGES) + "]"
)
CJK_RE = re.compile(r"[\u3000-\u303f\u3400-\u4dbf\u4e00-\u9fff\uff00-\uffef]")

SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".txt", ".cmake", ".py", ".ps1", ".bat"}
# Directories whose contents are never part of the deliverable:
#   .venv / venv / site-packages / node_modules - vendor code, listed
#     defensively because this checkout may sit inside a shared folder that
#     contains them as siblings;
#   tmp_* / .tmp - scratch space that tooling and investigations use. The name
#     is the convention, so ignoring the whole prefix is the point.
SKIP_DIRS = {".git", "build", "build-msvc", "external", "_deps", "__pycache__", ".vs",
             ".venv", "venv", "site-packages", "node_modules", ".tmp"}
SKIP_PREFIXES = ("tmp_", "build-")

# Markdown documentation is intentionally excluded from D2: the specification
# prose is Chinese by decision (see standards/code-dialect.md).
DOC_SUFFIXES = {".md", ".json", ".yml", ".yaml"}

# Explicit per-line suppression. This exists for one narrow purpose: a file that
# *tests* the gate has to contain the exact characters the gate rejects. Trying
# to hide the fixtures elsewhere (a skipped directory, a string built at run
# time) makes the test data harder to read and easier to get subtly wrong, and
# the alternative -- weakening the rule -- costs the rule its value.
#
# The marker must be on the offending line itself, not on a line above it: a
# line-scoped marker keeps the exemption impossible to widen by accident.
SUPPRESSION_MARKER = "qp-dialect-allow:"


@dataclass
class Violation:
    path: Path
    line: int
    rule: str
    detail: str
    snippet: str

    def render(self, root: Path) -> str:
        try:
            shown = self.path.relative_to(root)
        except ValueError:
            shown = self.path
        return f"{shown}:{self.line}: [{self.rule}] {self.detail}\n      {self.snippet}"


def comment_text(line: str) -> str:
    """Extract the comment portion of a source line, if any.

    Handles // and /* */ and # for Python/PowerShell/CMake. Deliberately
    simple: it only needs to be good enough to classify a line, and the
    gate is verified by tests/meta.
    """
    stripped = line.strip()
    for marker in ("//", "#", "*", "/*", "--"):
        if stripped.startswith(marker):
            return stripped
    idx = line.find("//")
    if idx >= 0:
        return line[idx:]
    return ""


def scan_text(text: str, path: Path, check_comments: bool) -> list[Violation]:
    """Scans one file's text. Split out from scan_file so that the meta test can
    feed fixtures as strings instead of writing them to disk.

    `check_comments` selects the source rules (D2, D3) in addition to D1. It is
    True for code, CMake and scripts, and False for documentation.
    """
    out: list[Violation] = []
    for n, line in enumerate(text.splitlines(), 1):
        if SUPPRESSION_MARKER in line:
            continue
        snippet = line.strip()[:100]
        if EMOJI_RE.search(line):
            out.append(Violation(path, n, "D1", "emoji or pictograph outside UI resources",
                                 snippet))
        if not check_comments:
            continue
        comment = comment_text(line)
        if not comment:
            continue
        if CJK_RE.search(comment):
            out.append(
                Violation(path, n, "D2",
                          "non-ASCII text in a source comment (comments must be English)",
                          snippet)
            )
        if DIAGRAM_RE.search(comment):
            out.append(
                Violation(path, n, "D3",
                          "arrow or box drawing in a source comment "
                          "(use ASCII: ->, |-, +--)",
                          snippet)
            )
    return out


def scan_file(path: Path, root: Path, check_comments: bool) -> list[Violation]:
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return []
    return scan_text(text, path, check_comments)


def is_source(suffix: str) -> bool:
    """True when D2 (ASCII English comments) applies to this suffix.

    Markdown and data files are documentation: their prose is Chinese by
    decision, so only D1 applies to them.
    """
    return suffix.lower() in SOURCE_SUFFIXES


def is_skipped(path: Path) -> bool:
    """True when a path belongs to a directory the gate never looks at."""
    return any(part in SKIP_DIRS or part.startswith(SKIP_PREFIXES) for part in path.parts)


def collect(root: Path) -> tuple[int, list[Violation]]:
    """Scans a tree. Returns (files scanned, violations)."""
    violations: list[Violation] = []
    scanned = 0
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if is_skipped(path):
            continue
        suffix = path.suffix.lower()
        if suffix not in SOURCE_SUFFIXES and suffix not in DOC_SUFFIXES:
            continue
        scanned += 1
        violations.extend(scan_file(path, root, is_source(suffix)))
    return scanned, violations


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    repo = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description="repository dialect gate")
    ap.add_argument("--root", default=str(repo))
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--by-file", action="store_true",
                    help="summarise instead of listing every line: one row per "
                         "file with a violation count, most violations first. "
                         "Used to plan and to track the translation backlog.")
    ap.add_argument("--rules", default=None,
                    help="comma-separated rule filter, e.g. D1 to see only emoji. "
                         "Applies to both the line listing and --by-file.")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()

    scanned, violations = collect(root)
    if args.rules:
        wanted = {r.strip().upper() for r in args.rules.split(",") if r.strip()}
        violations = [v for v in violations if v.rule in wanted]

    if args.by_file:
        per_file: dict[Path, int] = {}
        per_rule: dict[str, int] = {}
        for v in violations:
            per_file[v.path] = per_file.get(v.path, 0) + 1
            per_rule[v.rule] = per_rule.get(v.rule, 0) + 1
        print(f"dialect gate: scanned {scanned} files, {len(violations)} violations "
              f"in {len(per_file)} files")
        for rule, count in sorted(per_rule.items()):
            print(f"  {rule}: {count}")
        for path, count in sorted(per_file.items(), key=lambda kv: (-kv[1], str(kv[0]))):
            try:
                shown = path.relative_to(root)
            except ValueError:
                shown = path
            print(f"{count:5d}  {shown}")
        return 1 if violations else 0

    if not args.quiet:
        print(f"dialect gate: scanned {scanned} files, {len(violations)} violations")

    for v in violations[:400]:
        print(v.render(root))
    if len(violations) > 400:
        print(f"... and {len(violations) - 400} more")

    if violations:
        print(f"\nFAILED: {len(violations)} dialect violations", file=sys.stderr)
        return 1
    if not args.quiet:
        print("PASSED: dialect rules satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
