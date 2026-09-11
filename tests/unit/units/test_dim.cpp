/**
 * @file test_dim.cpp
 * @brief Unit and property tests for §Dim of the units module.
 *
 * The case ids correspond verbatim to the @tests fields in core/units/include/qp/units/dim.hpp.
 * They are checked mechanically by scripts/check_contracts.py (see standards/enforcement.md §5).
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <cstdint>
#include <type_traits>

using namespace qp::units;

// -- Static layout contract (corresponds to @frozen) -----------------------

TEST_CASE("units.dim.layout", "[units][abi]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<Dim>);
    STATIC_REQUIRE(std::is_standard_layout_v<Dim>);
    STATIC_REQUIRE(sizeof(Dim) == 7);
    STATIC_REQUIRE(alignof(Dim) == 1);
    STATIC_REQUIRE(sizeof(DimExp) == 1);
    STATIC_REQUIRE(kUnitsAbiVersion == 1);
    // The member order is ABI: m kg s A K mol cd
    STATIC_REQUIRE(std::is_same_v<decltype(Dim::L), DimExp>);
}

// -- Predicates ---------------------------------------------------------------

TEST_CASE("units.dim.equality", "[units]") {
    STATIC_REQUIRE(Dim{1, 0, 0, 0, 0, 0, 0} == Dim{1, 0, 0, 0, 0, 0, 0});
    STATIC_REQUIRE(Dim{1, 0, 0, 0, 0, 0, 0} != Dim{0, 1, 0, 0, 0, 0, 0});
    STATIC_REQUIRE(Dim{} == Dim::none());
    STATIC_REQUIRE(dims::length != dims::mass);
    STATIC_REQUIRE(dims::energy == dims::torque);        // same dimension, different meaning
    STATIC_REQUIRE(dims::frequency == dims::angular_velocity);  // rad is treated as dimensionless
}

TEST_CASE("units.dim.dimensionless_predicate", "[units]") {
    // Truly dimensionless: every exponent is 0
    STATIC_REQUIRE(Dim{}.is_dimensionless());
    STATIC_REQUIRE(Dim::none().is_dimensionless());
    STATIC_REQUIRE(dims::length.is_dimensionless() == false);
    STATIC_REQUIRE(dims::area.is_dimensionless() == false);
    // Exponents cancelling out is **not** dimensionless: L^1 * T^-1 is dimensional (velocity)
    STATIC_REQUIRE((Dim{1, 0, -1, 0, 0, 0, 0}.is_dimensionless()) == false);
    STATIC_REQUIRE((Dim{1, -1, 0, 0, 0, 0, 0}.is_dimensionless()) == false);
    STATIC_REQUIRE((Dim{-1, 1, 0, 0, 0, 0, 0}.is_dimensionless()) == false);
}

TEST_CASE("units.dim.zero_exponent_sum", "[units]") {
    STATIC_REQUIRE(Dim{}.has_zero_exponent_sum());
    // Velocity: L=1, T=-1 -> sum 0
    STATIC_REQUIRE((Dim{1, 0, -1, 0, 0, 0, 0}.has_zero_exponent_sum()));
    // L*M/T^2: 1+1-2 = 0 -> also judged a zero sum, but this is obviously force, which is dimensional
    STATIC_REQUIRE((Dim{1, 1, -2, 0, 0, 0, 0}.has_zero_exponent_sum()));
    STATIC_REQUIRE(dims::force.has_zero_exponent_sum());
    // One-way implication: truly dimensionless => zero sum
    STATIC_REQUIRE(Dim{}.is_dimensionless());
    STATIC_REQUIRE(Dim{}.has_zero_exponent_sum());
    // Counterexample: a zero sum that is not dimensionless -- the two are not interchangeable
    STATIC_REQUIRE(dims::force.has_zero_exponent_sum());
    STATIC_REQUIRE(dims::force.is_dimensionless() == false);
}

TEST_CASE("units.dim.representable", "[units]") {
    STATIC_REQUIRE(Dim{}.is_representable());
    STATIC_REQUIRE((Dim{kMaxExp, kMinExp, 0, 0, 0, 0, 0}.is_representable()));
    STATIC_REQUIRE((Dim{static_cast<DimExp>(kMaxExp + 1), 0, 0, 0, 0, 0, 0}.is_representable()) ==
                   false);
}

// -- Add and subtract: exact per component ------------------------------------

TEST_CASE("units.dim.add_exact", "[units]") {
    // Dim's operator+ and operator* are two spellings of the same operation (adding exponents).
    STATIC_REQUIRE(dims::length + dims::length == Dim{2, 0, 0, 0, 0, 0, 0});
    STATIC_REQUIRE(dims::length + dims::mass == Dim{1, 1, 0, 0, 0, 0, 0});
    STATIC_REQUIRE(dims::velocity + dims::time == dims::length);
    STATIC_REQUIRE(dims::force + dims::length == dims::energy);
}

TEST_CASE("units.dim.plus_equals_multiply", "[units][property]") {
    constexpr Dim samples[] = {Dim{},          dims::length, dims::mass,  dims::time,
                               dims::energy,   dims::force,  Dim{2, -1, 3, 0, 0, 0, 0}};
    for (Dim a : samples) {
        for (Dim b : samples) {
            REQUIRE(a + b == a * b);
        }
    }
}

TEST_CASE("units.dim.minus_equals_divide", "[units][property]") {
    constexpr Dim samples[] = {Dim{},          dims::length, dims::mass,  dims::time,
                               dims::energy,   dims::force,  Dim{2, -1, 3, 0, 0, 0, 0}};
    for (Dim a : samples) {
        for (Dim b : samples) {
            REQUIRE(a - b == a / b);
        }
    }
}

TEST_CASE("units.dim.subtract_exact", "[units]") {
    STATIC_REQUIRE(dims::area - dims::length == dims::length);
    STATIC_REQUIRE(dims::velocity - dims::time == dims::acceleration);
    STATIC_REQUIRE(dims::energy - dims::time == dims::power);
    STATIC_REQUIRE(dims::force - dims::area == dims::pressure);
    STATIC_REQUIRE(dims::mass - dims::volume == dims::density);
    STATIC_REQUIRE(dims::voltage - dims::current == dims::resistance);
    STATIC_REQUIRE(dims::magnetic_flux - dims::area == dims::magnetic_flux_density);
}

TEST_CASE("units.dim.multiply_exact", "[units]") {
    STATIC_REQUIRE(dims::length * dims::length == Dim{2, 0, 0, 0, 0, 0, 0});
    STATIC_REQUIRE(dims::mass * dims::mass == Dim{0, 2, 0, 0, 0, 0, 0});
    STATIC_REQUIRE(dims::length * dims::mass == Dim{1, 1, 0, 0, 0, 0, 0});
    STATIC_REQUIRE(dims::acceleration * dims::mass == dims::force);
    STATIC_REQUIRE(dims::current * dims::time == dims::charge);
    STATIC_REQUIRE(dims::force * dims::length == dims::energy);
    // Component exactness (the runtime path)
    constexpr Dim samples[] = {dims::length, dims::mass, dims::time, dims::current};
    for (Dim a : samples) {
        for (Dim b : samples) {
            const Dim r = a * b;
            REQUIRE(r.L == static_cast<DimExp>(a.L + b.L));
            REQUIRE(r.M == static_cast<DimExp>(a.M + b.M));
            REQUIRE(r.T == static_cast<DimExp>(a.T + b.T));
            REQUIRE(r.I == static_cast<DimExp>(a.I + b.I));
        }
    }
}

TEST_CASE("units.dim.scalar_multiply", "[units]") {
    STATIC_REQUIRE(dim_pow<2>(dims::length) == dims::area);
    STATIC_REQUIRE(dim_pow<3>(dims::length) == dims::volume);
    STATIC_REQUIRE(dim_pow<-1>(dims::time) == dims::frequency);
    STATIC_REQUIRE(dim_pow<-2>(dims::time) == Dim{0, 0, -2, 0, 0, 0, 0});
    STATIC_REQUIRE(dim_pow<2>(dims::length) * dims::mass == dims::moment_of_inertia);
}

TEST_CASE("units.dim.negate", "[units]") {
    STATIC_REQUIRE(dim_inverse(dims::time) == dims::frequency);
    STATIC_REQUIRE(dim_inverse(dims::length) == dims::reciprocal_length);
    STATIC_REQUIRE(dim_inverse(dim_inverse(dims::length)) == dims::length);
    STATIC_REQUIRE(dim_inverse(Dim{}) == Dim{});
}

// -- Properties: algebraic laws -----------------------------------------------

// Note: P1 validates the algebraic laws with a fixed sample set. After RapidCheck is wired in
// (dependency gate §1) these cases become RC_GTEST_PROP with **unchanged case ids** (a contract promises a property, not a mechanism).

TEST_CASE("units.dim.multiply_commutative", "[units][property]") {
    constexpr Dim samples[] = {Dim{},
                               dims::length,
                               dims::mass,
                               dims::time,
                               dims::current,
                               dims::temperature,
                               dims::amount,
                               dims::luminous,
                               Dim{2, -1, 3, 0, 0, 0, 0},
                               Dim{-1, 2, -3, 1, 0, 0, 0}};
    for (Dim a : samples) {
        for (Dim b : samples) {
            REQUIRE(a * b == b * a);
        }
    }
}

TEST_CASE("units.dim.multiply_associative", "[units][property]") {
    constexpr Dim samples[] = {Dim{}, dims::length, dims::mass, dims::time, Dim{2, -1, 3, 0, 0, 0, 0}};
    for (Dim a : samples) {
        for (Dim b : samples) {
            for (Dim c : samples) {
                REQUIRE((a * b) * c == a * (b * c));
            }
        }
    }
}

TEST_CASE("units.dim.multiply_identity", "[units][property]") {
    constexpr Dim samples[] = {Dim{}, dims::length, dims::mass, dims::energy, Dim{-2, 1, 0, 0, 0, 0, 0}};
    for (Dim a : samples) {
        REQUIRE(a * Dim{} == a);
        REQUIRE(Dim{} * a == a);
    }
}

TEST_CASE("units.dim.multiply_exact", "[units][property]") {
    constexpr Dim samples[] = {dims::length, dims::mass, dims::time, dims::current};
    for (Dim a : samples) {
        for (Dim b : samples) {
            const Dim r = a * b;
            REQUIRE(r.L == static_cast<DimExp>(a.L + b.L));
            REQUIRE(r.M == static_cast<DimExp>(a.M + b.M));
            REQUIRE(r.T == static_cast<DimExp>(a.T + b.T));
            REQUIRE(r.I == static_cast<DimExp>(a.I + b.I));
        }
    }
}

TEST_CASE("units.dim.divide_exact", "[units]") {
    const Dim a = dims::energy;
    const Dim b = dims::time;
    const Dim r = a / b;
    REQUIRE(r.L == 2);
    REQUIRE(r.M == 1);
    REQUIRE(r.T == -3);
    REQUIRE(r == dims::power);
}

TEST_CASE("units.dim.divide_self_is_none", "[units][property]") {
    constexpr Dim samples[] = {dims::length, dims::mass, dims::time, dims::energy,
                               Dim{2, -1, 3, 0, 0, 0, 0}};
    for (Dim a : samples) {
        REQUIRE(a / a == Dim{});
    }
}

TEST_CASE("units.dim.divide_by_none_identity", "[units][property]") {
    constexpr Dim samples[] = {Dim{}, dims::length, dims::force, Dim{-2, 1, 0, 0, 0, 0, 0}};
    for (Dim a : samples) {
        REQUIRE(a / Dim{} == a);
    }
}

TEST_CASE("units.dim.pow_one_identity", "[units][property]") {
    constexpr Dim samples[] = {Dim{}, dims::length, dims::energy, Dim{2, -1, 3, 0, 0, 0, 0}};
    for (Dim a : samples) {
        REQUIRE(dim_pow<1>(a) == a);
    }
}

TEST_CASE("units.dim.pow_zero_is_none", "[units][property]") {
    constexpr Dim samples[] = {Dim{}, dims::length, dims::energy, Dim{2, -1, 3, 0, 0, 0, 0}};
    for (Dim a : samples) {
        REQUIRE(dim_pow<0>(a) == Dim{});
    }
}

TEST_CASE("units.dim.pow_composition", "[units][property]") {
    constexpr Dim samples[] = {Dim{}, dims::length, dims::time, Dim{1, -2, 0, 0, 0, 0, 0}};
    for (Dim a : samples) {
        REQUIRE(dim_pow<2>(dim_pow<3>(a)) == dim_pow<6>(a));
        REQUIRE(dim_pow<3>(dim_pow<1>(a)) == dim_pow<3>(a));
    }
}
