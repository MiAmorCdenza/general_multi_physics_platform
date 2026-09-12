#!/usr/bin/env python3
"""The plan tree's claim about the repository's shape, checked against the repository.

Section 9.9 of `docs/plan-tree.md` is a framework-completeness audit, and its whole content is a claim about
which content **categories** exist and what each one cost the framework:

    六类内容、六条不同的进入路径、五类零接口改动。

That sentence is the most useful thing in the document and the most perishable. The moment somebody adds a
seventh plugin directory it becomes false, and it becomes false **silently** -- the prose still reads correctly,
nobody editing a plugin reads it, and the next person to consult the audit is consulting a stale fact. A
document that describes a property of the tree has to be checked against the tree, or it is a comment.

This script is therefore a gate, in the same family as `check_core_standalone.py`: it reads the table out of the
plan tree, reads the directories out of `plugins/`, and fails when the two disagree. It is registered as
`meta.content_shape` so `ctest` runs it with the rest of the gate self-tests.

What it deliberately does **not** do: check the "what each one cost the framework" column. Whether a category
needed a new interface is a judgement about a commit, and a script that guessed at it would be guessing. The
directory set is a fact, and facts are what a script can hold.

Usage: python tests/meta/check_content_shape.py
"""
from __future__ import annotations

import pathlib
import re
import sys

# The messages are Chinese and this script runs under whatever Python `ctest` found. A console whose encoding
# cannot represent them raises `UnicodeEncodeError` **while printing a failure**, which turns a clear message into
# a traceback -- and the traceback is what the reader sees. Every other gate in this directory reconfigures for
# the same reason; that it is a two-line ritual is the price of the messages being readable.
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
PLUGINS = REPO / "plugins"
PLAN_TREE = REPO / "docs" / "plan-tree.md"

# The section the table lives in. Named rather than searched for loosely: the document has several tables and a
# script that grabbed the wrong one would report a mismatch that is not there.
SECTION = "### 9.9"
# A row names a category by its **own** directory, which may be nested: `formats/` holds one directory per format
# and both `qpjson` and `csv` are categories in their own right. The script takes the deepest path in the row and,
# when that names a grouping directory rather than a category, the child directory it is under. That is a rule
# about this tree rather than a guess: `plugins/<a>/<b>/` is the category when it exists.
ROW = re.compile(r"^\|\s*`(plugins/[a-z_/]+?)/?`", re.M)


def documented_categories() -> set[str]:
    """The plugin directories named in section 9.9's table, as paths relative to `plugins/`."""
    text = PLAN_TREE.read_text(encoding="utf-8")
    start = text.find(SECTION)
    if start < 0:
        raise SystemExit(f"{SECTION} not found in {PLAN_TREE.name}: the audit moved or was renamed")
    # Up to the next heading of the same or a higher level.
    end = len(text)
    for m in re.finditer(r"^#{2,3} ", text[start + len(SECTION):], re.M):
        end = start + len(SECTION) + m.start()
        break
    section = text[start:end]

    categories: set[str] = set()
    for m in ROW.finditer(section):
        rel = m.group(1)
        parts = rel.split("/")[1:]  # drop the leading `plugins`
        # A row for `plugins/instruments` names the directory; a row for `plugins/formats/qpjson` names a nested
        # one. Both are categories; the grouping directory `formats` is not.
        categories.add("/".join(parts))
    if not categories:
        raise SystemExit(f"no `plugins/...` rows found in {SECTION}: the table's shape changed")
    return categories


def _is_grouping(entry: pathlib.Path) -> bool:
    """Whether `entry` only groups other plugin directories.

    **A plugin directory has an `include/` and a `src/`.** That is the shape every category in this tree has, and
    it is a fact about the directory rather than a list of names -- so a second grouping directory needs no edit
    here. The first version of this function asked "does it contain nothing but directories", which is true of
    `plugins/formats` **and** of a plugin directory in the moment before somebody adds its sources; the sharper
    test is the one that says what a plugin is rather than what it lacks.
    """
    return not (entry / "include").is_dir() and not (entry / "src").is_dir()


def actual_categories() -> set[str]:
    """Every plugin directory the tree actually has, as paths relative to `plugins/`."""
    found: set[str] = set()
    for entry in sorted(PLUGINS.iterdir()):
        if not entry.is_dir():
            continue
        if _is_grouping(entry):
            for child in sorted(entry.iterdir()):
                if child.is_dir():
                    found.add(f"{entry.name}/{child.name}")
        else:
            found.add(entry.name)
    return found


def main() -> int:
    documented = documented_categories()
    actual = actual_categories()

    missing = sorted(actual - documented)
    extra = sorted(documented - actual)

    if missing:
        print(f"[FAIL] plugins/ 下有目录没有写进 §9.9 的表：{missing}", file=sys.stderr)
        print("       加一类内容就要加一行——那正是这一节存在的方式，"
              "也是「加这类内容是否需要新接口」这个判据唯一能被回答的地方。", file=sys.stderr)
    if extra:
        print(f"[FAIL] §9.9 的表里有 plugins/ 下不存在的目录：{extra}", file=sys.stderr)

    if missing or extra:
        return 1

    print(f"[OK  ] §9.9 的内容类别表与 plugins/ 一致（{len(actual)} 类："
          + "、".join(sorted(actual)) + "）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
