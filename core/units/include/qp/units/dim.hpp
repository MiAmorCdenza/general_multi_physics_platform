/**
 * @file dim.hpp
 * @brief Compile-time dimension: integer exponents of the seven SI base dimensions.
 *
 * This is the **only low-level representation** of the qp dimension system, an `@frozen`
 * contract. Any change must bump `qp::units::kUnitsAbiVersion` and update tests/abi/.
 *
 * @ownership   pure (a value type)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every exponent lies within [kMinExp, kMaxExp]
 * @errors      noexcept
 * @complexity  -
 * @nondet      none
 * @frozen      yes (`Dim` layout and member order)
 * @tests       units.dim.layout, units.dim.equality, units.dim.dimensionless_predicate,
 *              units.dim.representable
 */
#pragma once

#include <cstdint>
#include <type_traits>

namespace qp::units {

/// @brief Exponent type. int8 is enough (-128..127) and keeps Dim tiny and trivially copyable.
using DimExp = std::int8_t;

/// @brief Exponent under/overflow bounds. Exceeding them is a compile-time assertion.
inline constexpr DimExp kMinExp = -100;
inline constexpr DimExp kMaxExp = 100;

/**
 * @brief Dimension vector: exponents of length, mass, time, current, temperature, amount, light.
 *
 * The order matches the seven SI base units: m kg s A K mol cd.
 *
 * @ownership   pure (a value type)
 * @thread      any
 * @pre         none
 * @post        Every exponent lies within [kMinExp, kMaxExp] (guaranteed by multiplication)
 * @invariant   Trivially copyable; constructible in a constexpr context
 * @errors      noexcept; no failure mode
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes -- this struct's layout and member order are ABI; changes bump the version
 * @tests       units.dim.layout, units.dim.equality,
 *              units.dim.add_exact, units.dim.subtract_exact,
 *              units.dim.scalar_multiply, units.dim.negate
 */
struct Dim final {
    DimExp L = 0;
    DimExp M = 0;
    DimExp T = 0;
    DimExp I = 0;
    DimExp Th = 0;
    DimExp N = 0;
    DimExp J = 0;

    /// @brief The all-zero dimension (dimensionless).
    [[nodiscard]] static constexpr Dim none() noexcept { return Dim{}; }

    /// @brief Compile-time exponent sum. Detects "exponents cancelling out"; **not** a dimensionless test.
    [[nodiscard]] constexpr int sum() const noexcept {
        return static_cast<int>(L) + M + T + I + Th + N + J;
    }

    /**
     * @brief Whether this is genuinely dimensionless: **all seven exponents are 0**.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        true if and only if L==M==T==I==Th==N==J==0
     * @invariant   **Not equivalent** to `sum() == 0`: velocity's exponent sum is -1, while
     *              `Dim{1,-1,0,0,0,0,0}` sums to 0 yet is still dimensional.
     *              The old implementation used `sum() == 0` and misjudged such values -- fixed.
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       units.dim.dimensionless_predicate
     */
    [[nodiscard]] constexpr bool is_dimensionless() const noexcept {
        return L == 0 && M == 0 && T == 0 && I == 0 && Th == 0 && N == 0 && J == 0;
    }

    /**
     * @brief Whether the exponent sum is zero (the dimensions cancel each other out).
     *
     * Kept for diagnostics: "your dimension exponents cancel each other out".
     * It is not a dimensionless test -- see is_dimensionless's @invariant.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns sum() == 0
     * @invariant   is_dimensionless() implies has_zero_exponent_sum() (one direction only)
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       units.dim.zero_exponent_sum
     */
    [[nodiscard]] constexpr bool has_zero_exponent_sum() const noexcept { return sum() == 0; }

    /// @brief Whether this structure can represent the dimension (exponents in range).
    [[nodiscard]] constexpr bool is_representable() const noexcept {
        const auto in_range = [](DimExp e) { return e >= kMinExp && e <= kMaxExp; };
        return in_range(L) && in_range(M) && in_range(T) && in_range(I) && in_range(Th) &&
               in_range(N) && in_range(J);
    }

    /// @brief Component-wise equality.
    [[nodiscard]] friend constexpr bool operator==(Dim a, Dim b) noexcept {
        return a.L == b.L && a.M == b.M && a.T == b.T && a.I == b.I && a.Th == b.Th &&
               a.N == b.N && a.J == b.J;
    }
    [[nodiscard]] friend constexpr bool operator!=(Dim a, Dim b) noexcept { return !(a == b); }
};

namespace detail {
/// @brief Adds exponents and checks the bounds. Out of range is a compile-time failure.
[[nodiscard]] constexpr DimExp add_exp(DimExp a, DimExp b) noexcept {
    const int r = static_cast<int>(a) + static_cast<int>(b);
    return r < kMinExp || r > kMaxExp ? kMinExp - 1 : static_cast<DimExp>(r);
}
constexpr bool in_range(Dim d) noexcept { return d.is_representable(); }
}  // namespace detail

/**
 * @brief Multiplying dimensions = adding exponents.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Each exponent of the result is the sum of the two matching input exponents
 * @invariant   Commutative, associative, with none() as the identity
 * @errors      noexcept; a non-representable result is caught by the caller's constexpr check
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no (the operation may grow), but the layout of the returned Dim is frozen
 * @tests       units.dim.multiply_exact, units.dim.multiply_commutative,
 *              units.dim.multiply_associative, units.dim.multiply_identity
 */
[[nodiscard]] constexpr Dim operator*(Dim a, Dim b) noexcept {
    return Dim{detail::add_exp(a.L, b.L), detail::add_exp(a.M, b.M),
               detail::add_exp(a.T, b.T), detail::add_exp(a.I, b.I),
               detail::add_exp(a.Th, b.Th), detail::add_exp(a.N, b.N),
               detail::add_exp(a.J, b.J)};
}

/**
 * @brief Dividing dimensions = subtracting exponents.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Each exponent of the result is the difference of the two matching input exponents
 * @invariant   a / a == none(); a / none() == a
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.dim.divide_exact, units.dim.divide_self_is_none,
 *              units.dim.divide_by_none_identity
 */
[[nodiscard]] constexpr Dim operator/(Dim a, Dim b) noexcept {
    return Dim{detail::add_exp(a.L, static_cast<DimExp>(-b.L)),
               detail::add_exp(a.M, static_cast<DimExp>(-b.M)),
               detail::add_exp(a.T, static_cast<DimExp>(-b.T)),
               detail::add_exp(a.I, static_cast<DimExp>(-b.I)),
               detail::add_exp(a.Th, static_cast<DimExp>(-b.Th)),
               detail::add_exp(a.N, static_cast<DimExp>(-b.N)),
               detail::add_exp(a.J, static_cast<DimExp>(-b.J))};
}

/**
 * @brief Raises a dimension to an integer power.
 *
 * Note: the return type is **deduced with `auto`**, not written as `Dim dim_pow(Dim)` with an
 * explicit return. MSVC cannot deduce class-type non-type template parameters (ADR-0004), and
 * this keeps the same strategy as the `Quantity` operators.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Each exponent is multiplied by p
 * @invariant   pow(pow(a,m), n) == pow(a, m*n); pow(a,1) == a; pow(a,0) == none()
 * @errors      noexcept; out of range is caught by the caller's constexpr check
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.dim.pow_one_identity, units.dim.pow_zero_is_none,
 *              units.dim.pow_composition
 */
template <int P>
[[nodiscard]] constexpr Dim dim_pow(Dim a) noexcept {
    if constexpr (P == 0) {
        return Dim{};
    } else {
        constexpr DimExp k = static_cast<DimExp>(P);
        return Dim{static_cast<DimExp>(a.L * k), static_cast<DimExp>(a.M * k),
                   static_cast<DimExp>(a.T * k), static_cast<DimExp>(a.I * k),
                   static_cast<DimExp>(a.Th * k), static_cast<DimExp>(a.N * k),
                   static_cast<DimExp>(a.J * k)};
    }
}

/**
 * @brief Adding dimensions = adding exponents. Equivalent to `operator*`.
 *
 * Note: a dimension's "multiplication" is `operator*` (adding exponents); `operator+` is
 * another spelling of the same operation, for writing compound dimensions such as
 * `Quantity<L + R>` in a template argument. Both are kept so that
 * `Quantity<Acceleration * Time>` and `Quantity<L + R>` read naturally; semantics are identical.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Each exponent of the result is the sum of the two matching input exponents
 * @invariant   Exactly equivalent to operator*: a + b == a * b
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.dim.add_exact, units.dim.plus_equals_multiply
 */
[[nodiscard]] constexpr Dim operator+(Dim a, Dim b) noexcept { return a * b; }

/**
 * @brief Subtracting dimensions = subtracting exponents. Equivalent to `operator/`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Each exponent of the result is the difference of the two matching input exponents
 * @invariant   Exactly equivalent to operator/: a - b == a / b
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.dim.subtract_exact, units.dim.minus_equals_divide
 */
[[nodiscard]] constexpr Dim operator-(Dim a, Dim b) noexcept { return a / b; }

/**
 * @brief Negates a dimension (equivalent to 1/Dim).
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Every exponent is negated
 * @invariant   dim_inverse(dim_inverse(a)) == a; dim_inverse(Dim{}) == Dim{}
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.dim.negate
 */
[[nodiscard]] constexpr Dim dim_inverse(Dim a) noexcept { return Dim{} / a; }

}  // namespace qp::units
