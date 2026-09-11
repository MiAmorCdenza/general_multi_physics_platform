#!/usr/bin/env python3
"""Plans the comment-translation backlog for the dialect gate (D2).

The gate itself only reports violations. Translating thousands of comment
lines is a batch job, and a batch job needs a partition: this script turns the
violation list into balanced, non-overlapping batches that can be handed to
independent workers, plus the per-file counts needed to verify that each batch
actually landed.

Only files where D2 applies are listed (source, CMake, Python, PowerShell).
Documentation is Chinese by decision and is never part of the backlog.

Usage:
    python scripts/plan_translation.py                # human-readable summary
    python scripts/plan_translation.py --json         # machine-readable manifest
    python scripts/plan_translation.py --batches 6    # choose the partition size

Exit code: always 0 unless the gate script itself cannot be loaded.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# The repository root is the directory that contains scripts/, NOT the current
# working directory: this checkout lives inside a shared projects folder that
# also holds unrelated code, and planning its translation backlog would be both
# slow and wrong.
REPO = HERE.parent


def load_checker():
    spec = importlib.util.spec_from_file_location("qp_check_dialect", HERE / "check_dialect.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_dialect"] = module
    spec.loader.exec_module(module)
    return module


def partition(files: list[tuple[str, int]], batch_count: int) -> list[list[tuple[str, int]]]:
    """Greedy longest-processing-time partition.

    Sorting by size and always filling the currently smallest batch keeps the
    batches within roughly one large file of each other, which matters because
    the batches run as separate workers and the slowest one sets the wall time.
    """
    batches: list[list[tuple[str, int]]] = [[] for _ in range(batch_count)]
    totals = [0] * batch_count
    for path, count in sorted(files, key=lambda kv: (-kv[1], kv[0])):
        target = totals.index(min(totals))
        batches[target].append((path, count))
        totals[target] += count
    for b in batches:
        b.sort()
    return batches


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    ap = argparse.ArgumentParser(description="plan the comment-translation backlog")
    ap.add_argument("--batches", type=int, default=5)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--out", default=None,
                    help="write the manifest here (UTF-8, no BOM). Prefer this "
                         "over shell redirection: PowerShell 5.1 writes UTF-16 "
                         "with a BOM, which no JSON reader accepts.")
    args = ap.parse_args(argv)

    cd = load_checker()
    _, violations = cd.collect(REPO)

    # Only D2 in a source file is translation work; D1 (emoji) is a deletion
    # and is reported separately so it cannot hide inside a translation batch.
    per_file: dict[str, dict[str, int]] = {}
    for v in violations:
        if not cd.is_source(v.path.suffix):
            continue
        rel = str(v.path.relative_to(REPO)).replace("\\", "/")
        slot = per_file.setdefault(rel, {"D1": 0, "D2": 0})
        slot[v.rule] += 1

    files = [(p, c["D2"]) for p, c in per_file.items() if c["D2"] > 0]
    emoji_only = sorted(p for p, c in per_file.items() if c["D2"] == 0 and c["D1"] > 0)
    batches = partition(files, args.batches)

    manifest = {
        "total_lines": sum(c for _, c in files),
        "total_files": len(files),
        "emoji_only": emoji_only,
        "batches": [{"lines": sum(c for _, c in b),
                     "files": [{"path": p, "lines": c} for p, c in b]}
                    for b in batches],
    }

    if args.out:
        Path(args.out).write_text(
            json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8", newline="\n")
        print(f"wrote {args.out}: {manifest['total_lines']} lines "
              f"in {manifest['total_files']} files, {len(batches)} batches")
        return 0

    if args.json:
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
        return 0

    print(f"translation backlog: {manifest['total_lines']} comment lines "
          f"in {manifest['total_files']} files, {len(batches)} batches")
    for i, b in enumerate(manifest["batches"], 1):
        print(f"\n--- batch {i}: {b['lines']} lines, {len(b['files'])} files ---")
        for path, count in b["files"]:
            print(f"{count:5d}  {path}")
    if emoji_only:
        print("\nemoji only (delete, no translation):")
        for path in emoji_only:
            print(f"       {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
