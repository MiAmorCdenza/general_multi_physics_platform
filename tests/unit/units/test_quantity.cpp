/**
 * @file test_quantity.cpp
 * @brief units 模块 §Quantity 的单元与性质测试。
 *
 * 重点在**负向测试**：量纲错误必须在编译期被拒绝，而不是在运行期被发现。
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <limits>
#include <type_traits>

using namespace qp::units;
using namespace qp::units::literals;

// ── 编译期可加性探测（用于负向断言）─────────────────────────────────────────

template <class A, class B, class = void>
struct can_add : std::false_type {};
template <class A, class B>
struct can_add<A, B, std::void_t<decltype(std::declval<A>() + std::declval<B>())>>
    : std::true_type {};

template <class A, class B, class = void>
struct can_sub : std::false_type {};
template <class A, class B>
struct can_sub<A, B, std::void_t<decltype(std::declval<A>() - std::declval<B>())>>
    : std::true_type {};

template <class A, class B, class = void>
struct can_compare : std::false_type {};
template <class A, class B>
struct can_compare<A, B, std::void_t<decltype(std::declval<A>() < std::declval<B>())>>
    : std::true_type {};

// ── 构造与取值 ───────────────────────────────────────────────────────────────

TEST_CASE("units.quantity.construct", "[units]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<Length>);
    STATIC_REQUIRE(std::is_standard_layout_v<Length>);
    STATIC_REQUIRE(sizeof(Length) == sizeof(double));
    STATIC_REQUIRE(std::is_same_v<Length::value_type, double>);
    STATIC_REQUIRE(Length::dim == dims::length);

    constexpr Length a{2.5};
    STATIC_REQUIRE(a.value() == 2.5);
    constexpr Length zero;  // 默认构造为 0
    STATIC_REQUIRE(zero.value() == 0.0);
}

TEST_CASE("units.quantity.set_updates_value", "[units]") {
    Length a{3.0};
    a.set(7.5);
    REQUIRE(a.value() == 7.5);
    STATIC_REQUIRE(Length::dim == dims::length);  // 量纲不因 set 改变
    a.set(-1.0);
    REQUIRE(a.value() == -1.0);
}

TEST_CASE("units.quantity.value_roundtrip", "[units][property]") {
    constexpr double samples[] = {0.0, 1.0, -1.0, 3.14159265358979, 1e-12, 1e12, -2.5e-7};
    for (double v : samples) {
        REQUIRE(Length{v}.value() == v);
        REQUIRE(Mass{v}.value() == v);
        REQUIRE(Energy{v}.value() == v);
    }
    // Quantity 不提供隐式转换到标量 —— 这一步必须由人显式做
    STATIC_REQUIRE(std::is_convertible_v<Length, double> == false);
    STATIC_REQUIRE(std::is_constructible_v<Length, double> == true);
    STATIC_REQUIRE(std::is_convertible_v<double, Length> == false);  // explicit 构造
}

// ── 加减：仅同量纲 ───────────────────────────────────────────────────────────

TEST_CASE("units.quantity.add_same_dim", "[units]") {
    constexpr Length a{3.0};
    constexpr Length b{4.0};
    STATIC_REQUIRE((a + b).value() == 7.0);
    STATIC_REQUIRE((a + b).dim == dims::length);
    STATIC_REQUIRE(std::is_same_v<decltype(a + b), const Length>);
}

TEST_CASE("units.quantity.sub_same_dim", "[units]") {
    constexpr Length a{3.0};
    constexpr Length b{4.0};
    STATIC_REQUIRE((a - b).value() == -1.0);
    STATIC_REQUIRE((a - a).value() == 0.0);
}

TEST_CASE("units.quantity.no_cross_dim_add", "[units]") {
    // 正向：同量纲可加
    STATIC_REQUIRE(can_add<Length, Length>::value);
    STATIC_REQUIRE(can_add<Energy, Energy>::value);
    STATIC_REQUIRE(can_add<Force, Force>::value);
    // 负向：跨量纲必须编译失败
    STATIC_REQUIRE(can_add<Length, Mass>::value == false);
    STATIC_REQUIRE(can_add<Length, Time>::value == false);
    STATIC_REQUIRE(can_add<Force, Energy>::value == false);
    STATIC_REQUIRE(can_sub<Length, Mass>::value == false);
    STATIC_REQUIRE(can_sub<Velocity, Acceleration>::value == false);
    // 负向：量纲相同但语义不同的别名**可以**相加 —— 这是已知局限，不是 bug
    STATIC_REQUIRE(can_add<Energy, Torque>::value == true);
    STATIC_REQUIRE(can_add<Work, Energy>::value == true);
}

TEST_CASE("units.quantity.no_implicit_scalar", "[units]") {
    STATIC_REQUIRE(std::is_convertible_v<Length, double> == false);
    STATIC_REQUIRE(std::is_convertible_v<Dimensionless, double> == false);
    STATIC_REQUIRE(std::is_assignable_v<double&, Length> == false);
    // 但显式取数是允许的，且必须显式
    constexpr Length a{2.0};
    const double raw = a.value();
    REQUIRE(raw == 2.0);
}

// ── 乘除：量纲运算 ───────────────────────────────────────────────────────────

TEST_CASE("units.quantity.mul_dim_adds", "[units]") {
    constexpr Length d{2.0};
    constexpr Length e{3.0};
    constexpr auto area_q = d * e;
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(area_q)>, Area>);
    STATIC_REQUIRE(area_q.value() == 6.0);

    constexpr Time t{4.0};
    constexpr auto v = d / t;
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(v)>, Velocity>);
    STATIC_REQUIRE(v.value() == 0.5);

    constexpr Mass m{5.0};
    constexpr auto f = m * Acceleration{2.0};
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(f)>, Force>);
    STATIC_REQUIRE(f.value() == 10.0);

    constexpr auto e_work = f * Length{3.0};
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(e_work)>, Energy>);
    STATIC_REQUIRE(e_work.value() == 30.0);
}

TEST_CASE("units.quantity.div_dim_subtracts", "[units]") {
    constexpr Energy e{12.0};
    constexpr Time t{3.0};
    constexpr auto p = e / t;
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(p)>, Power>);
    STATIC_REQUIRE(p.value() == 4.0);
}

TEST_CASE("units.quantity.scalar_multiply", "[units]") {
    constexpr Length a{3.0};
    STATIC_REQUIRE((a * 2.0).value() == 6.0);
    STATIC_REQUIRE((2.0 * a).value() == 6.0);
    STATIC_REQUIRE((a / 2.0).value() == 1.5);
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(a * 2.0)>, Length>);
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(a / 2.0)>, Length>);
}

TEST_CASE("units.quantity.negate", "[units]") {
    constexpr Length a{3.0};
    STATIC_REQUIRE((-a).value() == -3.0);
    STATIC_REQUIRE((-(-a)).value() == 3.0);
    STATIC_REQUIRE((-a).dim == dims::length);
}

TEST_CASE("units.quantity.mul_commutative", "[units][property]") {
    constexpr double samples[] = {-2.0, -0.5, 0.0, 0.5, 2.0, 1e-6, 1e6};
    for (double x : samples) {
        for (double y : samples) {
            const auto ab = Length{x} * Mass{y};
            const auto ba = Mass{y} * Length{x};
            STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(ab)>, std::decay_t<decltype(ba)>);
            REQUIRE(ab.value() == ba.value());
            REQUIRE(ab.dim == ba.dim);
        }
    }
}

TEST_CASE("units.quantity.mul_associative", "[units][property]") {
    constexpr double samples[] = {-2.0, -0.5, 0.0, 0.5, 2.0};
    for (double x : samples) {
        for (double y : samples) {
            for (double z : samples) {
                const auto lhs = (Length{x} * Mass{y}) * Time{z};
                const auto rhs = Length{x} * (Mass{y} * Time{z});
                STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(lhs)>, std::decay_t<decltype(rhs)>);
                REQUIRE(lhs.value() == rhs.value());
            }
        }
    }
}

// ── 幂 ───────────────────────────────────────────────────────────────────────

TEST_CASE("units.quantity.pow_two", "[units]") {
    constexpr Length a{3.0};
    constexpr auto sq = pow<2>(a);
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(sq)>, Area>);
    STATIC_REQUIRE(sq.value() == 9.0);

    constexpr Time t{2.0};
    constexpr auto inv_sq = pow<-2>(t);
    STATIC_REQUIRE(inv_sq.dim == dims::frequency * dims::frequency);
    STATIC_REQUIRE(inv_sq.value() == 0.25);
}

TEST_CASE("units.quantity.pow_zero", "[units]") {
    constexpr Length a{3.0};
    constexpr auto one = pow<0>(a);
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(one)>, Dimensionless>);
    STATIC_REQUIRE(one.value() == 1.0);
    STATIC_REQUIRE(one.dim == Dim{});
}

TEST_CASE("units.quantity.pow_constexpr", "[units]") {
    // 整数幂必须是 constexpr，否则无法写出编译期常量表达式
    constexpr auto sq = pow<2>(Length{3.0});
    STATIC_REQUIRE(sq.value() == 9.0);
    constexpr auto cube = pow<3>(Length{2.0});
    STATIC_REQUIRE(cube.value() == 8.0);
    constexpr auto inv = pow<-1>(Time{4.0});
    STATIC_REQUIRE(inv.value() == 0.25);
    constexpr auto inv3 = pow<-3>(Time{2.0});
    STATIC_REQUIRE(inv3.value() == 0.125);
    // 求值顺序固定 → 同一编译器下可复现
    STATIC_REQUIRE(pow<2>(Length{1.1}).value() == 1.1 * 1.1);
}

TEST_CASE("units.quantity.sqrt_unchecked", "[units]") {
    constexpr auto a = pow<2>(Length{3.0});
    STATIC_REQUIRE(a.dim == dims::area);
    const auto root = sqrt_unchecked(a);
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(root)>, Area>);
    REQUIRE(root.value() == 3.0);
}

TEST_CASE("units.quantity.is_finite", "[units]") {
    REQUIRE(Length{1.0}.is_finite());
    REQUIRE(Length{0.0}.is_finite());
    REQUIRE(Length{-1e300}.is_finite());
    REQUIRE_FALSE(Length{std::numeric_limits<double>::infinity()}.is_finite());
    REQUIRE_FALSE(Length{std::numeric_limits<double>::quiet_NaN()}.is_finite());
}

// ── 比较 ─────────────────────────────────────────────────────────────────────

TEST_CASE("units.quantity.comparison", "[units]") {
    constexpr Length a{1.0};
    constexpr Length b{2.0};
    STATIC_REQUIRE(a == a);
    STATIC_REQUIRE(a != b);
    STATIC_REQUIRE(a < b);
    STATIC_REQUIRE(a <= b);
    STATIC_REQUIRE(b > a);
    STATIC_REQUIRE(b >= a);
    // 跨量纲不可比较
    STATIC_REQUIRE(can_compare<Length, Mass>::value == false);
    STATIC_REQUIRE(can_compare<Length, Length>::value == true);
}

TEST_CASE("units.quantity.energy_equivalence", "[units]") {
    // 端到端量纲推导：1/2 m v^2 的量纲必须是能量
    constexpr Mass m{2.0};
    constexpr Velocity v{3.0};
    constexpr auto ke = 0.5 * m * pow<2>(v);
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(ke)>, Energy>);
    REQUIRE(ke.value() == 9.0);

    // F = m a，W = F d，P = W / t
    constexpr auto f = m * Acceleration{2.0};
    constexpr auto w = f * Length{5.0};
    constexpr auto p = w / Time{2.0};
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(p)>, Power>);
    REQUIRE(p.value() == 10.0);

    // 弹簧振子：omega = sqrt(k/m)
    constexpr auto k = Force{8.0} / Length{2.0};  // N/m
    STATIC_REQUIRE(k.dim == dims::force / dims::length);
    constexpr auto omega_sq = k / m;
    STATIC_REQUIRE(omega_sq.dim == dims::frequency * dims::frequency);
    REQUIRE(omega_sq.value() == 2.0);
}
