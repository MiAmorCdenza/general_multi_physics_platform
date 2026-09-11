#!/usr/bin/env python3
"""门禁自身的测试：验证 check_contracts.py 真的能抓到每类违规。

对应 standards/enforcement.md §8。

未经验证的门禁等于没有门禁——它给你虚假的安全感。
本脚本用 tests/meta/contract_violations/ 下的**故意违规样本**，
逐条确认对应规则被触发；同时确认合规样本不产生误报。

退出码：0 全部符合预期；1 有偏差。
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CHECKER = REPO / "scripts" / "check_contracts.py"
FIXTURES = HERE / "contract_violations"
FIXTURE_HEADER = FIXTURES / "violations.hpp"
FIXTURE_TESTS = FIXTURES / "declared_tests.json"


def load_checker():
    """按文件路径加载门禁模块（scripts 不是包）。"""
    spec = importlib.util.spec_from_file_location("qp_check_contracts", CHECKER)
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_contracts"] = module  # dataclass 需要它
    spec.loader.exec_module(module)
    return module


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    for required in (CHECKER, FIXTURE_HEADER, FIXTURE_TESTS):
        if not required.is_file():
            print(f"错误：找不到 {required}", file=sys.stderr)
            return 1

    cc = load_checker()

    contracts, violations = cc.parse_header(FIXTURE_HEADER)
    declared_tests = set(json.loads(FIXTURE_TESTS.read_text(encoding="utf-8"))["test_cases"])

    # 把两阶段违规合并：C4（引用不存在的用例）只在拿到"用例字典"后才能判定。
    by_line: dict[int, set[str]] = {}
    for v in violations:
        by_line.setdefault(v.line, set()).add(v.rule)
    for fc in contracts:
        for tid in fc.test_ids:
            if tid not in declared_tests:
                by_line.setdefault(fc.line, set()).add("C4")

    expectations = [
        ("missing_tests", "C2"),
        ("bad_test_id", "C4"),
        ("noexcept_mismatch", "C5"),
        ("bad_ownership", "C3"),
    ]

    failures: list[str] = []
    print(f"门禁自检：{FIXTURE_HEADER.name} 解析出 {len(contracts)} 个契约、"
          f"{len(violations)} 条解析期违规")

    text_lines = FIXTURE_HEADER.read_text(encoding="utf-8").splitlines()
    for func_name, rule in expectations:
        target = next((i + 1 for i, line in enumerate(text_lines) if func_name + "(" in line), None)
        if target is None:
            failures.append(f"反例样本里找不到函数 {func_name}")
            continue
        hit = any(rule in rules for line, rules in by_line.items() if abs(line - target) <= 1)
        print(f"  [{'OK  ' if hit else 'FAIL'}] {func_name:20s} 期望规则 {rule}")
        if not hit:
            failures.append(f"{func_name} 未触发 {rule}（该行实际："
                            f"{by_line.get(target, set()) or '无'}）")

    # ── 合规样本不得产生任何违规 ──
    good_line = next((i + 1 for i, line in enumerate(text_lines) if "good_sample(" in line), None)
    if good_line is None:
        failures.append("反例样本里找不到 good_sample")
    elif good_line in by_line:
        failures.append(f"合规样本 good_sample 被误报：{by_line[good_line]}")
        print(f"  [FAIL] good_sample 被误报 {by_line[good_line]}")
    else:
        print("  [OK  ] good_sample          无违规（无误报）")

    # ── 模块级契约：允许存在，但必须能与非契约的文件头区分开 ──
    module_contracts = [c for c in contracts if c.signature == "<module>"]
    misassigned = [c for c in contracts if c.signature != "<module>" and "file" in c.tags]
    if misassigned:
        failures.append("纯文件头被误识别为函数契约")
        print("  [FAIL] 纯文件头被误配给函数")
    elif len(module_contracts) != 1:
        failures.append(f"模块级契约数量异常：{len(module_contracts)}（期望 1）")
        print(f"  [FAIL] 模块级契约数量 {len(module_contracts)}")
    else:
        print("  [OK  ] 模块级契约与函数契约区分正确")

    if failures:
        print("\n门禁自检失败：", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1

    print("门禁自检通过：四类违规均被抓到，合规样本无误报。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
