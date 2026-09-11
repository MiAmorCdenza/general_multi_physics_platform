#!/usr/bin/env python3
"""Prints the translation backlog manifest in a compact, readable form."""
from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    path = REPO / "docs" / "translation-backlog.json"
    # utf-8-sig: tolerate a BOM in case the file was written by a shell redirect.
    manifest = json.loads(path.read_text(encoding="utf-8-sig"))
    print(f"lines {manifest['total_lines']} files {manifest['total_files']}")
    for i, batch in enumerate(manifest["batches"], 1):
        print(f"--- batch {i}: {batch['lines']} lines / {len(batch['files'])} files")
        for entry in batch["files"]:
            print(f"   {entry['lines']:5d}  {entry['path']}")
    if manifest["emoji_only"]:
        print("emoji only:", ", ".join(manifest["emoji_only"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
