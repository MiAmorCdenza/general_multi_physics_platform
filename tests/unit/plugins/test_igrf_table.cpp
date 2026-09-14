/**
 * @file test_igrf_table.cpp
 * @brief The generated coefficient table, against the published IGRF values.
 *
 * The table is generated from the Fortran's `DATA` statements by `scripts/generate_igrf_table.py`, and its gate
 * regenerates it and compares. What a gate cannot do is notice that the *generator* mis-assembled a continuation line,
 * dropped a `39*0.D0` repeat, or read a number with a space inside it -- all three of which happened while it was being
 * written. So a handful of values are asserted here against the published tables, chosen at the start of an epoch, in
 * the middle of one, and in the secular variation, where the packing and the parsing are both most exposed.
 */
#include <qp/plugins/magnetosphere/igrf_table.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace {

using namespace qp::plugins::magnetosphere::igrf;

}  // namespace

TEST_CASE("geopack.igrf.the_generated_table_holds_the_published_epochs", "[magnetosphere][igrf]") {
    // The arrays are the Fortran's own size, and the packing is one slot per (n, m) pair: 105 for degrees 1 to 13,
    // 45 for the secular variation, which stops at degree 8.
    STATIC_REQUIRE(kG65.size() == 105);
    STATIC_REQUIRE(kH65.size() == 105);
    STATIC_REQUIRE(kG20.size() == 105);
    STATIC_REQUIRE(kH20.size() == 105);
    STATIC_REQUIRE(kDG20.size() == 45);
    STATIC_REQUIRE(kDH20.size() == 45);

    // **The 1965 epoch**, at the three places a parsing mistake would land: the leading slot (which the model leaves
    // at zero), the axial dipole `g10`, and `g11`.
    REQUIRE(kG65[0] == 0.0);
    REQUIRE(kG65[1] == -30334.0);
    REQUIRE(kG65[2] == -2119.0);
    REQUIRE(kH65[2] == 5776.0);

    // **The 2020 epoch**, the last one with measured coefficients: `g10` has decayed from -30334 to -29404.8 nT over
    // fifty-five years, and the axial dipole is what a reader recognises.
    REQUIRE(kG20[1] == -29404.8);
    REQUIRE(kH20[2] == 4652.5);

    // **The 2020 secular variation**, which is what the model extrapolates with after its last epoch: the first
    // entries are zero by construction (there is no monopole) and the dipole's rate of change is +5.7 nT per year.
    REQUIRE(kDG20[0] == 0.0);
    REQUIRE(kDG20[1] == 5.7);
    REQUIRE(kDH20[2] == -25.9);

    // And the epochs are ordered in time, which is what the interpolation relies on: a table read out of order would
    // compile, pass a size check, and interpolate nonsense. **The axial dipole is weakening**, so its magnitude falls
    // monotonically from 1965 to 2020 -- 30334, 30220, 30100, 29992 down to 29404.8 nanotesla -- which is both the
    // ordering check and a physical fact about the field the model describes. The first version of this asked for the
    // *values* to increase, which they do toward zero; a case that gets the sign of a drift wrong is the kind of thing
    // a reader catches in a second and a test suite never does.
    REQUIRE(std::abs(kG65[1]) > std::abs(kG70[1]));
    REQUIRE(std::abs(kG70[1]) > std::abs(kG75[1]));
    REQUIRE(std::abs(kG75[1]) > std::abs(kG80[1]));
    REQUIRE(std::abs(kG15[1]) > std::abs(kG20[1]));
}
