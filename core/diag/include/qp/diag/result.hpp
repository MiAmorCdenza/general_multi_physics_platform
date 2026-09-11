/**
 * @file result.hpp
 * @brief `Result<T>`：不抛异常的失败表达。
 *
 * 为什么不用异常：
 *   1. **热路径禁止异常**（plan-tree.md §8 铁律 5，由 clang-tidy 强制）。
 *      数值内核每帧跑几十万次，异常表的代价与展开检查不可接受。
 *   2. 插件跨边界（DLL / 子进程 / 未来的 Web）抛出的异常**无法安全穿越**。
 *   3. 异常把失败藏在类型之外，调用方容易忘记处理；`Result` 让失败进入类型系统。
 *
 * 与 `Consequence` 的关系：
 *   `Result` 说明"这次调用失败了"，`Consequence` 说明"失败有多严重"。
 *   两者都进入类型，宿主才能做降级决策。
 *
 * @ownership   pure（值类型）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   要么持有值，要么持有错误码，二者不同时成立
 * @errors      noexcept（本类型自身不抛；构造失败即 std::terminate）
 * @frozen      是（类模板形状冻结）
 * @tests       diag.result.ok_value, diag.result.err_only,
 *              diag.result.value_or_fallback, diag.result.monadic_chaining,
 *              diag.result.void_specialization, diag.result.no_throw_on_access
 */
#pragma once

#include <qp/diag/contract.hpp>
#include <qp/diag/error.hpp>

#include <optional>
#include <type_traits>
#include <utility>

namespace qp::diag {

/**
 * @brief 承载"值或错误码"的结果类型。
 *
 * @tparam T 成功时的值类型。必须是可析构的完整类型。
 *
 * 用法约定：
 *   - 取用值**必须**先检查 `has_value()`；未检查即取是编程错误（debug 下断言）。
 *   - `value_or(fallback)` 是唯一允许的无检查取值方式。
 */
template <class T>
class [[nodiscard]] Result final {
public:
    using value_type = T;

    /// @brief 构造成功结果。
    constexpr Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)), error_(ErrorCode::ok) {}

    /// @brief 构造失败结果。错误码不得为 ok。
    constexpr Result(ErrorCode code) noexcept : value_(std::nullopt), error_(code) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return error_ == ErrorCode::ok;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

    /// @brief 错误码。成功时为 `ErrorCode::ok`。
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return error_; }

    /**
     * @brief 取值的引用。
     *
     * @ownership   borrows（引用指向本对象内部，本对象移动后失效）
     * @thread      any
     * @pre         has_value() == true
     * @post        返回对内部值的引用，不复制
     * @invariant   引用在 Result 存活期内有效
     * @errors      noexcept；违反 @pre 时终止进程（不是异常，也不是 UB）
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       diag.result.ok_value, diag.result.no_throw_on_access
     */
    [[nodiscard]] constexpr const T& value() const noexcept {
        // 刻意不用 std::optional::value()：它在空时抛异常，而本项目禁止用异常表达失败。
        // 违反 @pre 是编程错误 → 终止（见 contract.hpp 的取舍说明）。
        QP_PRECONDITION(value_.has_value());
        return *value_;
    }

    /**
     * @brief 取值，失败时返回兜底值。**唯一允许不做检查的取值方式**。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        成功时返回内部值的副本，失败时返回 fallback
     * @invariant   不修改本对象
     * @errors      noexcept；若 T 的拷贝构造抛出，则 std::terminate
     *              （本项目不用异常表达失败，宁可确定性终止）
     * @complexity  O(copy(T))
     * @nondet      none
     * @frozen      否
     * @tests       diag.result.value_or_fallback
     */
    [[nodiscard]] constexpr T value_or(T fallback) const noexcept {
        return has_value() ? *value_ : std::move(fallback);
    }

    /// @brief 失败时的后果级别。成功时返回 recoverable（无意义，仅保证全覆盖）。
    [[nodiscard]] constexpr Consequence consequence() const noexcept {
        return has_value() ? Consequence::recoverable : default_consequence(error_);
    }

    /**
     * @brief 成功时变换值，失败时原样透传错误。
     *
     * @ownership   pure
     * @thread      any
     * @pre         f 可被 T 调用
     * @post        成功时返回 f(值) 的结果；失败时返回同一错误码
     * @invariant   错误码在链路中不被吞掉
     * @errors      取决于 f；f 抛异常则本函数抛
     * @complexity  取决于 f
     * @nondet      取决于 f
     * @frozen      否
     * @tests       diag.result.monadic_chaining
     */
    template <class F>
    [[nodiscard]] constexpr auto and_then(F&& f) const -> decltype(f(std::declval<T>())) {
        using R = decltype(f(std::declval<T>()));
        if (has_value()) return f(*value_);
        return R{error_};
    }

private:
    std::optional<T> value_;
    ErrorCode error_;
};

/**
 * @brief `Result<void>`：只表达成功/失败，不携带值。
 *
 * 这是最常用的一种——大多数图变异、校验、加载操作的失败信息就是错误码本身。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   要么成功，要么持有错误码
 * @errors      noexcept
 * @frozen      是
 * @tests       diag.result.void_specialization, diag.result.void_ok_is_truthy
 */
template <>
class [[nodiscard]] Result<void> final {
public:
    using value_type = void;

    constexpr Result() noexcept : error_(ErrorCode::ok) {}
    constexpr Result(ErrorCode code) noexcept : error_(code) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return error_ == ErrorCode::ok; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return error_; }
    [[nodiscard]] constexpr Consequence consequence() const noexcept {
        return has_value() ? Consequence::recoverable : default_consequence(error_);
    }

private:
    ErrorCode error_;
};

/// @brief 构造成功结果。
[[nodiscard]] constexpr Result<void> ok() noexcept { return Result<void>{}; }

/// @brief 构造失败结果。
[[nodiscard]] constexpr Result<void> fail(ErrorCode code) noexcept { return Result<void>{code}; }

}  // namespace qp::diag
