#!/usr/bin/env python3
"""层级依赖门禁（standards/enforcement.md §4）。

把 `docs/plan-tree.md` §8 的依赖铁律从"文档约定"变成"CI 拒绝条件"。

检查项：
  L1 允许方向：core 内的 include 必须符合显式白名单
  L2 禁止 Qt：core 下不得出现任何 Q* 头文件
  L3 Eigen 受限：只允许出现在 eval / kernels
  L4 禁止反向依赖：core 不得包含 views / plugins 的头文件
  L5 白名单覆盖：出现了规则里没有的模块 → 报错，强制维护者显式登记

设计要点：**白名单是显式的**。新增跨模块依赖必须改本文件，
改文件这个动作本身就是"我知道我在扩大耦合"的确认。这是故意的摩擦。

退出码：0 通过；1 有违规。
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')

# ── 允许的依赖方向（模块 → 它被允许依赖的模块集合）──────────────────────────
#
# 与 docs/plan-tree.md §8 的白名单保持同步。新增模块必须在此登记，
# 否则 L5 会报"未登记模块"。
ALLOWED: dict[str, set[str]] = {
    # L0 地基
    "units": set(),
    "diag": {"units"},
    "reflect": {"units"},
    "abi": {"units"},
    # ports 需要 abi 的 LatticeDesc 作为 Value 的场句柄载荷——
    # 端口要能传递场，而场的布局定义在 abi。这条依赖是刻意的。
    "ports": {"units", "diag", "abi"},
    # L1 图与执行
    "ir": {"units", "diag", "ports"},
    "structure": {"units", "diag", "ir"},
    "mutate": {"units", "diag", "ir", "structure"},
    "validate": {"units", "diag", "ports", "ir", "structure"},
    "eval": {"units", "diag", "ports", "abi", "ir", "structure"},
    "domain": {"units", "diag", "abi", "ir", "eval"},
    "kernels": {"units", "diag", "abi", "domain"},
    "field": {"units", "diag", "abi"},
    # L2 运行与数据
    "run": {"units", "diag", "abi", "ports"},
    "store": {"units", "diag", "ports"},
    "trace": {"units", "diag", "run", "store"},
    "io": {"units", "diag", "abi", "store"},
    # L3 视图服务
    "document": {"units", "diag", "ir", "abi"},
    "capability": {"units", "diag", "ports"},
    "commands": {"units", "diag", "ir", "structure", "mutate"},
    "layout": {"units", "diag", "ir", "document"},
    "portui": {"units", "diag", "ports", "capability"},
}

# core 之外的一切都不许被 core 依赖
FORBIDDEN_PREFIXES = ("views", "plugins", "external")

# Eigen 只允许在这两个模块出现
EIGEN_ALLOWED = {"eval", "kernels"}


@dataclass
class Violation:
    path: Path
    line: int
    rule: str
    message: str

    def render(self, root: Path) -> str:
        try:
            shown = self.path.relative_to(root)
        except ValueError:
            shown = self.path
        return f"{shown}:{self.line}: [{self.rule}] {self.message}"


def module_of(include: str, core_root: Path) -> str | None:
    """把 include 路径映射到 core 模块名。

    `qp/units/dim.hpp`        → units
    `qp/diag/result.hpp`      → diag
    `qp/graph/ir/node.hpp`    → ir      （graph/ 是层，ir 才是模块）
    `qp/abi/field_buffer.hpp` → abi
    `qp/units.hpp`            → units   **伞头文件**：qp 之后直接是文件名

    伞头文件是每个模块的公开入口（`qp/units.hpp`、`qp/diag.hpp` …），
    不能把它当成模块名 "units.hpp"。早期版本正是这样误判的。
    """
    parts = Path(include).parts
    if len(parts) < 2 or parts[0] != "qp":
        return None

    # 伞头文件：qp/<module>.hpp
    if len(parts) == 2:
        stem = Path(parts[1]).stem
        return stem or None

    second = parts[1]
    if second == "graph" and len(parts) >= 3:
        return parts[2]
    if second in ("runtime", "authoring") and len(parts) >= 3:
        return parts[2]
    return second


def module_of_relative(rel_parts: tuple[str, ...]) -> str | None:
    """从**相对 core 根**的路径判断所属模块。

    支持两种目录约定：
      - 开发约定：`<mod>/include/qp/...`  → 取第 0 段
      - 纯头文件：`qp/<mod>/...` 或 `qp/graph/<mod>/...` → 取 qp 之后的段

    早期版本只有后一种路径识别逻辑，导致用第一种约定的文件被判成模块 "qp"
    ——由 tests/meta 的层级反例抓出。现在两条路径统一走本函数。
    """
    if not rel_parts:
        return None
    if rel_parts[0] == "qp":
        return module_of("qp/" + "/".join(rel_parts[1:]), Path("."))
    return rel_parts[0]


def check_file(path: Path, core_root: Path, root: Path,
               allowed: dict[str, set[str]]) -> list[Violation]:
    violations: list[Violation] = []
    try:
        rel = path.relative_to(core_root)
    except ValueError:
        return violations
    own = module_of_relative(rel.parts)
    if own is None:
        return violations

    for lineno, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        m = INCLUDE_RE.match(raw)
        if not m:
            continue
        inc = m.group(1)

        # L2 禁止 Qt
        if (re.match(r"^Q[A-Z]\w*$", inc)
                or inc.startswith(("QtCore/", "QtGui/", "QtQuick", "QtWidgets", "QtQml"))):
            violations.append(Violation(path, lineno, "L2",
                                        f"core 内不得包含 Qt：{inc}"))
            continue

        # L4 禁止反向依赖
        if inc.split("/")[0] in FORBIDDEN_PREFIXES:
            violations.append(Violation(path, lineno, "L4",
                                        f"core 不得依赖 {inc.split('/')[0]}/：{inc}"))
            continue

        # L3 Eigen 受限
        if inc.startswith("Eigen/") or inc == "Eigen":
            if own not in EIGEN_ALLOWED:
                violations.append(Violation(
                    path, lineno, "L3",
                    f"Eigen 只允许出现在 {sorted(EIGEN_ALLOWED)}，当前模块是 {own!r}：{inc}"))
            continue

        # 只检查 core 内的相互依赖
        dep = module_of(inc, core_root)
        if dep is None:
            continue

        # L5 未登记模块
        if dep not in allowed:
            violations.append(Violation(path, lineno, "L5",
                                        f"未登记的 core 模块 {dep!r}（请在本脚本的 ALLOWED 中显式加入）"))
            continue
        if own not in allowed:
            violations.append(Violation(path, lineno, "L5",
                                        f"当前模块 {own!r} 未在 ALLOWED 中登记"))
            continue

        # L1 允许方向
        if dep == own:
            continue
        if dep not in allowed[own]:
            violations.append(Violation(
                path, lineno, "L1",
                f"不允许的依赖 {own} → {dep}（{own} 允许：{sorted(allowed[own]) or '无'}）"))

    return violations


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="core 层级依赖门禁")
    parser.add_argument("--core", default=str(repo_root / "core"), help="core 根目录")
    parser.add_argument("--root", default=str(repo_root), help="用于相对路径显示")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    core_root = Path(args.core)
    root = Path(args.root).resolve()
    if not core_root.is_dir():
        print(f"错误：core 目录不存在 {core_root}", file=sys.stderr)
        return 1

    files = sorted(p for p in core_root.rglob("*") if p.suffix in (".hpp", ".h", ".cpp"))
    violations: list[Violation] = []
    for path in files:
        violations.extend(check_file(path, core_root, root, ALLOWED))

    if not args.quiet:
        print(f"层级检查：扫描 {len(files)} 个文件，登记模块 {len(ALLOWED)} 个")

    for v in violations:
        print(v.render(root))

    if violations:
        print(f"\n门禁失败：{len(violations)} 处层级违规", file=sys.stderr)
        return 1
    if not args.quiet:
        print("门禁通过：层级依赖合法。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
