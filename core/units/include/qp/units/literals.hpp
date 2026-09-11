/**
 * @file literals.hpp
 * @brief User-defined literals: `5.0_m`, `9.8_m_s2`, `250.0_mm` and so on.
 *
 * Positioning (important):
 *   A literal is only **syntax sugar** bound to the literal itself -- variables and expression
 *   results cannot use it. What actually prevents dimension errors is the `Quantity<D>` type system.
 *   This file makes "writing a constant" read naturally; it is not a safety mechanism.
 *
 * Conversion conventions:
 *   - C++ requires a floating-point literal operator to take `long double`.
 *   - Every conversion is done in `long double`, and only the **final step** narrows to double
 *     through `from_long_double()`: the only narrowing point in the file, centralized and auditable.
 *   - Conversion factors are always written as double, avoiding `1000.0L` mixed-precision promotion.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The converted result matches SI base units (250.0_mm has the value 0.25)
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no (the literal set is extensible)
 * @tests       units.literals.base_units, units.literals.engineering_prefixes,
 *              units.literals.derived_units
 */
#pragma once

#include <qp/units/dimensions.hpp>

namespace qp::units::literals {

namespace detail {

/// @brief The file's only long double -> double narrowing point.
[[nodiscard]] constexpr double from_long_double(long double v) noexcept {
    return static_cast<double>(v);
}

}  // namespace detail

// -- Base quantities ----------------------------------------------------------
[[nodiscard]] constexpr Length operator""_m(long double v) noexcept {
    return Length{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Mass operator""_kg(long double v) noexcept {
    return Mass{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Time operator""_s(long double v) noexcept {
    return Time{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Current operator""_A(long double v) noexcept {
    return Current{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Temperature operator""_K(long double v) noexcept {
    return Temperature{detail::from_long_double(v)};
}

// -- Common conversions (units a classroom measurement really writes) ---------
[[nodiscard]] constexpr Length operator""_mm(long double v) noexcept {
    return Length{detail::from_long_double(v) / 1000.0};
}
[[nodiscard]] constexpr Length operator""_cm(long double v) noexcept {
    return Length{detail::from_long_double(v) / 100.0};
}
[[nodiscard]] constexpr Length operator""_km(long double v) noexcept {
    return Length{detail::from_long_double(v) * 1000.0};
}
[[nodiscard]] constexpr Mass operator""_g(long double v) noexcept {
    return Mass{detail::from_long_double(v) / 1000.0};
}
[[nodiscard]] constexpr Time operator""_ms(long double v) noexcept {
    return Time{detail::from_long_double(v) / 1000.0};
}
[[nodiscard]] constexpr Time operator""_us(long double v) noexcept {
    return Time{detail::from_long_double(v) / 1.0e6};
}
[[nodiscard]] constexpr Time operator""_min(long double v) noexcept {
    return Time{detail::from_long_double(v) * 60.0};
}

// -- Derived quantities -------------------------------------------------------
[[nodiscard]] constexpr Velocity operator""_m_s(long double v) noexcept {
    return Velocity{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Acceleration operator""_m_s2(long double v) noexcept {
    return Acceleration{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Frequency operator""_Hz(long double v) noexcept {
    return Frequency{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Force operator""_N(long double v) noexcept {
    return Force{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Energy operator""_J(long double v) noexcept {
    return Energy{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Power operator""_W(long double v) noexcept {
    return Power{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Pressure operator""_Pa(long double v) noexcept {
    return Pressure{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Density operator""_kg_m3(long double v) noexcept {
    return Density{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Momentum operator""_kg_m_s(long double v) noexcept {
    return Momentum{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Area operator""_m2(long double v) noexcept {
    return Area{detail::from_long_double(v)};
}
[[nodiscard]] constexpr Volume operator""_m3(long double v) noexcept {
    return Volume{detail::from_long_double(v)};
}

}  // namespace qp::units::literals
