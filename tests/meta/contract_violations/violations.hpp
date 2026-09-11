/**
 * @file contract_violations.hpp
 * @brief 门禁反例样本：每条违规都必须被 check_contracts.py 抓到。
 *
 * 本文件**不是**生产代码，只被 tests/meta/check_contract_checker.py 使用。
 * 对应 standards/enforcement.md §8：未经验证的门禁等于没有门禁。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   本文件内的合规样本不产生任何违规
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      否
 * @tests       meta.sample.case, meta.another.case
 */
#pragma once

namespace qp::meta {

// ── 反例 1：缺 @tests（应触发 C2）────────────────────────────────────────────
/**
 * @brief 缺少 @tests 字段的函数。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回输入
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 */
[[nodiscard]] inline int missing_tests(int x) noexcept { return x; }

// ── 反例 2：@tests 引用不存在的用例（应触发 C4）──────────────────────────────
/**
 * @brief 引用了不存在的测试用例。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回输入
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       meta.this_test_case_does_not_exist
 */
[[nodiscard]] inline int bad_test_id(int x) noexcept { return x; }

// ── 反例 3：@errors 与 noexcept 不一致（应触发 C5）───────────────────────────
/**
 * @brief 声称 noexcept 但签名允许抛出。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回输入
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       meta.sample.case
 */
[[nodiscard]] inline int noexcept_mismatch(int x) { return x; }

// ── 反例 4：@ownership 取值非法（应触发 C3）─────────────────────────────────
/**
 * @brief ownership 写了个不存在的取值。
 *
 * @ownership   whatever
 * @thread      any
 * @pre         none
 * @post        返回输入
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       meta.sample.case
 */
[[nodiscard]] inline int bad_ownership(int x) noexcept { return x; }

// ── 正例：完全合规，不应产生任何违规 ────────────────────────────────────────
/**
 * @brief 合规样本，用于确认门禁不会误报。
 *
 * @ownership   pure
 * @thread      any
 * @pre         x 为任意整数
 * @post        返回 x
 * @invariant   无
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       meta.sample.case
 */
[[nodiscard]] inline int good_sample(int x) noexcept { return x; }

}  // namespace qp::meta
