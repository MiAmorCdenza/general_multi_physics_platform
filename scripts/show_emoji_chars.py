#!/usr/bin/env python3
"""Lists the distinct D1 characters found in the repository, with counts.

The dialect gate answers "where is the violation"; this answers "which
characters are involved". That distinction matters because D1 covers a range of
decorative Unicode, not only emoji: a comment banner drawn with box-drawing
characters violates D1 just as a smiley does, and the replacement is different
in each case.

Usage:
    python scripts/show_emoji_chars.py            # all D1 characters
    python scripts/show_emoji_chars.py --source   # only files where D2 applies
"""
from __future__ import annotations

import argparse
import collections
import importlib.util
import sys
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def load_checker():
    spec = importlib.util.spec_from_file_location("qp_check_dialect", HERE / "check_dialect.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_dialect"] = module
    spec.loader.exec_module(module)
    return module


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    ap = argparse.ArgumentParser(description="list distinct D1 characters")
    ap.add_argument("--source", action="store_true",
                    help="only files where D2 applies (code, CMake, scripts)")
    args = ap.parse_args(argv)

    cd = load_checker()
    _, violations = cd.collect(REPO)

    chars: collections.Counter[str] = collections.Counter()
    for v in violations:
        if v.rule != "D1":
            continue
        if args.source and not cd.is_source(v.path.suffix):
            continue
        for ch in cd.EMOJI_RE.findall(v.snippet):
            chars[ch] += 1

    print(f"distinct characters: {len(chars)}")
    for ch, count in chars.most_common():
        try:
            name = unicodedata.name(ch)
        except ValueError:
            name = "<unnamed>"
        print(f"U+{ord(ch):04X}  {count:5d}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
