/**
 * @file test_unit_symbol.cpp
 * @brief units 模块 §unit_symbol 的黄金测试。
 *
 * 这一组是**黄金回归**：单位字符串必须与手写期望值逐字相同。
 * 任何一处漂移都意味着 C++ / YAML / 脚本三处的"单位"开始不一致。
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <string>

using namespace qp::units;

static std::string short_of(Dim d) { return unit_symbol(d, SymbolStyle::short_form); }
static std::string long_of(Dim d) { return unit_symbol(d, SymbolStyle::long_form); }

TEST_CASE("units.symbol.dimensionless_is_one", "[units][golden]") {
    REQUIRE(short_of(Dim{}) == "1");
    REQUIRE(short_of(dims::frequency * dims::time) == "1");
    REQUIRE(short_of(dims::length / dims::length) == "1");
    REQUIRE(long_of(Dim{}) == "1");
}

TEST_CASE("units.symbol.short_forms", "[units][golden]") {
    // 基本量
    REQUIRE(short_of(dims::length) == "m");
    REQUIRE(short_of(dims::mass) == "kg");
    REQUIRE(short_of(dims::time) == "s");
    REQUIRE(short_of(dims::current) == "A");
    REQUIRE(short_of(dims::temperature) == "K");
    REQUIRE(short_of(dims::amount) == "mol");
    REQUIRE(short_of(dims::luminous) == "cd");
    // 几何
    REQUIRE(short_of(dims::area) == "m^2");
    REQUIRE(short_of(dims::volume) == "m^3");
    REQUIRE(short_of(dims::reciprocal_length) == "1/m");
    // 运动学
    REQUIRE(short_of(dims::velocity) == "m/s");
    REQUIRE(short_of(dims::acceleration) == "m/s^2");
    REQUIRE(short_of(dims::jerk) == "m/s^3");
    // 力学
    REQUIRE(short_of(dims::momentum) == "kg*m/s");
    REQUIRE(short_of(dims::force) == "kg*m/s^2");
    REQUIRE(short_of(dims::energy) == "kg*m^2/s^2");
    REQUIRE(short_of(dims::power) == "kg*m^2/s^3");
    REQUIRE(short_of(dims::pressure) == "kg/(m*s^2)");
    REQUIRE(short_of(dims::density) == "kg/m^3");
    REQUIRE(short_of(dims::moment_of_inertia) == "kg*m^2");
    // 振动
    REQUIRE(short_of(dims::frequency) == "1/s");
    // 电磁（分母按符号字母序：A 在 s 之前）
    REQUIRE(short_of(dims::charge) == "A*s");
    REQUIRE(short_of(dims::magnetic_flux_density) == "kg/(A*s^2)");
    REQUIRE(short_of(dims::voltage) == "kg*m^2/(A*s^3)");
    REQUIRE(short_of(dims::resistance) == "kg*m^2/(A^2*s^3)");
    REQUIRE(short_of(dims::magnetic_flux) == "kg*m^2/(A*s^2)");
    REQUIRE(short_of(dims::inductance) == "kg*m^2/(A^2*s^2)");
}

TEST_CASE("units.symbol.denominator_parenthesized", "[units][golden]") {
    // 分母含多个因子必须加括号，否则语义有歧义。
    // 这里只断言**可观察输出**，不碰 detail:: 下的实现细节——
    // 曾经为内部辅助函数单独写测试，那是测试实现而非行为，已改回。
    REQUIRE(short_of(dims::mass) == "kg");              // 分母 0 个因子
    REQUIRE(short_of(dims::velocity) == "m/s");         // 分母 1 个
    REQUIRE(short_of(dims::density) == "kg/m^3");       // 分母 1 个
    REQUIRE(short_of(dims::frequency) == "1/s");        // 分子空 → "1"
    REQUIRE(short_of(dims::force) == "kg*m/s^2");       // 分母 1 个 → 无括号
    REQUIRE(short_of(dims::energy) == "kg*m^2/s^2");    // 分母 1 个 → 无括号
    REQUIRE(short_of(dims::pressure) == "kg/(m*s^2)");  // 分母 2 个 → 有括号
    REQUIRE(short_of(dims::voltage) == "kg*m^2/(A*s^3)");
    REQUIRE(short_of(dims::resistance) == "kg*m^2/(A^2*s^3)");
    REQUIRE(short_of(dims::magnetic_flux_density) == "kg/(A*s^2)");
    REQUIRE(short_of(dims::inductance) == "kg*m^2/(A^2*s^2)");

    // 括号只在分母多因子时出现
    REQUIRE(short_of(dims::velocity).find('(') == std::string::npos);
    REQUIRE(short_of(dims::density).find('(') == std::string::npos);
    REQUIRE(short_of(dims::force).find('(') == std::string::npos);
    REQUIRE(short_of(dims::pressure).find('(') != std::string::npos);

    // 长式规则与短式一致
    REQUIRE(long_of(dims::pressure) == "kilogram per (meter*second^2)");
    REQUIRE(long_of(dims::voltage) == "kilogram*meter^2 per (ampere*second^3)");
    REQUIRE(long_of(dims::velocity) == "meter per second");
    REQUIRE(long_of(dims::force) == "kilogram*meter per second^2");
}

TEST_CASE("units.symbol.area_uses_caret", "[units][golden]") {
    // 指数 1 不得出现 "^1"
    REQUIRE(short_of(dims::length) == "m");
    REQUIRE(short_of(dims::length).find('^') == std::string::npos);
    REQUIRE(short_of(dims::momentum).find("^1") == std::string::npos);
    // 指数 >1 必须出现 "^n"
    REQUIRE(short_of(dims::area).find("^2") != std::string::npos);
    // 指数 0 的分量不得出现
    REQUIRE(short_of(dims::area).find("kg") == std::string::npos);
    REQUIRE(short_of(dims::area).find('s') == std::string::npos);
}

TEST_CASE("units.symbol.negative_exponent_uses_per", "[units][golden]") {
    REQUIRE(short_of(dims::velocity) == "m/s");
    REQUIRE(short_of(dims::velocity).find("^-") == std::string::npos);
    REQUIRE(short_of(dims::frequency) == "1/s");
    REQUIRE(short_of(dims::reciprocal_length) == "1/m");
    // 负指数只允许出现在分母侧
    REQUIRE(short_of(dims::energy) == "kg*m^2/s^2");
}

TEST_CASE("units.symbol.long_form", "[units][golden]") {
    REQUIRE(long_of(dims::length) == "meter");
    REQUIRE(long_of(dims::mass) == "kilogram");
    REQUIRE(long_of(dims::time) == "second");
    REQUIRE(long_of(dims::current) == "ampere");
    REQUIRE(long_of(dims::temperature) == "kelvin");
    REQUIRE(long_of(dims::amount) == "mole");
    REQUIRE(long_of(dims::luminous) == "candela");
    REQUIRE(long_of(dims::velocity) == "meter per second");
    REQUIRE(long_of(dims::force) == "kilogram*meter per second^2");
    REQUIRE(long_of(dims::energy) == "kilogram*meter^2 per second^2");
    REQUIRE(long_of(dims::pressure) == "kilogram per (meter*second^2)");
    REQUIRE(long_of(dims::voltage) == "kilogram*meter^2 per (ampere*second^3)");
}

TEST_CASE("units.symbol.deterministic", "[units][property]") {
    constexpr Dim samples[] = {Dim{},          dims::length,   dims::mass,     dims::time,
                               dims::area,     dims::velocity, dims::force,    dims::energy,
                               dims::power,    dims::pressure, dims::density,  dims::frequency,
                               dims::charge,   dims::voltage,  dims::resistance,
                               dims::magnetic_flux_density,    Dim{3, -2, 1, 0, 0, 0, 0}};
    for (Dim d : samples) {
        // 同输入 → 同输出，逐字相同
        REQUIRE(short_of(d) == short_of(d));
        REQUIRE(long_of(d) == long_of(d));
        // 非空
        REQUIRE_FALSE(short_of(d).empty());
        REQUIRE_FALSE(long_of(d).empty());
    }
}

TEST_CASE("units.symbol.compile_time_matches_runtime", "[units]") {
    REQUIRE(unit_symbol<dims::length>() == short_of(dims::length));
    REQUIRE(unit_symbol<dims::energy>() == short_of(dims::energy));
    REQUIRE(unit_symbol<Dim{}>() == "1");
}

TEST_CASE("units.dim_axes.diagnostic_format", "[units]") {
    REQUIRE(dim_axes(Dim{}) == "0,0,0,0,0,0,0");
    REQUIRE(dim_axes(dims::force) == "1,1,-2,0,0,0,0");
    REQUIRE(dim_axes(dims::energy) == "2,1,-2,0,0,0,0");
    REQUIRE(dim_axes(dims::magnetic_flux_density) == "0,1,-2,-1,0,0,0");
}
