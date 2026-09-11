#!/usr/bin/env python3
"""Counts non-ASCII decoration by Unicode range, to plan an ASCII cleanup.

The dialect gate reports violations per rule. This reports them per *range*, so
that a bulk replacement can be planned: each range needs a different ASCII
rendering (an arrow becomes `->`, a circled digit becomes `(1)`, a box-drawing
banner becomes `--`).

Usage:
    python scripts/show_nonascii_stats.py
"""
from __future__ import annotations

import collections
import sys
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

# Ranges that are worth distinguishing when planning a replacement pass.
RANGES = [
    ("arrows", 0x2190, 0x21FF),
    ("circled digits/letters", 0x2460, 0x24FF),
    ("box drawing", 0x2500, 0x257F),
    ("block elements", 0x2580, 0x259F),
    ("geometric shapes", 0x25A0, 0x25FF),
    ("dingbats / misc symbols", 0x2700, 0x27BF),
    ("misc technical", 0x2300, 0x23FF),
    ("fullwidth forms", 0xFF00, 0xFFEF),
    ("CJK punctuation", 0x3000, 0x303F),
]

SKIP_DIRS = {".git", "build", "build-msvc", "external", "_deps", "__pycache__", ".vs",
             ".venv", "venv", "site-packages", "node_modules"}
SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".txt", ".cmake", ".py", ".ps1", ".bat", ".md"}


def range_of(ch: str) -> str:
    cp = ord(ch)
    for name, lo, hi in RANGES:
        if lo <= cp <= hi:
            return name
    return "other non-ASCII"


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    counts: collections.Counter[str] = collections.Counter()
    sample: dict[str, list[str]] = collections.defaultdict(list)

    for path in sorted(REPO.rglob("*")):
        if not path.is_file() or any(p in SKIP_DIRS for p in path.parts):
            continue
        if path.suffix.lower() not in SUFFIXES:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for ch in text:
            if ord(ch) < 0x80:
                continue
            label = range_of(ch)
            if label == "other non-ASCII":
                continue
            counts[label] += 1
            if len(sample[label]) < 8 and ch not in sample[label]:
                sample[label].append(ch)

    print("non-ASCII decoration by range:")
    for name, _, _ in RANGES:
        n = counts.get(name, 0)
        if not n:
            continue
        shown = " ".join(f"U+{ord(c):04X}({unicodedata.name(c, '?')})" for c in sample[name])
        print(f"{n:6d}  {name:24s} {shown}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
