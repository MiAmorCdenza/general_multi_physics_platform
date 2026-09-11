#!/usr/bin/env python3
"""Test of the layer gate itself: verify that check_layers.py really catches each kind of violation.

Corresponds to standards/enforcement.md §8: an unverified gate is no gate at all.

The counterexamples live under tests/meta/layer_fixtures/<case>/include/qp/<module>/<file>.hpp, and
each counterexample directory is checked as a standalone core root (the real core could never
contain these violating files).

Exit code: 0 everything as expected; 1 a deviation.
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
    """Scan one counterexample directory as a core root.

    Note the core root is `case_dir` (not `case_dir/include`), matching the structure of
    `core/units/include/qp/...` in the real repository: the module name is always the first
    segment relative to the core root, or the segment after qp.
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

    # (counterexample directory, rule expected to be caught)
    #
    # Directory convention: a counterexample directory is treated as a whole core root, so its
    # contents must mirror the real repository -- `<mod>/include/qp/<mod>/<file>.hpp` (see module_of_relative).
    expectations = [
        ("bad_units_to_abi", "L1"),      # direction not allowed
        ("bad_qt_in_core", "L2"),        # Qt inside core
        ("bad_reverse_dep", "L4"),       # reverse-dependency consumer
        ("bad_unregistered", "L5"),      # unregistered module
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

    # -- Positive case: a legal dependency must not be reported --
    ok_dir = FIXTURES / "ok_diag_to_units"
    ok_violations = scan(cc, ok_dir)
    if ok_violations:
        details = "; ".join(f"{v.rule}:{v.message}" for v in ok_violations)
        failures.append(f"合法依赖被误报：{details}")
        print(f"  [FAIL] ok_diag_to_units        被误报 {details}")
    else:
        print("  [OK  ] ok_diag_to_units        无违规（无误报）")

    # -- Whitelist self-consistency --
    # Every module name that may be depended on must also have its own entry in ALLOWED;
    # otherwise the whitelist contains a typo or an omission (a dependency direction silently dies).
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
