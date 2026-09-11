#!/usr/bin/env python3
"""File encoding gate.

Three rules that no compiler and no linter checks, each one learned from a real
failure in this project:

  E1  Every tracked text file is valid UTF-8.
      A file that is not UTF-8 still compiles on MSVC as long as its bytes
      happen to fit the ANSI code page, and then breaks for everyone else.

  E2  Line endings are LF.
      The golden regression tests compare bytes; a CRLF checkout silently
      changes every expected hash. `.gitattributes` asks for LF, but attributes
      do not apply to files created by a script or an editor that defaults to
      CRLF, and they do not apply at all outside a git checkout.

  E3  PowerShell scripts start with a UTF-8 BOM.
      Windows PowerShell 5.1 decodes a BOM-less script using the ANSI code page.
      A Chinese comment then becomes mojibake at parse time, and a Chinese
      string literal becomes a wrong string at run time. Neither is an error;
      the script just misbehaves. This project shipped that bug twice.

Usage:
    python scripts/check_encoding.py [--root DIR] [--quiet]

Exit code: 0 pass, 1 violations found.
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SKIP_DIRS = {".git", "build", "build-msvc", "external", "_deps", "__pycache__", ".vs",
             ".venv", "venv", "site-packages", "node_modules", ".tmp"}
# tmp_* is scratch space used by tooling and by investigations; build-* holds
# build trees for individual workers. Neither is part of the deliverable.
SKIP_PREFIXES = ("tmp_", "build-")

# Binary formats are exempt from E1/E2: they are neither UTF-8 nor line oriented.
BINARY_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".ico", ".pdf", ".zip",
                   ".7z", ".exe", ".dll", ".lib", ".pdb", ".bin", ".golden", ".npy",
                   ".npz", ".obj", ".o", ".a", ".so", ".pyc"}

TEXT_SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".txt", ".cmake", ".py", ".ps1", ".bat",
                 ".md", ".json", ".yml", ".yaml", ".gitattributes", ".gitignore"}

BOM = b"\xef\xbb\xbf"


@dataclass
class Violation:
    path: Path
    rule: str
    detail: str

    def render(self, root: Path) -> str:
        try:
            shown = self.path.relative_to(root)
        except ValueError:
            shown = self.path
        return f"{shown}: [{self.rule}] {self.detail}"


def check_file(path: Path) -> list[Violation]:
    out: list[Violation] = []
    suffix = path.suffix.lower()

    if suffix in BINARY_SUFFIXES:
        return out
    if suffix not in TEXT_SUFFIXES and path.name not in (".gitattributes", ".gitignore"):
        return out

    try:
        raw = path.read_bytes()
    except OSError as exc:
        return [Violation(path, "E1", f"cannot read: {exc}")]

    # E1 - valid UTF-8 (the BOM is stripped before decoding; it is a valid
    # prefix, but decoding it as a character would put U+FEFF in the text).
    body = raw[len(BOM):] if raw.startswith(BOM) else raw
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError as exc:
        out.append(Violation(path, "E1", f"not valid UTF-8: {exc}"))
        return out

    # E2 - LF only. Total CR count, not just CRLF: a file with lone CRs (the old
    # Mac convention, which some Windows editors still emit) satisfies "no CRLF"
    # while being just as wrong, and the earlier version of this check missed it.
    crlf = text.count("\r\n")
    lone_cr = text.count("\r") - crlf
    if crlf or lone_cr:
        parts = []
        if crlf:
            parts.append(f"{crlf} CRLF line ending(s)")
        if lone_cr:
            parts.append(f"{lone_cr} lone CR")
        out.append(Violation(path, "E2", " and ".join(parts)))

    # E3 - .ps1 needs a BOM
    if suffix == ".ps1" and not raw.startswith(BOM):
        out.append(Violation(path, "E3",
                             "PowerShell script without a UTF-8 BOM; Windows "
                             "PowerShell 5.1 will decode it as ANSI"))

    return out


def is_skipped(path: Path) -> bool:
    """True when a path belongs to a directory the gate never looks at."""
    return any(part in SKIP_DIRS or part.startswith(SKIP_PREFIXES) for part in path.parts)


def collect(root: Path) -> tuple[int, list[Violation]]:
    scanned = 0
    violations: list[Violation] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if is_skipped(path):
            continue
        suffix = path.suffix.lower()
        if suffix not in TEXT_SUFFIXES and suffix not in BINARY_SUFFIXES:
            if path.name not in (".gitattributes", ".gitignore"):
                continue
        scanned += 1
        violations.extend(check_file(path))
    return scanned, violations


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    ap = argparse.ArgumentParser(description="file encoding gate")
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()

    scanned, violations = collect(root)

    if not args.quiet:
        print(f"encoding gate: scanned {scanned} files, {len(violations)} violations")
    for v in violations[:200]:
        print(v.render(root))
    if len(violations) > 200:
        print(f"... and {len(violations) - 200} more")

    if violations:
        print(f"\nFAILED: {len(violations)} encoding violations", file=sys.stderr)
        return 1
    if not args.quiet:
        print("PASSED: encodings are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
