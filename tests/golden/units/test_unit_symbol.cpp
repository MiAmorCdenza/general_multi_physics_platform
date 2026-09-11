/**
 * @file test_unit_symbol.cpp
 * @brief Golden tests for the units module, section unit_symbol.
 *
 * A **golden regression**: unit strings must match the hand-written expectation exactly.
 * Any drift means the "unit" in C++ / YAML / scripts has started to disagree.
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
    // base quantities
    REQUIRE(short_of(dims::length) == "m");
    REQUIRE(short_of(dims::mass) == "kg");
    REQUIRE(short_of(dims::time) == "s");
    REQUIRE(short_of(dims::current) == "A");
    REQUIRE(short_of(dims::temperature) == "K");
    REQUIRE(short_of(dims::amount) == "mol");
    REQUIRE(short_of(dims::luminous) == "cd");
    // geometry
    REQUIRE(short_of(dims::area) == "m^2");
    REQUIRE(short_of(dims::volume) == "m^3");
    REQUIRE(short_of(dims::reciprocal_length) == "1/m");
    // kinematics
    REQUIRE(short_of(dims::velocity) == "m/s");
    REQUIRE(short_of(dims::acceleration) == "m/s^2");
    REQUIRE(short_of(dims::jerk) == "m/s^3");
    // mechanics
    REQUIRE(short_of(dims::momentum) == "kg*m/s");
    REQUIRE(short_of(dims::force) == "kg*m/s^2");
    REQUIRE(short_of(dims::energy) == "kg*m^2/s^2");
    REQUIRE(short_of(dims::power) == "kg*m^2/s^3");
    REQUIRE(short_of(dims::pressure) == "kg/(m*s^2)");
    REQUIRE(short_of(dims::density) == "kg/m^3");
    REQUIRE(short_of(dims::moment_of_inertia) == "kg*m^2");
    // vibration
    REQUIRE(short_of(dims::frequency) == "1/s");
    // electromagnetics (denominator sorted by symbol letter: A before s)
    REQUIRE(short_of(dims::charge) == "A*s");
    REQUIRE(short_of(dims::magnetic_flux_density) == "kg/(A*s^2)");
    REQUIRE(short_of(dims::voltage) == "kg*m^2/(A*s^3)");
    REQUIRE(short_of(dims::resistance) == "kg*m^2/(A^2*s^3)");
    REQUIRE(short_of(dims::magnetic_flux) == "kg*m^2/(A*s^2)");
    REQUIRE(short_of(dims::inductance) == "kg*m^2/(A^2*s^2)");
}

TEST_CASE("units.symbol.denominator_parenthesized", "[units][golden]") {
    // A denominator with several factors must be parenthesized, else it is ambiguous.
    // Only **observable output** is asserted here, never detail:: internals: writing a
    // separate test for an internal helper tested the implementation, so it was reverted.
    REQUIRE(short_of(dims::mass) == "kg");              // 0 denominator factors
    REQUIRE(short_of(dims::velocity) == "m/s");         // 1 denominator factor
    REQUIRE(short_of(dims::density) == "kg/m^3");       // 1 denominator factor
    REQUIRE(short_of(dims::frequency) == "1/s");        // empty numerator -> "1"
    REQUIRE(short_of(dims::force) == "kg*m/s^2");       // 1 denominator factor -> no parentheses
    REQUIRE(short_of(dims::energy) == "kg*m^2/s^2");    // 1 denominator factor -> no parentheses
    REQUIRE(short_of(dims::pressure) == "kg/(m*s^2)");  // 2 denominator factors -> parentheses
    REQUIRE(short_of(dims::voltage) == "kg*m^2/(A*s^3)");
    REQUIRE(short_of(dims::resistance) == "kg*m^2/(A^2*s^3)");
    REQUIRE(short_of(dims::magnetic_flux_density) == "kg/(A*s^2)");
    REQUIRE(short_of(dims::inductance) == "kg*m^2/(A^2*s^2)");

    // Parentheses appear only when the denominator has several factors
    REQUIRE(short_of(dims::velocity).find('(') == std::string::npos);
    REQUIRE(short_of(dims::density).find('(') == std::string::npos);
    REQUIRE(short_of(dims::force).find('(') == std::string::npos);
    REQUIRE(short_of(dims::pressure).find('(') != std::string::npos);

    // Long-form rules match the short form
    REQUIRE(long_of(dims::pressure) == "kilogram per (meter*second^2)");
    REQUIRE(long_of(dims::voltage) == "kilogram*meter^2 per (ampere*second^3)");
    REQUIRE(long_of(dims::velocity) == "meter per second");
    REQUIRE(long_of(dims::force) == "kilogram*meter per second^2");
}

TEST_CASE("units.symbol.area_uses_caret", "[units][golden]") {
    // Exponent 1 must not produce "^1"
    REQUIRE(short_of(dims::length) == "m");
    REQUIRE(short_of(dims::length).find('^') == std::string::npos);
    REQUIRE(short_of(dims::momentum).find("^1") == std::string::npos);
    // Exponent >1 must produce "^n"
    REQUIRE(short_of(dims::area).find("^2") != std::string::npos);
    // Components with exponent 0 must not appear
    REQUIRE(short_of(dims::area).find("kg") == std::string::npos);
    REQUIRE(short_of(dims::area).find('s') == std::string::npos);
}

TEST_CASE("units.symbol.negative_exponent_uses_per", "[units][golden]") {
    REQUIRE(short_of(dims::velocity) == "m/s");
    REQUIRE(short_of(dims::velocity).find("^-") == std::string::npos);
    REQUIRE(short_of(dims::frequency) == "1/s");
    REQUIRE(short_of(dims::reciprocal_length) == "1/m");
    // Negative exponents may appear only on the denominator side
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
        // Same input -> same output, character for character
        REQUIRE(short_of(d) == short_of(d));
        REQUIRE(long_of(d) == long_of(d));
        // non-empty
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
