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

# 层目录：它们只是**分类**，不是模块。模块是层下面的一级。
# 例：`qp/graph/ir/node.hpp` 属于模块 ir，不属于模块 graph。
LAYER_DIRS = frozenset({"graph", "runtime", "authoring"})

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
    # mutate 的命令携带 qp::ports::Value（SetParam 的参数载荷），
    # 因此必须允许 mutate → ports。这是 value 类型定义在 ports 的必然结果。
    "mutate": {"units", "diag", "ports", "ir", "structure"},
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
    """把 include 路径映射到 core 模块名，也用于源码路径。

    目录约定（兼容两种历史写法）：
      `qp/units/dim.hpp`         → units
      `qp/diag/result.hpp`       → diag
      `qp/graph/ir/node.hpp`     → ir      （层目录被跳过）
      `qp/abi/field_buffer.hpp`  → abi
      `qp/units.hpp`             → units   伞头文件
      `qp/graph/ir.hpp`          → ir      层目录下的伞头文件

    实现：剔除 `include` / `src` 这类目录约定噪声后，
    从 `qp/` 之后的目录里取**第一个非层目录**；若全被过滤掉
    （即路径形如 `qp/<layer>/<file>`），则取文件名词干。
    """
    parts = [p for p in Path(include).parts if p not in ("include", "src")]
    if len(parts) < 2 or parts[0] != "qp":
        return None

    dirs = list(parts[1:-1])          # qp 与文件名之间的目录段
    modules = [d for d in dirs if d not in LAYER_DIRS]
    if modules:
        return modules[0]

    # 没有模块目录：路径是 qp/<layer>/<file> 或 qp/<file>
    return Path(parts[-1]).stem or None


def module_of_relative(rel_parts: tuple[str, ...]) -> str | None:
    """从**相对 core 根**的路径判断所属模块。

    支持两种目录约定：
      - 开发约定：`<layer>/<mod>/include/qp/...` → 取 `qp/` 之后的段
      - 简化约定：`<mod>/include/qp/...`         → 取第 0 段

    关键：判断逻辑必须与 `module_of()` **完全一致**，否则"文件属于哪个模块"
    与"include 指向哪个模块"会用两套规则，产生自相矛盾的判定。
    早期版本只做 `rel_parts[0]`，于是 `graph/ir/...` 被误判为模块 "graph"，
    与 include 解析出的 "ir" 对不上——由层级门禁自身抓出。

    因此这里统一委托给 `module_of()`：先定位路径里的 `qp` 段，
    再把 `qp/...` 整段交给它解析。
    """
    if not rel_parts:
        return None
    rest = tuple(p for p in rel_parts if p not in ("include", "src"))
    # 路径形如 <layer>/<mod>/qp/... 或 <mod>/qp/...：从 `qp` 段开始交给 module_of
    if "qp" in rest:
        i = rest.index("qp")
        return module_of("/".join(rest[i:]), Path("."))
    # 源码路径（无 qp 段）：形如 <layer>/<mod>/<file> 或 <mod>/<file>
    dirs = [d for d in rest[:-1] if d not in LAYER_DIRS]
    if dirs:
        return dirs[0]
    # 全部被过滤（如 <layer>/<file>）：用文件名兜底
    return Path(rest[-1]).stem if rest else None


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
