#!/usr/bin/env python3
"""Test of the gate itself: verify that check_contracts.py really catches each kind of violation.

Corresponds to standards/enforcement.md §8.

An unverified gate is no gate at all -- it gives false confidence. This script uses the
**deliberately violating samples** under tests/meta/contract_violations/ and confirms rule by rule
that the matching violation is triggered; it also confirms that a compliant sample is not reported.

Exit code: 0 everything as expected; 1 a deviation.
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
    """Load the gate module by file path (scripts is not a package)."""
    spec = importlib.util.spec_from_file_location("qp_check_contracts", CHECKER)
    module = importlib.util.module_from_spec(spec)
    sys.modules["qp_check_contracts"] = module  # dataclass needs this
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

    # Merge the two phases: C4 (referencing a nonexistent case) is only decidable once the case set is known.
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

    # -- The compliant sample must produce no violation --
    good_line = next((i + 1 for i, line in enumerate(text_lines) if "good_sample(" in line), None)
    if good_line is None:
        failures.append("反例样本里找不到 good_sample")
    elif good_line in by_line:
        failures.append(f"合规样本 good_sample 被误报：{by_line[good_line]}")
        print(f"  [FAIL] good_sample 被误报 {by_line[good_line]}")
    else:
        print("  [OK  ] good_sample          无违规（无误报）")

    # -- Module-level contracts: allowed, but distinguishable from a non-contract file header --
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

    # -- Test selection: --only and --exclude must be duals with one spelling --
    #
    # The views gate hands one subtree to a second invocation, so "which cases are
    # mine" is a real mechanism with a real failure mode. It failed once: `--only`
    # compared its terms against the tests-root-relative path while `--exclude`
    # compared against the absolute path too, so `--only tests/model` matched nothing.
    # The gate then saw an empty case set and reported every id in the other module as
    # unreferenced -- twelve violations naming units, for a filter typo. The two
    # spellings have to be one spelling, and both directions have to be exercised.
    #
    # These run against the real tests tree, so they also pin the property that matters
    # in production: the two filters partition the cases rather than overlapping.
    tests_root = REPO / "tests"
    everything = cc.collect_test_ids(tests_root)
    model_ids = cc.collect_test_ids(tests_root, None, {"model"})
    unit_ids = cc.collect_test_ids(tests_root, None, {"unit"})
    complement = cc.collect_test_ids(tests_root, {"model"}, None)

    if not everything:
        failures.append("测试树里一个用例都没收集到——选择器可能把全部用例滤掉了")
        print("  [FAIL] 测试树用例总数为 0")
    elif not model_ids:
        failures.append("--only model 收集到 0 个用例：过滤语义可能又退回了绝对路径匹配")
        print("  [FAIL] --only model 收集到 0 个用例")
    elif model_ids & unit_ids:
        failures.append(f"--only model 与 --only unit 相交：{sorted(model_ids & unit_ids)}")
        print("  [FAIL] --only 两个不相交作用域出现交集")
    elif complement != everything - model_ids:
        failures.append("--exclude 与 --only 不是互为补集："
                        f"--exclude model 得到 {len(complement)} 个、"
                        f"全集减 --only model 得到 {len(everything - model_ids)} 个")
        print("  [FAIL] --exclude 与 --only 语义不对称")
    elif cc.collect_test_ids(tests_root, None, {"nonexistent/dir"}):
        # An allowlist that matches nothing must select nothing. If it ever falls back
        # to "match everything", a caller with a typo in its scope silently checks the
        # whole tree instead of its own subtree -- and reports every other module's
        # cases as orphans, which is the exact failure this flag was added to end.
        failures.append("--only 匹配不到任何目录时应当返回空集并让上层报错，"
                        "而不是静默返回全集")
        print("  [FAIL] --only 空匹配没有返回空集")
    else:
        print(f"  [OK  ] --only / --exclude 互为补集（model {len(model_ids)} + "
              f"其余 {len(complement)} = {len(everything)}）")

    if failures:
        print("\n门禁自检失败：", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1

    print("门禁自检通过：四类违规均被抓到，合规样本无误报。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
