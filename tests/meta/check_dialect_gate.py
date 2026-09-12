#!/usr/bin/env python3
"""Self-test for the code dialect gate: verifies check_dialect.py really catches each violation class.

Corresponds to standards/enforcement.md §8: an unverified gate is no gate at all.

The fixtures here are deliberately **strings in memory**, not files on disk:
`scan_text` is the gate's decision core, so writing fixtures as strings tests that logic
directly and keeps no batch of "violating files the real gate would scan" in the repository --
such fixtures landing on the scan path would make the whole-repo gate fail forever.
Only collect()'s directory walk and suffix filtering need real files, so those use a temp dir.

Exit code: 0 everything as expected; 1 a deviation.
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CHECKER = REPO / "scripts" / "check_dialect.py"

EMOJI = "\U0001f600"          # U+1F600 emoji
DECORATION = "\u2728"          # U+2728 decoration (in the U+2700-U+27BF range)
CIRCLED = "\u2460"             # U+2460 circled digit one (decorative numbering)
ARROW = "\u2192"               # U+2192 arrow: not D1, but D3 (in source comments)
BOX_H = "\u2500"               # U+2500 box-drawing horizontal: same as above
BLOCK = "\u2588"               # U+2588 solid block: same as above
TRIANGLE = "\u25b2"            # U+25B2 geometric shape: neither D1 nor D3 (legitimate in a design note)
CJK = "\u4e2d\u6587"           # the two characters meaning "Chinese"


def load_checker():
    spec = importlib.util.spec_from_file_location("qp_check_dialect", CHECKER)
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_dialect"] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    if not CHECKER.exists():
        print(f"错误：找不到 {CHECKER}", file=sys.stderr)
        return 1

    cd = load_checker()
    failures: list[str] = []

    # -- Negative cases: must be caught ((name, text, check as source, expected rules)) --
    negative = [
        ("emoji_in_comment",
         f"// ready {EMOJI}\n",
         True, {"D1"}),
        ("emoji_in_source_line",
         f"const char* s = \"ok {EMOJI}\";\n",
         True, {"D1"}),
        ("emoji_in_markdown",
         f"# 标题 {EMOJI}\n",
         False, {"D1"}),
        ("decoration_in_cmake",
         f"# step done {DECORATION}\n",
         True, {"D1"}),
        ("circled_digit_in_source",
         f"// step {CIRCLED} do the thing\n",
         True, {"D1"}),
        ("arrow_in_source_comment",
         f"// a {ARROW} b\n",
         True, {"D3"}),
        ("box_drawing_in_source_comment",
         f"// {BOX_H}{BOX_H}{BOX_H} section {BOX_H}{BOX_H}{BOX_H}\n",
         True, {"D3"}),
        ("block_in_python_comment",
         f"# bar {BLOCK}{BLOCK}\n",
         True, {"D3"}),
        ("cjk_line_comment",
         f"// {CJK}注释\n",  # qp-dialect-allow: D2 fixture
         True, {"D2"}),
        ("cjk_trailing_comment",
         f"int x = 1;  // {CJK}\n",
         True, {"D2"}),
        ("cjk_python_comment",
         f"# {CJK}\n",
         True, {"D2"}),
        ("cjk_cmake_comment",
         f"# {CJK}\n",
         True, {"D2"}),
        ("emoji_and_cjk_together",
         f"// {CJK} {EMOJI}\n",  # qp-dialect-allow: D1+D2 fixture
         True, {"D1", "D2"}),
        ("arrow_and_cjk_together",
         f"// {CJK} {ARROW} x\n",  # qp-dialect-allow: D2+D3 fixture
         True, {"D2", "D3"}),
    ]

    # -- Positive cases: must not be misreported --
    positive = [
        ("ascii_comment",
         "// Returns the node count; zero when the graph is empty.\n",
         True),
        ("ascii_arrow",
         "// in --> out; box is +-- ... --+\n",
         True),
        ("triangle_in_design_note",
         f"// The curve is monotonic: {TRIANGLE} increasing, no inflection.\n",
         True),          # a geometric shape is not D1 and is not decorative drawing
        ("ascii_string_literal",
         "const char* s = \"run 3 completed in 1.5 s\";\n",
         True),
        ("escaped_unicode_in_string",
         "const char* s = \"caf\\u00e9\";\n",
         True),
        ("cjk_string_literal_in_source",
         "const char* s = \"\u8fd0\u884c\u5b8c\u6210\";  \n",
         True),          # a localized string may live in a literal (D2 looks at comments only)
        ("arrow_in_string_literal",
         f"const char* s = \"{ARROW}\";\n",
         True),          # an arrow inside a string is not governed by the comment rules
        # A PowerShell here-string body is data. The opening `@"` is on an earlier
        # line, so a per-line scanner cannot tell the body from a comment -- and the
        # false positive lands on exactly the lines that are correct, which is how a
        # rule gets weakened instead of fixed. Found on the localized help text in
        # scripts/package.ps1.
        ("here_string_body_is_not_a_comment",
         'Fail @"\n' + CJK + ' help text\n"@\n',
         True),
        ("here_string_terminator_is_not_a_comment",
         'Fail @"\nbody\n"@\n',
         True),
        ("cjk_prose_in_markdown",
         f"# 规格说明\n\n{CJK}的散文按中文书写。\n",
         False),         # documentation is not subject to D2
        ("diagram_in_markdown",
         f"# Flow\n\n a {ARROW} b, +{BOX_H}{BOX_H}+\n",
         False),         # documentation is not subject to D3: a diagram is for people to read
        ("emoji_free_markdown",
         "# Logging schema\n\nOne JSON object per line.\n",
         False),
    ]

    print(f"方言门禁自检：{len(negative)} 个反例 + {len(positive)} 个正例")

    for name, text, check_comments, expected in negative:
        found = {v.rule for v in cd.scan_text(text, Path(name), check_comments)}
        ok = found == expected
        print(f"  [{'OK  ' if ok else 'FAIL'}] {name:28s} 期望 {sorted(expected)}，实际 {sorted(found) or '无'}")
        if not ok:
            failures.append(f"{name}: 期望 {sorted(expected)}，实际 {sorted(found) or '无'}")

    for name, text, check_comments in positive:
        found = cd.scan_text(text, Path(name), check_comments)
        ok = not found
        detail = "; ".join(f"{v.rule}:{v.snippet}" for v in found)
        print(f"  [{'OK  ' if ok else 'FAIL'}] {name:28s} {'无误报' if ok else detail}")
        if not ok:
            failures.append(f"{name} 被误报：{detail}")

    # -- Suffix classification: decides whether D2 applies --
    for suffix, expected in ((".hpp", True), (".cpp", True), (".py", True), (".ps1", True),
                             (".txt", True), (".md", False), (".json", False), (".yml", False)):
        actual = cd.is_source(suffix)
        ok = actual == expected
        print(f"  [{'OK  ' if ok else 'FAIL'}] is_source({suffix:6s}) = {actual}")
        if not ok:
            failures.append(f"is_source({suffix}) = {actual}，期望 {expected}")

    # -- Directory walk and exclusions --
    # This part needs real files: it tests collect()'s walk and suffix filtering.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "src").mkdir()
        (root / "build").mkdir()
        (root / ".git").mkdir()
        (root / "external").mkdir()

        (root / "src" / "good.hpp").write_text("// fine\n", encoding="utf-8")
        (root / "src" / "doc.md").write_text(f"# {CJK}\n", encoding="utf-8")
        (root / "src" / "notes.txt").write_text(f"// {CJK}\n", encoding="utf-8")
        # violations inside excluded directories must not be counted
        (root / "build" / "bad.hpp").write_text(f"// {CJK}\n", encoding="utf-8")
        (root / ".git" / "bad.hpp").write_text(f"// {CJK}\n", encoding="utf-8")
        (root / "external" / "bad.hpp").write_text(f"// {CJK}\n", encoding="utf-8")
        # unregistered suffixes must not be scanned
        (root / "src" / "image.png").write_bytes(b"\x89PNG\r\n\x1a\n")
        (root / "src" / "binary.exe").write_bytes(bytes(range(256)))

        scanned, violations = cd.collect(root)
        rules = sorted(v.rule for v in violations)
        # Expected: good.hpp (none) + doc.md (D2 not applicable, none) + notes.txt (D2)
        ok = scanned == 3 and rules == ["D2"]
        print(f"  [{'OK  ' if ok else 'FAIL'}] collect() 扫描 {scanned} 个文件（期望 3），"
              f"违规 {rules or '无'}（期望 ['D2']）")
        if not ok:
            failures.append(f"collect() 扫描 {scanned} 个文件、违规 {rules}，期望 3 / ['D2']")
            # A scan of 0 out of a fixture that was just written is not a rule failure -- it means the walk
            # did not see the files at all, and the cause is outside this file (a temp directory the walk
            # skipped, a filesystem that did not show the writes yet, something deleting them underneath).
            # Observed twice and not reproducible; the listing is what makes the next occurrence diagnosable
            # instead of mysterious, which is the difference between a flaky gate and a known one.
            if scanned == 0:
                listing = sorted(str(q.relative_to(root)) for q in root.rglob("*"))
                failures.append("  fixture listing at scan time: " + ", ".join(listing))
                failures.append(f"  fixture root: {root}")

    if failures:
        print("\n方言门禁自检失败：", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1

    print("方言门禁自检通过：D1/D2/D3 均能抓到，合法内容无误报，排除目录生效。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
