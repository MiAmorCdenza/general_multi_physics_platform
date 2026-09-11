/**
 * @file result.hpp
 * @brief `Result<T>`: expressing failure without throwing.
 *
 * Why not exceptions:
 *   1. **Exceptions are banned on the hot path** (plan-tree.md section 8, iron rule 5, enforced by clang-tidy).
 *      The numeric kernel runs hundreds of thousands of times per frame; exception tables and unwind checks cost too much.
 *   2. An exception thrown across a plugin boundary (DLL / subprocess / future Web) **cannot travel safely**.
 *   3. Exceptions hide failure outside the type, so callers forget to handle it; `Result` puts failure into the type system.
 *
 * Relation to `Consequence`:
 *   `Result` says "this call failed", `Consequence` says "how severe the failure is".
 *   Both enter the type, which is what lets the host make degradation decisions.
 *
 * @ownership   pure (value type)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   it either holds a value or holds an error code, never both
 * @errors      noexcept (this type itself does not throw; a failed construction means std::terminate)
 * @frozen      yes (the class template shape is frozen)
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
 * @brief Result type that carries "a value or an error code".
 *
 * @tparam T The value type on success. Must be a destructible complete type.
 *
 * Usage rules:
 *   - Taking the value **must** be preceded by a `has_value()` check; taking it unchecked is a programming error (asserted in debug).
 *   - `value_or(fallback)` is the only unchecked way to take the value.
 */
template <class T>
class [[nodiscard]] Result final {
public:
    using value_type = T;

    /// @brief Construct a success result.
    constexpr Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)), error_(ErrorCode::ok) {}

    /// @brief Construct a failure result. The error code must not be ok.
    constexpr Result(ErrorCode code) noexcept : value_(std::nullopt), error_(code) {}

    [[nodiscard]] constexpr bool has_value() const noexcept {
        return error_ == ErrorCode::ok;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_value(); }

    /// @brief The error code. `ErrorCode::ok` on success.
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return error_; }

    /**
     * @brief Reference to the value.
     *
     * @ownership   borrows (the reference points inside this object and dangles once it moves)
     * @thread      any
     * @pre         has_value() == true
     * @post        returns a reference to the internal value, without copying
     * @invariant   the reference stays valid for the lifetime of the Result
     * @errors      noexcept; violating @pre terminates the process (not an exception, and not UB)
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       diag.result.ok_value, diag.result.no_throw_on_access
     */
    [[nodiscard]] constexpr const T& value() const noexcept {
        // Deliberately not std::optional::value(): that throws when empty, and this project forbids exceptions for failure.
        // Violating @pre is a programming error -> terminate (see the trade-off note in contract.hpp).
        QP_PRECONDITION(value_.has_value());
        return *value_;
    }

    /**
     * @brief Take the value, or a fallback on failure. **The only unchecked way to take a value**.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        returns a copy of the internal value on success, or fallback on failure
     * @invariant   does not modify this object
     * @errors      noexcept; if T's copy constructor throws, std::terminate
     *              (this project does not express failure with exceptions; it prefers deterministic termination)
     * @complexity  O(copy(T))
     * @nondet      none
     * @frozen      no
     * @tests       diag.result.value_or_fallback
     */
    [[nodiscard]] constexpr T value_or(T fallback) const noexcept {
        return has_value() ? *value_ : std::move(fallback);
    }

    /// @brief Consequence level on failure. Returns recoverable on success (meaningless; only keeps the switch total).
    [[nodiscard]] constexpr Consequence consequence() const noexcept {
        return has_value() ? Consequence::recoverable : default_consequence(error_);
    }

    /**
     * @brief Transform the value on success; pass the error straight through on failure.
     *
     * @ownership   pure
     * @thread      any
     * @pre         f can be called with T
     * @post        returns the result of f(value) on success; returns the same error code on failure
     * @invariant   the error code is never swallowed along the chain
     * @errors      depends on f; if f throws, this function throws
     * @complexity  depends on f
     * @nondet      depends on f
     * @frozen      no
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
 * @brief `Result<void>`: expresses only success/failure and carries no value.
 *
 * This is the most common form -- for most graph mutations, validations, and load operations the failure information is the error code itself.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   it either succeeds or holds an error code
 * @errors      noexcept
 * @frozen      yes
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

/// @brief Construct a success result.
[[nodiscard]] constexpr Result<void> ok() noexcept { return Result<void>{}; }

/// @brief Construct a failure result.
[[nodiscard]] constexpr Result<void> fail(ErrorCode code) noexcept { return Result<void>{code}; }

}  // namespace qp::diag
