#!/usr/bin/env python3
"""Qt 许可门禁自身的测试：验证 check_qt_modules.py 真的能拦住不该链的模块。

对应 standards/enforcement.md §8：未经验证的门禁等于没有门禁。

夹具是**合成的 `build.ninja`**，写在临时目录里。理由和 dialect 门禁的自检一样：
反例本身就是「不该存在的构建树」，放进仓库会让真实门禁永远失败。而门禁读的正是
生成出来的链接行，所以合成一份链接行就是最贴近真实的夹具。

这里测三件事，缺一不可：

  1. **反例必须被拦下**——GPL 模块被链接时报错。
  2. **正例不得误报**——只链 LGPL 模块时放行。只测反例的门禁挡不住「把一切都判违规」。
  3. **没有 Qt 构建树时必须放行**——这一条最容易被写错成失败，而它恰恰是项目承诺的
     配置：core 在没有 Qt 的机器上必须能构建并通过全部测试。

退出码：0 全部符合预期；1 有偏差。
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CHECKER = REPO / "scripts" / "check_qt_modules.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("qp_check_qt", CHECKER)
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_qt"] = module
    spec.loader.exec_module(module)
    return module


def write_ninja(build_dir: Path, libs: list[str]) -> None:
    """写一份最小 build.ninja，链接行里带上给定的 Qt 库。

    形状照着 Ninja 生成的真实 `build.ninja` 来：链接命令是一行长文本，
    库以 `C:\\Qt\\...\\Qt6Xxx.lib` 的形式出现。门禁只关心 `.lib` 的名字，
    所以这里用简化的路径。
    """
    build_dir.mkdir(parents=True, exist_ok=True)
    line = "  LINK_LIBRARIES = " + " ".join(
        rf"C:\Qt\6.8.3\msvc2022_64\lib\{lib}.lib" for lib in libs)
    (build_dir / "build.ninja").write_text(
        "# synthetic fixture\nbuild app.exe: CXX_EXECUTABLE_LINKER app.obj\n" + line + "\n",
        encoding="utf-8")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    if not CHECKER.exists():
        print(f"错误：找不到 {CHECKER}", file=sys.stderr)
        return 1

    qc = load_checker()
    failures: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        print("Qt 许可门禁自检：4 个反例 + 2 个正例 + 1 个跳过场景")

        # -- Negative cases: these must be caught --
        # (name, linked Qt libraries, module expected to trip the gate)
        negative = [
            ("charts_is_gpl", ["Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Charts"], "Charts"),
            ("qml_is_forbidden", ["Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Quick"], "Quick"),
            ("webengine_bundles_chromium",
             ["Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6WebEngineWidgets"], "WebEngineWidgets"),
            # Qt names a debug build `Qt6Chartsd.lib`. A gate comparing raw file
            # names would let this through -- and a Debug build is exactly what a
            # developer links while prototyping the chart they are about to ship.
            # This hole was found by running the gate on this project's own tree.
            ("charts_debug_suffix",
             ["Qt6Cored", "Qt6Guid", "Qt6Widgetsd", "Qt6Chartsd"], "Charts"),
        ]
        for name, libs, expected in negative:
            build = root / name
            write_ninja(build, libs)
            modules = qc.linked_modules(build)
            hit = expected in {m for m in modules if m in qc.FORBIDDEN_MODULES}
            ok = hit and expected in modules
            print(f"  [{'OK  ' if ok else 'FAIL'}] {name:26s} 链接 {libs[-1]}，"
                  f"期望命中 {expected}")
            if not ok:
                failures.append(f"{name}: 未命中 {expected}（实际模块 {sorted(modules)}）")

        # -- Positive cases: these must not be reported --
        positive = [
            ("only_lgpl_modules", ["Qt6Core", "Qt6Gui", "Qt6Widgets"]),
            ("tools_are_allowed",
             ["Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Svg", "Qt6Concurrent"]),
        ]
        for name, libs in positive:
            build = root / name
            write_ninja(build, libs)
            modules = qc.linked_modules(build)
            caught = sorted(m for m in modules if m in qc.FORBIDDEN_MODULES)
            ok = not caught
            print(f"  [{'OK  ' if ok else 'FAIL'}] {name:26s} "
                  f"{'无误报' if ok else '误报 ' + str(caught)}")
            if not ok:
                failures.append(f"{name} 被误报：{caught}")

        # -- Skip case: with no Qt build tree the gate must pass --
        # This is the configuration the project promises keeps working (core
        # builds and passes with Qt absent). Treating it as a failure would turn
        # the gate red for everyone who only builds core, and then it gets ignored.
        empty = root / "no_qt_here"
        empty.mkdir()
        rc = qc.main(["--build-dir", str(empty), "--quiet"])
        ok = rc == 0
        print(f"  [{'OK  ' if ok else 'FAIL'}] 无 Qt 构建树              "
              f"{'放行（退出码 0）' if ok else f'退出码 {rc}，期望 0'}")
        if not ok:
            failures.append(f"无 Qt 构建树时退出码 {rc}，期望 0")

        # -- The extractor itself: it must read a realistically shaped link line --
        build = root / "extract"
        write_ninja(build, ["Qt6Core", "Qt6Gui", "Qt6Widgets"])
        modules = qc.linked_modules(build)
        ok = modules == {"Core", "Gui", "Widgets"}
        print(f"  [{'OK  ' if ok else 'FAIL'}] 模块提取                "
              f"得到 {sorted(modules)}")
        if not ok:
            failures.append(f"linked_modules 得到 {sorted(modules)}，期望 Core/Gui/Widgets")

    if failures:
        print("\nQt 许可门禁自检失败：", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1

    print("Qt 许可门禁自检通过：GPL/QML 模块必被拦下（含 debug 后缀），"
          "LGPL 模块无误报，无 Qt 构建树时正确跳过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
