#!/usr/bin/env python3
"""层级门禁自身的测试：验证 check_layers.py 真的能抓到每类违规。

对应 standards/enforcement.md §8：未经验证的门禁等于没有门禁。

反例放在 tests/meta/layer_fixtures/<case>/include/qp/<module>/<file>.hpp，
每个反例目录被当成一个独立的 core 根来检查（因为真实的 core 里不可能
放这些违规文件）。

退出码：0 全部符合预期；1 有偏差。
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CHECKER = REPO / "scripts" / "check_layers.py"
FIXTURES = HERE / "layer_fixtures"


def load_checker():
    spec = importlib.util.spec_from_file_location("qp_check_layers", CHECKER)
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_layers"] = module
    spec.loader.exec_module(module)
    return module


def scan(cc, case_dir: Path) -> list:
    """把某个反例目录当作 core 根来扫描。

    注意 core 根取 `case_dir`（而不是 `case_dir/include`），
    与真实仓库里 `core/units/include/qp/...` 的结构一致：
    模块名始终是相对于 core 根的第一段或 qp 之后的段。
    """
    violations = []
    for path in sorted(p for p in case_dir.rglob("*") if p.suffix in (".hpp", ".h", ".cpp")):
        violations.extend(cc.check_file(path, case_dir, case_dir, cc.ALLOWED))
    return violations


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    for required in (CHECKER, FIXTURES):
        if not required.exists():
            print(f"错误：找不到 {required}", file=sys.stderr)
            return 1

    cc = load_checker()

    # (反例目录, 期望被抓到的规则)
    #
    # 目录约定说明：反例目录被整体当作 core 根，因此内部必须与真实仓库同构——
    # 开发约定 `<mod>/include/qp/<mod>/<file>.hpp`（见 module_of_relative）。
    expectations = [
        ("bad_units_to_abi", "L1"),      # 方向不允许
        ("bad_qt_in_core", "L2"),        # Qt 进 core
        ("bad_reverse_dep", "L4"),       # 反向依赖消费者
        ("bad_unregistered", "L5"),      # 未登记模块
    ]

    failures: list[str] = []
    print(f"层级门禁自检：{len(expectations)} 个反例 + 1 个正例")

    for case, rule in expectations:
        case_dir = FIXTURES / case
        if not case_dir.is_dir():
            failures.append(f"缺反例目录 {case}")
            continue
        violations = scan(cc, case_dir)
        rules = {v.rule for v in violations}
        hit = rule in rules
        print(f"  [{'OK  ' if hit else 'FAIL'}] {case:24s} 期望 {rule}，实际 {sorted(rules) or '无'}")
        if not hit:
            failures.append(f"{case} 未触发 {rule}（实际 {sorted(rules) or '无'}）")

    # ── 正例：合法依赖不得被误报 ──
    ok_dir = FIXTURES / "ok_diag_to_units"
    ok_violations = scan(cc, ok_dir)
    if ok_violations:
        details = "; ".join(f"{v.rule}:{v.message}" for v in ok_violations)
        failures.append(f"合法依赖被误报：{details}")
        print(f"  [FAIL] ok_diag_to_units        被误报 {details}")
    else:
        print("  [OK  ] ok_diag_to_units        无违规（无误报）")

    # ── 白名单自身一致性 ──
    # 每个被允许依赖的模块名也必须在 ALLOWED 里有自己的条目，
    # 否则说明白名单里出现了拼写错误或遗漏（依赖方向会静默失效）。
    dangling: list[str] = []
    for owner, deps in cc.ALLOWED.items():
        for dep in deps:
            if dep not in cc.ALLOWED:
                dangling.append(f"{owner} → {dep}")
    if dangling:
        failures.append(f"ALLOWED 引用了未登记的模块：{dangling}")
        print(f"  [FAIL] 白名单悬挂引用 {dangling}")
    else:
        print("  [OK  ] ALLOWED 自洽（无悬挂引用）")

    if failures:
        print("\n层级门禁自检失败：", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1

    print("层级门禁自检通过：四类违规均被抓到，合法依赖无误报。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
