/**
 * @file quantity.hpp
 * @brief A value with a dimension: arithmetic whose dimensions the compiler enforces.
 *
 * Design points (charter §4.2 and ADR-0001):
 *   - The dimension is **part of the type**, not a runtime field. A wrong dimension = a compile error.
 *   - No implicit conversion to a scalar. Getting the bare number requires an explicit `.value()` --
 *     that step is the confirmation "I am giving up dimension protection".
 *
 * @frozen The class template shape in this file is frozen; new member functions are compatible additions.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Same dimensions can be combined, different dimensions cannot (enforced at compile time)
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      yes (`Quantity`'s class template shape and `value()` semantics)
 * @tests       units.quantity.energy_equivalence
 */
#pragma once

#include <qp/units/dim.hpp>

#include <cmath>
#include <type_traits>

namespace qp::units {

/**
 * @brief A scalar that carries a dimension.
 *
 * @tparam D The dimension. The numeric semantics are "the value expressed in SI base units".
 *
 * @ownership   pure (a value type, trivially copyable)
 * @thread      any
 * @pre         none
 * @post        value() returns the bare number as constructed, with no conversion
 * @invariant   The same type can be added; different types cannot (rejected at compile time)
 * @errors      noexcept; no failure mode
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes -- the class template shape and value() semantics are frozen
 * @tests       units.quantity.construct, units.quantity.value_roundtrip,
 *              units.quantity.add_same_dim, units.quantity.sub_same_dim,
 *              units.quantity.mul_dim_adds, units.quantity.div_dim_subtracts,
 *              units.quantity.scalar_multiply, units.quantity.negate,
 *              units.quantity.comparison, units.quantity.no_implicit_scalar,
 *              units.quantity.no_cross_dim_add
 */
template <Dim D>
class Quantity final {
public:
    using value_type = double;
    static constexpr Dim dim = D;

    /**
     * @brief Default-constructs to 0.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        value() == 0.0
     * @invariant   0 is the only value independent of the dimension
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     */
    constexpr Quantity() noexcept = default;

    /**
     * @brief Construct from a bare number. The number is interpreted in SI base units.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        value() == v
     * @invariant   No implicit conversion is provided: `Length x = 3.0;` must fail to compile
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      yes (the explicit property is frozen)
     */
    explicit constexpr Quantity(double v) noexcept : v_(v) {}

    /**
     * @brief Write the bare number in place (reusing the object, avoiding an allocation).
     *
     * @ownership   pure (modifies itself)
     * @thread      any
     * @pre         none
     * @post        value() == v
     * @invariant   The dimension is unchanged
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       units.quantity.set_updates_value
     */
    constexpr void set(double v) noexcept { v_ = v; }

    /**
     * @brief Read the bare number.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        The returned value equals the argument of the latest construction or set
     * @invariant   value(Quantity(x)) == x (for any finite x)
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      yes
     * @tests       units.quantity.value_roundtrip
     */
    [[nodiscard]] constexpr double value() const noexcept { return v_; }

    /**
     * @brief Whether the value is finite. The numeric kernel tests this before clamping (charter C2).
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        true if and only if value() is neither inf nor NaN
     * @invariant   Identical to std::isfinite(value())
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       units.quantity.is_finite
     */
    [[nodiscard]] bool is_finite() const noexcept { return std::isfinite(v_); }

private:
    double v_ = 0.0;
};

// -- Add and subtract: same dimension only ------------------------------------

/**
 * @brief Add two values of the same dimension.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns Quantity<D> holding the sum of the two
 * @invariant   Commutative; associative; (a + b) - b == a (exact only when no rounding occurs)
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.add_same_dim
 */
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator+(Quantity<D> a, Quantity<D> b) noexcept {
    return Quantity<D>{a.value() + b.value()};
}

/**
 * @brief Subtract two values of the same dimension.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns Quantity<D> holding the difference of the two
 * @invariant   The value of a - a is 0.0
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.sub_same_dim
 */
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator-(Quantity<D> a, Quantity<D> b) noexcept {
    return Quantity<D>{a.value() - b.value()};
}

/**
 * @brief Negate.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The number is negated, the dimension is unchanged
 * @invariant   -(-a) == a
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.negate
 */
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator-(Quantity<D> a) noexcept {
    return Quantity<D>{-a.value()};
}

// -- Multiply and divide: dimension arithmetic --------------------------------

/**
 * @brief Multiply: the numbers multiply, the dimensions add.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The result dimension is L + R; the number is the exact product
 * @invariant   Commutative, associative
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.mul_dim_adds, units.quantity.mul_commutative,
 *              units.quantity.mul_associative
 */
template <auto L, auto R>
[[nodiscard]] constexpr Quantity<L + R> operator*(Quantity<L> a, Quantity<R> b) noexcept {
    return Quantity<L + R>{a.value() * b.value()};
}

/**
 * @brief Divide: the numbers divide, the dimensions subtract.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The result dimension is L / R
 * @invariant   (a / b) * b has the same dimension as a
 * @errors      noexcept; a zero b produces inf/nan per IEEE 754, no exception
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.div_dim_subtracts
 */
template <auto L, auto R>
[[nodiscard]] constexpr Quantity<L / R> operator/(Quantity<L> a, Quantity<R> b) noexcept {
    return Quantity<L / R>{a.value() / b.value()};
}

/// @brief Multiply by a dimensionless scalar. A scalar does not change the dimension.
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator*(Quantity<D> a, double s) noexcept {
    return Quantity<D>{a.value() * s};
}
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator*(double s, Quantity<D> a) noexcept {
    return Quantity<D>{s * a.value()};
}
/// @brief Divide by a dimensionless scalar.
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator/(Quantity<D> a, double s) noexcept {
    return Quantity<D>{a.value() / s};
}

// -- Power: the dimension scales by an integer exponent -----------------------

namespace detail {

/// @brief Integer power, constexpr-friendly (exponentiation by squaring, no std::pow).
///
/// Difference from std::pow: an integer power uses repeated multiplication, so it **introduces no
/// libm precision loss** and may appear in a constant expression. Floating-point multiplication is
/// not associative, so the evaluation order is fixed here to keep pow<N>(x) reproducible (charter R2).
[[nodiscard]] constexpr double int_pow(double base, int exp) noexcept {
    if (exp == 0) return 1.0;
    const bool negative = exp < 0;
    unsigned int n = negative ? static_cast<unsigned int>(-(exp + 1)) + 1U
                              : static_cast<unsigned int>(exp);
    double result = 1.0;
    double factor = base;
    while (n > 0) {
        if ((n & 1U) != 0U) result *= factor;
        factor *= factor;
        n >>= 1U;
    }
    return negative ? 1.0 / result : result;
}

}  // namespace detail

/**
 * @brief Integer power.
 *
 * @ownership   pure
 * @thread      any
 * @pre         P is a compile-time integer
 * @post        The dimension is D to the P; the number is a to the P
 * @invariant   pow<1>(a) == a; pow<0>(a) is the dimensionless 1; pow<2>(a) has dimension D*D
 * @errors      noexcept; P<0 with a==0 produces inf per IEEE 754, no exception
 * @complexity  O(log|P|)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.pow_two, units.quantity.pow_zero,
 *              units.quantity.pow_constexpr, units.quantity.pow_is_deterministic,
 *              units.quantity.pow_matches_repeated_multiplication
 */
template <int P, auto D>
[[nodiscard]] constexpr Quantity<dim_pow<P>(D)> pow(Quantity<D> a) noexcept {
    if constexpr (P == 0) {
        return Quantity<Dim{}>{1.0};
    } else {
        return Quantity<dim_pow<P>(D)>{detail::int_pow(a.value(), P)};
    }
}

/**
 * @brief Square root: the dimension exponents are halved.
 *
 * Note: the dimension is only legal when every exponent is even. This function does not check --
 * the call site must guarantee it with a constexpr assertion. See the open item in spec.md.
 *
 * @ownership   pure
 * @thread      any
 * @pre         Every exponent of D is even
 * @post        The dimension exponents are half of D; the number is sqrt(a)
 * @invariant   sqrt_unchecked(pow<2>(x)) == x (within floating-point tolerance)
 * @errors      noexcept; a negative number gives NaN, no exception
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.sqrt_unchecked
 */
template <auto D>
[[nodiscard]] constexpr Quantity<dim_pow<1>(D)> sqrt_unchecked(Quantity<D> a) noexcept {
    return Quantity<D>{std::sqrt(a.value())};
}

// -- Comparison: same dimension only ------------------------------------------

/**
 * @brief Compare values of the same dimension.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a boolean according to the numeric order
 * @invariant   A total order (except for NaN, which follows IEEE 754)
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       units.quantity.comparison
 */
template <auto D>
[[nodiscard]] constexpr bool operator==(Quantity<D> a, Quantity<D> b) noexcept {
    return a.value() == b.value();
}
template <auto D>
[[nodiscard]] constexpr bool operator!=(Quantity<D> a, Quantity<D> b) noexcept {
    return !(a == b);
}
template <auto D>
[[nodiscard]] constexpr bool operator<(Quantity<D> a, Quantity<D> b) noexcept {
    return a.value() < b.value();
}
template <auto D>
[[nodiscard]] constexpr bool operator<=(Quantity<D> a, Quantity<D> b) noexcept {
    return a.value() <= b.value();
}
template <auto D>
[[nodiscard]] constexpr bool operator>(Quantity<D> a, Quantity<D> b) noexcept {
    return a.value() > b.value();
}
template <auto D>
[[nodiscard]] constexpr bool operator>=(Quantity<D> a, Quantity<D> b) noexcept {
    return a.value() >= b.value();
}

}  // namespace qp::units
