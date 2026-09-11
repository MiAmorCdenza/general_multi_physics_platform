/**
 * @file contract.hpp
 * @brief 前置条件检查：`@pre` 的执法工具。
 *
 * 契约规范（standards/function-contract.md）要求 `@pre` 违反时是**编程错误**，
 * 用断言而非返回错误码。本文件提供那个断言。
 *
 * 行为约定：
 *   - Debug（未定义 NDEBUG）：条件不成立即 `std::terminate`，消息含文件/行/表达式。
 *     刻意**不用 assert()**：NDEBUG 下它会被完全移除，而我们要求 debug 下必然停下。
 *   - Release：条件不成立立即 `std::terminate`。
 *     刻意**不"未定义行为"**：`@pre` 违反是编程错误，但错误也必须**确定性**，
 *     否则"同 seed 复现"（章程 R2）在遇到 bug 时会变成"有时崩有时不崩"。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   违反 @pre 必然终止进程，不返回、不继续
 * @errors      noexcept（本文件所有函数都不抛）
 * @frozen      否
 * @tests       diag.contract.holds_does_nothing, diag.contract.violation_terminates
 */
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace qp::diag {

/// @brief 契约违反时的终止处理。独立成函数便于测试与替换。
[[noreturn]] inline void contract_violation(const char* expr, const char* file,
                                            int line) noexcept {
    std::fprintf(stderr, "\n[qp] 契约违反（@pre 不成立）\n  %s\n  位于 %s:%d\n\n",
                 expr, file, line);
    std::fflush(stderr);
    std::terminate();
}

/**
 * @brief 前置条件检查。条件不成立即终止。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        条件成立时正常返回，无副作用
 * @invariant   条件不成立时**必然**终止进程（不返回）
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       diag.contract.holds_does_nothing, diag.contract.violation_terminates
 */
inline void precondition(bool holds, const char* expr, const char* file, int line) noexcept {
    if (!holds) contract_violation(expr, file, line);
}

}  // namespace qp::diag

/// @brief 检查前置条件。条件不成立 → 终止进程（见 contract.hpp 的说明）。
#define QP_PRECONDITION(expr) \
    ::qp::diag::precondition(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

/// @brief 检查不变量。语义同 QP_PRECONDITION，命名区分以便阅读与搜索。
#define QP_INVARIANT(expr) \
    ::qp::diag::precondition(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
