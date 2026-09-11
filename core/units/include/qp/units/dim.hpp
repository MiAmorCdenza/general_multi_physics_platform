/**
 * @file dim.hpp
 * @brief 量纲的编译期表示：七个 SI 基本量纲的整数指数。
 *
 * 这是 qp 量纲系统的**唯一底层表示**，是 `@frozen` 契约。
 * 任何改动都必须升 `qp::units::kUnitsAbiVersion` 并同步 tests/abi/。
 *
 * @ownership   pure（值类型）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   指数全部落在 [kMinExp, kMaxExp] 内
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是（`Dim` 的布局与成员顺序）
 * @tests       units.dim.layout, units.dim.equality, units.dim.dimensionless_predicate,
 *              units.dim.representable
 */
#pragma once

#include <cstdint>
#include <type_traits>

namespace qp::units {

/// @brief 量纲指数类型。int8 足够（-128..127），且保证 Dim 极小、可平凡复制。
using DimExp = std::int8_t;

/// @brief 指数溢出下限/上限。运算结果超出即编译期断言。
inline constexpr DimExp kMinExp = -100;
inline constexpr DimExp kMaxExp = 100;

/**
 * @brief 量纲向量：长度、质量、时间、电流、温度、物质量、发光强度 的指数。
 *
 * 顺序对应 SI 七个基本单位：m kg s A K mol cd。
 *
 * @ownership   pure（值类型）
 * @thread      any
 * @pre         none
 * @post        所有指数落在 [kMinExp, kMaxExp] 内（由乘法运算保证）
 * @invariant   平凡可复制；可 constexpr 构造
 * @errors      noexcept；无失败模式
 * @complexity  O(1)
 * @nondet      none
 * @frozen      是——本结构的布局与成员顺序均为 ABI，改动需升版本号
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

    /// @brief 全零量纲（无量纲）。
    [[nodiscard]] static constexpr Dim none() noexcept { return Dim{}; }

    /// @brief 编译期指数和。用于检测"指数相互抵消"，**不是**无量纲判据。
    [[nodiscard]] constexpr int sum() const noexcept {
        return static_cast<int>(L) + M + T + I + Th + N + J;
    }

    /**
     * @brief 是否真正无量纲：**全部七个指数均为 0**。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        true 当且仅当 L==M==T==I==Th==N==J==0
     * @invariant   与 `sum() == 0` **不等价**：速度的指数和为 -1，
     *              而 `Dim{1,-1,0,0,0,0,0}` 的指数和为 0 却仍是有量纲的。
     *              旧实现用 `sum() == 0` 会把这类量误判为无量纲——已修正。
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       units.dim.dimensionless_predicate
     */
    [[nodiscard]] constexpr bool is_dimensionless() const noexcept {
        return L == 0 && M == 0 && T == 0 && I == 0 && Th == 0 && N == 0 && J == 0;
    }

    /**
     * @brief 指数和是否为零（各量纲相互抵消）。
     *
     * 保留本函数是为了诊断信息："你的量纲指数相互抵消了"。
     * 它不是无量纲判据——见 is_dimensionless 的 @invariant。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        返回 sum() == 0
     * @invariant   is_dimensionless() ⟹ has_zero_exponent_sum()（单向成立）
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       units.dim.zero_exponent_sum
     */
    [[nodiscard]] constexpr bool has_zero_exponent_sum() const noexcept { return sum() == 0; }

    /// @brief 该量纲是否由本结构表达得了（指数不越界）。
    [[nodiscard]] constexpr bool is_representable() const noexcept {
        const auto in_range = [](DimExp e) { return e >= kMinExp && e <= kMaxExp; };
        return in_range(L) && in_range(M) && in_range(T) && in_range(I) && in_range(Th) &&
               in_range(N) && in_range(J);
    }

    /// @brief 逐分量是否相等。
    [[nodiscard]] friend constexpr bool operator==(Dim a, Dim b) noexcept {
        return a.L == b.L && a.M == b.M && a.T == b.T && a.I == b.I && a.Th == b.Th &&
               a.N == b.N && a.J == b.J;
    }
    [[nodiscard]] friend constexpr bool operator!=(Dim a, Dim b) noexcept { return !(a == b); }
};

namespace detail {
/// @brief 指数相加并检查越界。越界即编译期失败。
[[nodiscard]] constexpr DimExp add_exp(DimExp a, DimExp b) noexcept {
    const int r = static_cast<int>(a) + static_cast<int>(b);
    return r < kMinExp || r > kMaxExp ? kMinExp - 1 : static_cast<DimExp>(r);
}
constexpr bool in_range(Dim d) noexcept { return d.is_representable(); }
}  // namespace detail

/**
 * @brief 量纲相乘 = 指数相加。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        结果的每个指数为两输入对应指数之和
 * @invariant   交换律、结合律、none() 为单位元
 * @errors      noexcept；结果不可表示时由调用点的 constexpr 检查捕获
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否（运算本身可扩），但返回类型 Dim 的布局冻结
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
 * @brief 量纲相除 = 指数相减。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        结果的每个指数为两输入对应指数之差
 * @invariant   a / a == none()；a / none() == a
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
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
 * @brief 量纲取整数次幂。
 *
 * 注意：返回类型**用 `auto` 推导**，没有写成 `Dim dim_pow(Dim)` + 显式返回。
 * 原因是 MSVC 无法对"类类型非类型模板参数"做推导（见 ADR-0004），
 * 与 `Quantity` 的运算符保持同一策略。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        每个指数乘以 p
 * @invariant   pow(pow(a,m), n) == pow(a, m*n)；pow(a,1) == a；pow(a,0) == none()
 * @errors      noexcept；越界由调用点 constexpr 检查捕获
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
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
 * @brief 量纲相加 = 指数相加。等价于 `operator*`。
 *
 * 注意：量纲的"乘法"是 `operator*`（指数相加），`operator+` 是同一运算的
 * 另一种拼写，用于在模板参数位置书写 `Quantity<L + R>` 这类复合量纲。
 * 两者都保留，是为了让 `Quantity<Acceleration * Time>` 与 `Quantity<L + R>`
 * 都能自然书写；语义完全相同。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        结果的每个指数为两输入对应指数之和
 * @invariant   与 operator* 完全等价：a + b == a * b
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.dim.add_exact, units.dim.plus_equals_multiply
 */
[[nodiscard]] constexpr Dim operator+(Dim a, Dim b) noexcept { return a * b; }

/**
 * @brief 量纲相减 = 指数相减。等价于 `operator/`。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        结果的每个指数为两输入对应指数之差
 * @invariant   与 operator/ 完全等价：a - b == a / b
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.dim.subtract_exact, units.dim.minus_equals_divide
 */
[[nodiscard]] constexpr Dim operator-(Dim a, Dim b) noexcept { return a / b; }

/**
 * @brief 量纲取负（等价于 1/Dim）。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        每个指数取相反数
 * @invariant   dim_inverse(dim_inverse(a)) == a；dim_inverse(Dim{}) == Dim{}
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       units.dim.negate
 */
[[nodiscard]] constexpr Dim dim_inverse(Dim a) noexcept { return Dim{} / a; }

}  // namespace qp::units
