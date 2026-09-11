/**
 * @file literals.hpp
 * @brief 用户自定义字面量：`5.0_m`、`9.8_m_s2`、`250.0_mm` 等。
 *
 * 定位说明（重要）：
 *   字面量只是**语法糖**，它绑定在字面量上——变量、表达式结果都用不上它。
 *   真正防止量纲错误的是 `Quantity<D>` 的类型系统本身。
 *   本文件的作用是让"写常量"这件事顺口，不是安全机制。
 *
 * 换算约定：
 *   - C++ 规定浮点字面量运算符只能接 `long double`。
 *   - 所有换算都在 `long double` 下完成，**最后一步**用 `from_long_double()`
 *     显式收窄为 double。这是全文件唯一的收窄点，集中且可审计。
 *   - 换算因子一律写作 double，避免 `1000.0L` 这种混精度的隐式提升。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   换算结果与 SI 基本单位一致（250.0_mm 的数值为 0.25）
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      否（字面量集合可扩）
 * @tests       units.literals.base_units, units.literals.engineering_prefixes,
 *              units.literals.derived_units
 */
#pragma once

#include <qp/units/dimensions.hpp>

namespace qp::units::literals {

namespace detail {

/// @brief 全文件唯一的 long double → double 收窄点。
[[nodiscard]] constexpr double from_long_double(long double v) noexcept {
    return static_cast<double>(v);
}

}  // namespace detail

// ── 基本量 ───────────────────────────────────────────────────────────────────
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

// ── 常用换算（课堂测量里真实会写的单位） ─────────────────────────────────────
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

// ── 复合量 ───────────────────────────────────────────────────────────────────
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
