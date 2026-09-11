/**
 * @file test_conversion.cpp
 * @brief 单位换算、字面量、Unit 的测试。
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <cmath>

using namespace qp::units;
using namespace qp::units::literals;

TEST_CASE("units.literals.base_units", "[units]") {
    STATIC_REQUIRE(decltype(5.0_m)::dim == dims::length);
    STATIC_REQUIRE(decltype(5.0_kg)::dim == dims::mass);
    STATIC_REQUIRE(decltype(5.0_s)::dim == dims::time);
    STATIC_REQUIRE(decltype(5.0_A)::dim == dims::current);
    STATIC_REQUIRE(decltype(5.0_K)::dim == dims::temperature);

    STATIC_REQUIRE((5.0_m).value() == 5.0);
    STATIC_REQUIRE((2.0_kg).value() == 2.0);
    STATIC_REQUIRE((3.0_s).value() == 3.0);
}

TEST_CASE("units.literals.engineering_prefixes", "[units]") {
    STATIC_REQUIRE((250.0_mm).value() == 0.25);
    STATIC_REQUIRE((250.0_cm).value() == 2.5);
    STATIC_REQUIRE((2.5_km).value() == 2500.0);
    STATIC_REQUIRE((500.0_g).value() == 0.5);
    STATIC_REQUIRE((250.0_ms).value() == 0.25);
    STATIC_REQUIRE((90.0_min).value() == 5400.0);

    // 前缀换算必须精确到浮点可表示范围
    REQUIRE((1.0_cm).value() == 0.01);
    REQUIRE((1.0_mm).value() == 0.001);
}

TEST_CASE("units.literals.derived_units", "[units]") {
    STATIC_REQUIRE(decltype(9.8_m_s2)::dim == dims::acceleration);
    STATIC_REQUIRE(decltype(50.0_Hz)::dim == dims::frequency);
    STATIC_REQUIRE(decltype(10.0_N)::dim == dims::force);
    STATIC_REQUIRE(decltype(10.0_J)::dim == dims::energy);
    STATIC_REQUIRE(decltype(10.0_W)::dim == dims::power);
    STATIC_REQUIRE(decltype(10.0_Pa)::dim == dims::pressure);
    STATIC_REQUIRE(decltype(10.0_kg_m3)::dim == dims::density);

    // 字面量与类型系统的衔接：字面量只是构造糖，类型检查照常生效
    constexpr auto f = 2.0_kg * 9.8_m_s2;
    STATIC_REQUIRE(std::is_same_v<std::decay_t<decltype(f)>, Force>);
    REQUIRE(f.value() == 19.6);
}

TEST_CASE("units.unit.convert_to_si", "[units]") {
    const Unit cm{dims::length, 0.01, "cm"};
    REQUIRE(cm.to_si(250.0) == 2.5);
    REQUIRE(cm.from_si(2.5) == 250.0);

    const Unit km{dims::length, 1000.0, "km"};
    REQUIRE(km.to_si(1.5) == 1500.0);

    const Unit identity{dims::length, 1.0, "m"};
    REQUIRE(identity.to_si(7.0) == 7.0);
}

TEST_CASE("units.unit.roundtrip", "[units][property]") {
    const Unit units[] = {{dims::length, 0.001, "mm"},
                          {dims::length, 0.01, "cm"},
                          {dims::length, 1.0, "m"},
                          {dims::length, 1000.0, "km"},
                          {dims::mass, 0.001, "g"},
                          {dims::mass, 1.0, "kg"},
                          {dims::time, 0.001, "ms"},
                          {dims::time, 1.0, "s"}};
    const double samples[] = {0.0, 1.0, -1.0, 250.0, 1e-6, 1e6};
    for (const Unit& u : units) {
        for (double v : samples) {
            REQUIRE(u.from_si(u.to_si(v)) == v);
        }
    }
}
