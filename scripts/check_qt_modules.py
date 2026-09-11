#!/usr/bin/env python3
"""Qt licence gate: only LGPL-compatible Qt modules may be linked.

Why check **linkage** and not source:

A source scan can only look for `#include <QtCharts/...>`. It would miss the two
ways this actually goes wrong -- a CMake `find_package(Qt6 COMPONENTS Charts)`
with no include in the file being scanned, and a transitive pull through a Qt
module that is itself LGPL but depends on a GPL one. What matters legally is the
set of libraries that end up in the distributed binary, and the build system is
where that set is decided.

So this gate reads the generated build files and extracts every `Qt6<Name>.lib`
the project links. That is the same list `windeployqt` would copy, which makes
the check equivalent to "what would ship".

The rule:

  Qt Charts and Qt Quick are the two modules a well-meaning change is most likely
  to add, and both are disqualifying. Qt Charts is GPL-3.0-or-commercial with no
  LGPL option, which would place the whole distributed binary under GPL-3.0 and
  defeat the point of shipping under Apache-2.0. Qt Quick is a technical decision
  (ADR-0006) rather than a licensing one, but it is checked here too because the
  check is nearly free once the module list is in hand.

See THIRD_PARTY_NOTICES.md section 1 and docs/adr/ADR-0006.

Usage:
    python scripts/check_qt_modules.py [--build-dir DIR] [--quiet]

Exit code: 0 pass (or no Qt in use), 1 a disqualifying module is linked.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Modules that may NOT be linked, with the reason. Checked against the linked set.
FORBIDDEN_MODULES = {
    "Charts": "GPL-3.0 or commercial only (no LGPL) -- would put the whole "
              "distributed binary under GPL-3.0",
    "ChartsQml": "pulls in Charts, same GPL-3.0 problem",
    "DataVisualization": "GPL-3.0 or commercial only, same problem as Charts",
    "Quick": "ADR-0006 decision: Widgets, not QML",
    "QuickControls2": "ADR-0006 decision: Widgets, not QML",
    "Qml": "ADR-0006 decision: Widgets, not QML",
    "WebEngineWidgets": "ships a bundled Chromium with its own licence obligations",
    "WebEngineCore": "ships a bundled Chromium with its own licence obligations",
    "Multimedia": "pulls in codec libraries whose patent licensing is not covered here",
    "Pdf": "depends on a bundled PDFium build with separate obligations",
}

# Directories that hold generated build trees.
DEFAULT_BUILD_DIRS = ["build", "build-msvc", "build-views"]

LINKED_QT_RE = re.compile(r"Qt6([A-Za-z0-9_]+?)(d?)\.lib", re.IGNORECASE)


def normalise_module(name: str) -> str:
    """Strip Qt's debug-library suffix.

    Qt names a debug build `Qt6Widgetsd.lib` and the release build
    `Qt6Widgets.lib`, so the raw file name carries a trailing `d` that is not
    part of the module name. Comparing raw names would let `Qt6Chartsd.lib` pass a
    check for `Charts` -- and a **debug** build is exactly what a developer links
    while prototyping the chart they are about to ship. The gate's first real run
    read `Cored, Guid, Widgetsd` from this project's own Debug tree, which is how
    the hole was found.
    """
    if name.endswith("d") and name[:-1] in FORBIDDEN_MODULES:
        return name[:-1]
    return name


def linked_modules(build_dir: Path) -> set[str]:
    """Every Qt module the build links, read from the generated build files."""
    found: set[str] = set()
    # build.ninja holds the link lines; *.vcxproj would hold them for MSBuild.
    for pattern in ("build.ninja", "**/link.txt", "**/build.make"):
        for path in build_dir.glob(pattern):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for match in LINKED_QT_RE.finditer(text):
                found.add(normalise_module(match.group(1)))
    return found


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    ap = argparse.ArgumentParser(description="Qt licence gate")
    ap.add_argument("--build-dir", action="append", default=None,
                    help="build tree to inspect; repeatable. Defaults to the "
                         "conventional directories that exist.")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    candidates = [Path(d) for d in (args.build_dir or DEFAULT_BUILD_DIRS)]
    existing = [d for d in candidates if (d / "build.ninja").exists()]

    if not existing:
        # Not a failure: a core-only checkout has no Qt at all, and that is the
        # configuration the project promises keeps working. Reporting it as an
        # error would make the gate red for everyone who never builds the GUI.
        if not args.quiet:
            print("qt-licence gate: no Qt build tree found "
                  f"(looked in {', '.join(str(d) for d in candidates)}); skipping")
        return 0

    violations: list[tuple[Path, str, str]] = []
    all_modules: set[str] = set()
    for build_dir in existing:
        modules = linked_modules(build_dir)
        all_modules |= modules
        for banned, reason in sorted(FORBIDDEN_MODULES.items()):
            if banned in modules:
                violations.append((build_dir, banned, reason))

    if not args.quiet:
        print(f"qt-licence gate: {len(existing)} build tree(s), "
              f"{len(all_modules)} Qt module(s) linked: "
              f"{', '.join(sorted(all_modules)) or 'none'}")

    for build_dir, banned, reason in violations:
        print(f"{build_dir}: [QT-LICENCE] Qt6{banned} is linked, but it is not "
              f"LGPL-compatible: {reason}", file=sys.stderr)
        print("    See THIRD_PARTY_NOTICES.md section 1 and "
              "docs/adr/ADR-0006-gui-framework-and-qt-licensing.md", file=sys.stderr)

    if violations:
        print(f"\nFAILED: {len(violations)} disqualifying Qt module(s)", file=sys.stderr)
        return 1
    if not args.quiet:
        print("PASSED: every linked Qt module is LGPL-compatible "
              "under the terms recorded in NOTICE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
