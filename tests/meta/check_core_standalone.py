#!/usr/bin/env python3
"""Test of the charter's central claim: `views/` and `plugins/` can be deleted.

`docs/plan-tree.md` states this as a promise, and the layer gate enforces the include
half of it: no header under `core/` may include one from `views/` or `plugins/`. That
gate is necessary and not sufficient.

## What the include gate cannot see

A directory can be required without a single include pointing at it:

  - CMake may reference it unconditionally, so configuration fails the moment it is
    absent. This is the failure that matters most in practice, because the person who
    hits it is a downstream consumer who vendored `core/` alone, and the error they see
    names a path they deliberately did not take.
  - A test may read a file out of it at runtime, which turns "core's suite passes
    without plugins" into "core's suite passes because nobody ran it that way".
  - A tool or script may assume it exists.

`docs/plan-tree.md` also calls `views/` and `plugins/` **content**, and calls the ability
to drop them the core assertion of the whole layering scheme. A claim that load-bearing
deserves an executable check rather than a paragraph.

## What this does, and what it deliberately does not

It copies the build inputs into a temporary tree with the two content directories
removed, configures it with both content flags **off**, and asserts that configuration
succeeds. That is precisely the consumer's situation: their checkout has no `plugins/`,
and they want `core/`.

It does not build. Configuration is where an unconditional `add_subdirectory` fails, and
building the whole of core to prove the same point would put minutes on every test run
for a check that a full CI build already performs. The `cmake_graph` gate covers the
built side.

Exit code: 0 the claim holds; 1 it does not, or the check could not run.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent

# Directories that make up "core plus what it needs to configure".
#
# Copied wholesale rather than symlinked: a symlinked `plugins/` would still be visible
# to CMake through the link, which is exactly the thing being removed. Copying also keeps
# the check honest on Windows, where directory symlinks need privileges.
COPY = ("core", "cmake", "external", "tests")
COPY_FILES = ("CMakeLists.txt",)

# Content directories. The claim is that removing these changes nothing for core.
CONTENT = ("plugins", "views")


def _run_configure(tree: Path) -> tuple[bool, str]:
    """Configures `tree` with both content flags off. Returns (ok, output)."""
    build = tree / "build-check"
    argv = [
        "cmake",
        "-S",
        str(tree),
        "-B",
        str(build),
        "-DQP_BUILD_PLUGINS=OFF",
        "-DQP_BUILD_VIEWS=OFF",
        # Tests off as well: the point is that core configures, and a test target that
        # needs a content directory would be a second, separate finding -- one this
        # script reports through the structural checks below rather than through CMake.
        "-DQP_BUILD_TESTS=OFF",
    ]
    try:
        # `chcp 65001` first: CMake decodes a compiler's output using the console code
        # page, and on a Chinese Windows a legacy page makes the MSVC dependency prefix
        # unreadable. That is a build concern rather than a configure one, but the same
        # configure is run by hand on this machine and the cost of including it is zero.
        proc = subprocess.run(
            argv,
            cwd=str(tree),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=300,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return False, f"could not run cmake: {exc}"
    out = (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode == 0, out


def _structural_findings() -> list[str]:
    """Cheap checks for a dependency on a content directory that no include reveals."""
    findings: list[str] = []

    # 1. No CMake file may add a content directory from outside a guard.
    #
    # The first version of this check looked for the guard text on the same line as the
    # `add_subdirectory` call, and therefore flagged `if(QP_BUILD_VIEWS AND EXISTS ...)`
    # followed by an indented `add_subdirectory(views/model)` -- a correctly guarded
    # pair. A checker that reports the fix as the fault is worse than no checker: the
    # natural response is to loosen it until it stops complaining.
    #
    # So this tracks nesting depth instead. A content directory added at depth zero is
    # unconditional by construction, whatever the guard's spelling; one added inside any
    # `if(...)` is guarded, whatever the guard's condition. Depth counting over CMake is
    # approximate -- it does not understand `foreach`/`endfor` or block-argument forms --
    # which is why the configure below is the real evidence and this is only a second,
    # faster signal.
    for cmake_file in sorted(REPO.rglob("CMakeLists.txt")):
        rel = cmake_file.relative_to(REPO).as_posix()
        # A content directory's own CMakeLists may refer to itself freely.
        if rel.split("/")[0] in CONTENT:
            continue
        text = cmake_file.read_text(encoding="utf-8", errors="replace")
        depth = 0
        for line_no, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            # `endif()` first: a line is never both.
            if re.match(r"^endif\b", stripped):
                depth = max(0, depth - 1)
                continue
            if re.match(r"^if\b", stripped):
                depth += 1
                continue
            for name in CONTENT:
                if re.match(rf"^add_subdirectory\s*\(\s*\"?{re.escape(name)}[/\")]", stripped):
                    if depth == 0:
                        findings.append(
                            f"{rel}:{line_no}: add_subdirectory({name}) is at the top level of "
                            f"this file, so deleting {name}/ would break configuration"
                        )

    # 2. Nothing under core/ may reach into a content directory by path, which the
    #    include gate would miss because it only looks at `#include` lines.
    for path in sorted((REPO / "core").rglob("*")):
        if not path.is_file() or path.suffix not in {".hpp", ".cpp", ".h", ".cc"}:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name in CONTENT:
            for needle in (f'"{name}/', f"<{name}/", f"../{name}/"):
                if needle in text:
                    findings.append(
                        f"{path.relative_to(REPO).as_posix()}: refers to a path under {name}/ "
                        f"({needle!r}); core must not depend on content, even by a string path"
                    )
                    break

    return findings


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    for name in CONTENT:
        if not (REPO / name).is_dir():
            print(f"note: {name}/ is absent from this checkout; the claim is trivially true")

    findings = _structural_findings()

    with tempfile.TemporaryDirectory(prefix="qp-core-standalone-") as tmp:
        tree = Path(tmp) / "repo"
        tree.mkdir()

        for name in COPY:
            src = REPO / name
            if src.is_dir():
                # `external/` holds Catch2 and possibly a Qt kit; neither is needed with
                # tests off, and copying a Qt installation would take far longer than the
                # whole check is worth. An empty directory keeps the include paths valid.
                if name == "external":
                    (tree / name).mkdir()
                    continue
                shutil.copytree(src, tree / name, symlinks=False, ignore=shutil.ignore_patterns(
                    "build", "build-*", ".git", "__pycache__"))
        for name in COPY_FILES:
            shutil.copy2(REPO / name, tree / name)

        # The whole point: these do not exist in the consumer's tree.
        for name in CONTENT:
            if (tree / name).exists():
                findings.append(f"internal error: {name}/ was copied into the check tree")
                print(f"  [FAIL] {name}/ was copied into the check tree", file=sys.stderr)

        ok, output = _run_configure(tree)

        if ok and not findings:
            print("  [OK  ] core configures with views/ and plugins/ removed")
            print("  [OK  ] no unguarded reference to a content directory")
            print("核心可独立构建：content 目录删除后配置成功。")
            return 0

        if not ok:
            findings.insert(
                0,
                "core does NOT configure without views/ and plugins/. CMake output:\n"
                + "\n".join("      " + ln for ln in output.strip().splitlines()[-25:]),
            )

    print("\n核心独立性自检失败：", file=sys.stderr)
    for f in findings:
        print("  - " + f, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
