/**
 * @file test_smoke.cpp
 * @brief core 独立性冒烟测试。
 *
 * 对应 standards/enforcement.md §6：core 必须能脱离 Qt、脱离所有插件独立构建。
 * 本文件是那一门禁的最小可执行证明：只包含 core 头文件，不使用任何 Qt 类型。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   core 不依赖 Qt / views / plugins
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      否
 * @tests       smoke.core_units_standalone
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <string>

TEST_CASE("smoke.core_units_standalone", "[smoke]") {
    using namespace qp::units;
    using namespace qp::units::literals;

    // 力学场景的量纲推导：弹簧振子 omega = sqrt(k/m)
    constexpr Mass m{0.5};
    constexpr auto k = 20.0_N / 1.0_m;
    STATIC_REQUIRE(k.dim == dims::force / dims::length);
    constexpr auto omega_sq = k / m;
    STATIC_REQUIRE(omega_sq.dim == dims::frequency * dims::frequency);
    REQUIRE(omega_sq.value() == 40.0);

    // 斜面：法向力 N = m g cos(theta)，量纲必须是力
    constexpr auto g = 9.8_m_s2;
    constexpr auto fg = m * g;
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(fg)>, Force>);
    REQUIRE(fg.value() == 4.9);

    // 单位字符串可从类型自动生成 —— C++ 与 YAML 共用的那份
    REQUIRE(unit_symbol<dims::force>() == "kg*m/s^2");
    REQUIRE(unit_symbol(omega_sq.dim) == "1/s^2");
}
