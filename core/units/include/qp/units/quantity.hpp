/**
 * @file quantity.hpp
 * @brief 带量纲的数值：编译期强制量纲正确的算术。
 *
 * 设计要点（对应章程 §4.2 与 ADR-0001）：
 *   - 量纲是**类型的一部分**，不是运行时字段。写错量纲 = 编译失败。
 *   - 不提供到标量的隐式转换。想拿裸数值必须显式 `.value()`——
 *     这一步就是"我在放弃量纲保护"的确认动作。
 *
 * @frozen 本文件的类模板形状冻结；新增成员函数属于兼容扩展。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   同量纲可运算、跨量纲不可运算（编译期强制）
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是（`Quantity` 的类模板形状与 `value()` 语义）
 * @tests       units.quantity.energy_equivalence
 */
#pragma once

#include <qp/units/dim.hpp>

#include <cmath>
#include <type_traits>

namespace qp::units {

/**
 * @brief 一个带量纲的标量。
 *
 * @tparam D 量纲。数值语义为"以 SI 基本单位表示的值"。
 *
 * @ownership   pure（值类型，可平凡复制）
 * @thread      any
 * @pre         none
 * @post        value() 返回构造时的裸数值，不做任何换算
 * @invariant   同类型可加；不同类型不可加（编译期拒绝）
 * @errors      noexcept；无失败模式
 * @complexity  O(1)
 * @nondet      none
 * @frozen      是——类模板形状与 value() 语义冻结
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
     * @brief 默认构造为 0。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        value() == 0.0
     * @invariant   0 是唯一与量纲无关的数值
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     */
    constexpr Quantity() noexcept = default;

    /**
     * @brief 从裸数值构造。数值以 SI 基本单位解释。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        value() == v
     * @invariant   不提供隐式转换：`Length x = 3.0;` 必须编译失败
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      是（explicit 这一性质冻结）
     */
    explicit constexpr Quantity(double v) noexcept : v_(v) {}

    /**
     * @brief 就地写入裸数值（复用对象，避免分配）。
     *
     * @ownership   pure（修改自身）
     * @thread      any
     * @pre         none
     * @post        value() == v
     * @invariant   量纲不变
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       units.quantity.set_updates_value
     */
    constexpr void set(double v) noexcept { v_ = v; }

    /**
     * @brief 取裸数值。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        返回值等于最近一次构造或 set 的实参
     * @invariant   value(Quantity(x)) == x（对任意有限 x）
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      是
     * @tests       units.quantity.value_roundtrip
     */
    [[nodiscard]] constexpr double value() const noexcept { return v_; }

    /**
     * @brief 是否为有限值。数值内核在钳制前用它判定（章程 C2）。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        true 当且仅当 value() 既非 inf 也非 NaN
     * @invariant   与 std::isfinite(value()) 恒等
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       units.quantity.is_finite
     */
    [[nodiscard]] bool is_finite() const noexcept { return std::isfinite(v_); }

private:
    double v_ = 0.0;
};

// ── 加减：仅同量纲 ───────────────────────────────────────────────────────────

/**
 * @brief 同量纲相加。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回 Quantity<D>，数值为两者之和
 * @invariant   交换律；结合律；(a + b) - b == a（浮点容差外精确成立仅当无舍入）
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.quantity.add_same_dim
 */
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator+(Quantity<D> a, Quantity<D> b) noexcept {
    return Quantity<D>{a.value() + b.value()};
}

/**
 * @brief 同量纲相减。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回 Quantity<D>，数值为两者之差
 * @invariant   a - a 的数值为 0.0
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.quantity.sub_same_dim
 */
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator-(Quantity<D> a, Quantity<D> b) noexcept {
    return Quantity<D>{a.value() - b.value()};
}

/**
 * @brief 取负。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        数值取反，量纲不变
 * @invariant   -(-a) == a
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.quantity.negate
 */
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator-(Quantity<D> a) noexcept {
    return Quantity<D>{-a.value()};
}

// ── 乘除：量纲运算 ───────────────────────────────────────────────────────────

/**
 * @brief 相乘：数值相乘，量纲相加。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        结果量纲为 L + R；数值为精确乘积
 * @invariant   交换律、结合律
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.quantity.mul_dim_adds, units.quantity.mul_commutative,
 *              units.quantity.mul_associative
 */
template <auto L, auto R>
[[nodiscard]] constexpr Quantity<L + R> operator*(Quantity<L> a, Quantity<R> b) noexcept {
    return Quantity<L + R>{a.value() * b.value()};
}

/**
 * @brief 相除：数值相除，量纲相减。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        结果量纲为 L / R
 * @invariant   (a / b) * b 的量纲与 a 相同
 * @errors      noexcept；b 为零时按 IEEE 754 产生 inf/nan，不抛异常
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.quantity.div_dim_subtracts
 */
template <auto L, auto R>
[[nodiscard]] constexpr Quantity<L / R> operator/(Quantity<L> a, Quantity<R> b) noexcept {
    return Quantity<L / R>{a.value() / b.value()};
}

/// @brief 与无量纲标量相乘。标量不改变量纲。
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator*(Quantity<D> a, double s) noexcept {
    return Quantity<D>{a.value() * s};
}
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator*(double s, Quantity<D> a) noexcept {
    return Quantity<D>{s * a.value()};
}
/// @brief 与无量纲标量相除。
template <auto D>
[[nodiscard]] constexpr Quantity<D> operator/(Quantity<D> a, double s) noexcept {
    return Quantity<D>{a.value() / s};
}

// ── 幂：量纲按整数次幂缩放 ───────────────────────────────────────────────────

namespace detail {

/// @brief 整数次幂，constexpr 友好（平方求幂，不用 std::pow）。
///
/// 与 std::pow 的差异：整数幂用重复乘法，**不引入 libm 的精度损失**，
/// 且可出现在常量表达式中。浮点结合律不成立，因此这里固定求值顺序，
/// 保证同一编译器下 pow<N>(x) 可复现（章程 R2 的精神）。
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
 * @brief 整数次幂。
 *
 * @ownership   pure
 * @thread      any
 * @pre         P 为编译期整数
 * @post        量纲为 D 的 P 次幂；数值为 a 的 P 次幂
 * @invariant   pow<1>(a) == a；pow<0>(a) 为无量纲 1；pow<2>(a) 量纲为 D*D
 * @errors      noexcept；P<0 且 a==0 时按 IEEE 754 产生 inf，不抛异常
 * @complexity  O(log|P|)
 * @nondet      none
 * @frozen      否
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
 * @brief 平方根：量纲指数减半。
 *
 * 注意：只有当所有指数均为偶数时量纲才合法。本函数不做检查——
 * 调用点必须用 constexpr 断言保证。见 spec.md 待决项。
 *
 * @ownership   pure
 * @thread      any
 * @pre         D 的所有指数均为偶数
 * @post        量纲指数为 D 的一半；数值为 sqrt(a)
 * @invariant   sqrt_unchecked(pow<2>(x)) == x（浮点容差内）
 * @errors      noexcept；负数为 NaN，不抛异常
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.quantity.sqrt_unchecked
 */
template <auto D>
[[nodiscard]] constexpr Quantity<dim_pow<1>(D)> sqrt_unchecked(Quantity<D> a) noexcept {
    return Quantity<D>{std::sqrt(a.value())};
}

// ── 比较：仅同量纲 ───────────────────────────────────────────────────────────

/**
 * @brief 同量纲比较。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        按数值大小返回布尔
 * @invariant   全序（数值为 NaN 时除外，遵循 IEEE 754）
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
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
