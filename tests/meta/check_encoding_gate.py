#!/usr/bin/env python3
"""编码门禁自身的测试：验证 check_encoding.py 的 E1/E2/E3 都能抓到。

对应 standards/enforcement.md §8：未经验证的门禁等于没有门禁。

夹具写在临时目录里而不是仓库里：E1/E2 的反例本身就是"不该存在于仓库的文件"，
放进仓库会让真实门禁永远失败。E3 的反例是一个无 BOM 的 `.ps1`，同理。

退出码：0 全部符合预期；1 有偏差。
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CHECKER = REPO / "scripts" / "check_encoding.py"

BOM = b"\xef\xbb\xbf"


def load_checker():
    spec = importlib.util.spec_from_file_location("qp_check_encoding", CHECKER)
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_encoding"] = module
    spec.loader.exec_module(module)
    return module


def rules_for(ce, path: Path) -> set[str]:
    return {v.rule for v in ce.check_file(path)}


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    if not CHECKER.exists():
        print(f"错误：找不到 {CHECKER}", file=sys.stderr)
        return 1

    ce = load_checker()
    failures: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def write(name: str, data: bytes) -> Path:
            p = root / name
            p.write_bytes(data)
            return p

        # -- Negative cases: these must be caught --
        # (name, bytes, expected rule set)
        negative = [
            ("bad_utf16.cpp", "// note\n".encode("utf-16"), {"E1"}),
            ("bad_latin1.hpp", b"// caf\xe9\n", {"E1"}),
            # A "normal looking" Chinese comment stored as GBK: the realistic E1 case.
            ("bad_gbk.cpp", "// \u6d4b\u8bd5\n".encode("gbk"), {"E1"}),
            ("crlf.cpp", b"// a\r\n// b\r\n", {"E2"}),
            ("lone_cr.py", b"# a\r# b\n", {"E2"}),
            ("crlf.md", b"# title\r\n\r\nbody\r\n", {"E2"}),
            ("no_bom.ps1", b"# comment\n", {"E3"}),
            ("no_bom_ps1_with_cjk.ps1", "# \u4e2d\u6587\n".encode("utf-8"), {"E3"}),
        ]

        # -- Positive cases: these must not be reported --
        positive = [
            ("ok.cpp", b"// ascii comment\n"),
            ("ok_cjk_utf8.cpp", "// \u4e2d\u6587\u6ce8\u91ca\n".encode("utf-8")),
            ("ok_bom.ps1", BOM + b"# comment\n"),
            ("ok_bom_cjk.ps1", BOM + "# \u4e2d\u6587\n".encode("utf-8")),
            ("ok.md", "# \u6807\u9898\n\n\u6b63\u6587\u3002\n".encode("utf-8")),
            ("ok_gitattributes", b"* text=auto eol=lf\n"),
        ]

        print(f"编码门禁自检：{len(negative)} 个反例 + {len(positive)} 个正例")

        for name, data, expected in negative:
            found = rules_for(ce, write(name, data))
            ok = found == expected
            print(f"  [{'OK  ' if ok else 'FAIL'}] {name:28s} 期望 {sorted(expected)}，"
                  f"实际 {sorted(found) or '无'}")
            if not ok:
                failures.append(f"{name}: 期望 {sorted(expected)}，实际 {sorted(found) or '无'}")

        for name, data in positive:
            found = rules_for(ce, write(name, data))
            ok = not found
            detail = "; ".join(f"{v.rule}:{v.detail}" for v in found)
            print(f"  [{'OK  ' if ok else 'FAIL'}] {name:28s} {'无误报' if ok else detail}")
            if not ok:
                failures.append(f"{name} 被误报：{detail}")

        # -- Suffix filter: binaries and unrelated suffixes must not be scanned --
        # A PNG containing NUL and invalid UTF-8: reading it as text would produce a false E1.
        write("image.png", b"\x89PNG\r\n\x1a\n\x00\xff\xfe")
        write("blob.exe", bytes(range(256)))
        write("data.bin", b"\x00\x01\x02\r\n")
        scanned, violations = ce.collect(root)
        names = {v.path.name for v in violations}
        leaked = names & {"image.png", "blob.exe", "data.bin"}
        ok = not leaked
        print(f"  [{'OK  ' if ok else 'FAIL'}] 二进制豁免            扫描 {scanned} 个文件，"
              f"{'无泄漏' if ok else '被误报：' + str(sorted(leaked))}")
        if not ok:
            failures.append(f"二进制文件被当作文本扫描：{sorted(leaked)}")

        # -- Excluded directories --
        for skip in ("build", ".git", "external", "__pycache__", ".venv"):
            d = root / skip
            d.mkdir()
            (d / "bad.cpp").write_bytes(b"// a\r\n")
        _, violations = ce.collect(root)
        leaked = {v.path.parts[-2] for v in violations} & {"build", ".git", "external",
                                                          "__pycache__", ".venv"}
        ok = not leaked
        print(f"  [{'OK  ' if ok else 'FAIL'}] 排除目录              "
              f"{'生效' if ok else '失效：' + str(sorted(leaked))}")
        if not ok:
            failures.append(f"排除目录失效：{sorted(leaked)}")

    if failures:
        print("\n编码门禁自检失败：", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1

    print("编码门禁自检通过：E1/E2/E3 均能抓到，合法文件与二进制无误报，排除目录生效。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
