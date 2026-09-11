/**
 * @file test_smoke.cpp
 * @brief Smoke test for core independence.
 *
 * Corresponds to standards/enforcement.md section 6: core must build standalone, without Qt and without any plugin.
 * This file is the minimal executable proof of that gate: it includes core headers only and uses no Qt type.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   core depends on no Qt / views / plugins
 * @errors      noexcept
 * @complexity  n/a
 * @nondet      none
 * @frozen      no
 * @tests       smoke.core_units_standalone
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <string>

TEST_CASE("smoke.core_units_standalone", "[smoke]") {
    using namespace qp::units;
    using namespace qp::units::literals;

    // Dimension derivation for a mechanics scenario: spring oscillator omega = sqrt(k/m)
    constexpr Mass m{0.5};
    constexpr auto k = 20.0_N / 1.0_m;
    STATIC_REQUIRE(k.dim == dims::force / dims::length);
    constexpr auto omega_sq = k / m;
    STATIC_REQUIRE(omega_sq.dim == dims::frequency * dims::frequency);
    REQUIRE(omega_sq.value() == 40.0);

    // Inclined plane: normal force N = m g cos(theta); the dimension must be a force
    constexpr auto g = 9.8_m_s2;
    constexpr auto fg = m * g;
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(fg)>, Force>);
    REQUIRE(fg.value() == 4.9);

    // The unit string can be generated from the type -- the same one C++ and YAML share
    REQUIRE(unit_symbol<dims::force>() == "kg*m/s^2");
    REQUIRE(unit_symbol(omega_sq.dim) == "1/s^2");
}
