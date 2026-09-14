/**
 * @file test_oracle_geopack.cpp
 * @brief The oracle's own identity, asserted: the Fortran this tree compiles is the Fortran the port is measured
 *        against, and it answers what it answered when it was first run.
 *
 * **Why a test for the oracle itself.** Everything the port will claim rests on these routines: "the C++ agrees with
 * the original" is only worth something if the original is the original and the numbers it produces are the ones that
 * were recorded. So this file pins both -- the interface (the symbol names, the calling convention, the state block's
 * layout) and a handful of measured outputs, including the two mistakes that were made getting here:
 *
 *   * **units**: `IGRF_GSW_08` returns **nanotesla**, not tesla. The kit works in tesla, so the port converts at its
 *     boundary, and the conversion is asserted there rather than remembered;
 *   * **the state block**: `COMMON /GEOPACK1/ AA(10),SPS,CPS,BB(22)` holds the **sine and cosine** of the dipole
 *     tilt. The first version of the smoke driver guessed a longer layout and read a tilt of zero for every date --
 *     a wrong answer that looks like a right one.
 *
 * The measured values below are from 2024-01-01 00:00 UT with a 400 km/s solar wind along -x in GSE, which is the
 * driver set `tests/oracle/oracle_smoke.f90` uses: a dipole tilt of -25.38 degrees (the real tilt for that date) and
 * an equatorial field of (1065.497, 37.867, 1016.775) nT at three earth radii.
 */
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

// **The Fortran interface, at file scope.** A state block declared inside an anonymous namespace gets internal
// linkage, which is how the first version of this file linked against nothing and reported `undefined reference to
// (anonymous namespace)::geopack1_` -- a reminder that `extern "C"` says what the *name* looks like, not that the
// symbol is external.

/// @brief The state block, declared exactly as `RECALC_08` declares it.
///
/// Line 318 of the vendored `Geopack-2008_dp.for`: `COMMON /GEOPACK1/ AA(10),SPS,CPS,BB(22)`. All doubles, so the
/// struct's layout is the block's layout with no padding to worry about -- and the names are kept because a reader
/// comparing the two should not have to map them.
struct Geopack1Block final {
    double aa[10];
    double sps;  ///< sine of the dipole tilt
    double cps;  ///< cosine of the dipole tilt
    double bb[22];
};

extern "C" {
extern Geopack1Block geopack1_;
/// @brief Establishes the state: date, time, and the solar wind velocity in GSE, in km/s.
void recalc_08_(const int* iyear, const int* iday, const int* ihour, const int* minute, const int* isec,
                const double* vgsex, const double* vgsey, const double* vgsez);
/// @brief The internal field (IGRF) at a GSW position in earth radii, in nanotesla.
void igrf_gsw_08_(const double* xgsw, const double* ygsw, const double* zgsw, double* hxgsw, double* hygsw,
                  double* hzgsw);
/// @brief The same field in geocentric spherical coordinates.
void igrf_geo_08_(const double* r, const double* theta, const double* phi, double* br, double* btheta, double* bphi);
/// @brief The centred dipole alone, in GSW, in nanotesla. The kit has its own dipole; this is the cross-check.
void dip_08_(const double* xgsw, const double* ygsw, const double* zgsw, double* bxgsw, double* bygsw, double* bzgsw);
/// @brief GSW and GSE differ by the rotation that puts the solar wind along -x.
void gswgse_08_(const double* xgsw, const double* ygsw, const double* zgsw, double* xgse, double* ygse, double* zgse,
                const int* j);
/// @brief Geocentric cartesian and geodetic.
void geodgeo_08_(const double* h, const double* xmu, double* r, double* theta, const int* j);
}

namespace {

/// @brief The date and solar wind the recorded numbers belong to.
void recalc_2024_day1() {
    const int year = 2024;
    const int day = 1;
    const int hour = 0;
    const int minute = 0;
    const int second = 0;
    const double vx = -400.0;
    const double vy = 0.0;
    const double vz = 0.0;
    recalc_08_(&year, &day, &hour, &minute, &second, &vx, &vy, &vz);
}

/// @brief The dipole tilt in degrees, read from the state block the way the source declares it.
[[nodiscard]] double tilt_degrees() {
    return std::asin(std::clamp(geopack1_.sps, -1.0, 1.0)) * 57.29577951308232;
}

}  // namespace

TEST_CASE("oracle.geopack.the_state_block_holds_the_tilt_it_was_given", "[oracle]") {
    recalc_2024_day1();
    // **-25.38 degrees for 2024-01-01 00:00 UT.** The tilt runs through a day and a year; a state block read as an
    // *angle* rather than a sine gives zero for every date, and an angle for the wrong epoch gives a plausible
    // smaller number. The assertion is the measured value, and its size is what makes it a check rather than a
    // formality.
    const double tilt = tilt_degrees();
    REQUIRE(tilt < 0.0);
    REQUIRE(std::abs(tilt - (-25.3796)) < 1.0e-3);
    // The cosine is consistent with the sine, which is what "the block holds sin and cos" means.
    REQUIRE(std::abs(std::cos(tilt / 57.29577951308232) - geopack1_.cps) < 1.0e-12);

    // A different date gives a different tilt: without this, a block that happened to hold a constant would pass.
    const int year = 2024;
    const int day = 182;
    const int hour = 12;
    const int minute = 0;
    const int second = 0;
    const double vx = -400.0;
    const double vy = 0.0;
    const double vz = 0.0;
    recalc_08_(&year, &day, &hour, &minute, &second, &vx, &vy, &vz);
    const double other = tilt_degrees();
    REQUIRE(std::abs(other - tilt) > 10.0);
}

TEST_CASE("oracle.geopack.the_igrf_is_nanotesla_at_three_earth_radii", "[oracle]") {
    recalc_2024_day1();
    const double x = 3.0;
    const double y = 0.0;
    const double z = 0.0;
    double hx = 0.0;
    double hy = 0.0;
    double hz = 0.0;
    igrf_gsw_08_(&x, &y, &z, &hx, &hy, &hz);
    // The measured values. `IGRF_GSW_08` is a GSW call, so the tilt puts a little of the field along y -- which is
    // exactly the part a dipole-only internal field cannot produce, and therefore the part the port has to earn.
    REQUIRE(std::abs(hx - 1065.497) < 0.01);
    REQUIRE(std::abs(hy - 37.867) < 0.01);
    REQUIRE(std::abs(hz - 1016.775) < 0.01);
    const double magnitude = std::sqrt(hx * hx + hy * hy + hz * hz);
    REQUIRE(std::abs(magnitude - 1473.4) < 0.5);
    // **Nanotesla, not tesla**: the same point a thousand times further out is a thousandth of the field, and the
    // order of magnitude is what says which unit this is.
    REQUIRE(magnitude > 1000.0);
    REQUIRE(magnitude < 2000.0);

    // The dipole alone, for comparison, is close to but not equal to the IGRF: the difference is the non-dipole part,
    // which is the whole reason the full IGRF is worth porting.
    double bx = 0.0;
    double by = 0.0;
    double bz = 0.0;
    dip_08_(&x, &y, &z, &bx, &by, &bz);
    const double dipole_magnitude = std::sqrt(bx * bx + by * by + bz * bz);
    REQUIRE(dipole_magnitude > 1000.0);
    REQUIRE(dipole_magnitude < magnitude);
}

TEST_CASE("oracle.geopack.the_transforms_round_trip", "[oracle]") {
    recalc_2024_day1();
    // GSW and GSE are two right-handed frames that differ by a rotation about x: the transform and its inverse are
    // the same call with the direction flag the other way, and a point survives the round trip.
    const double x = 4.0;
    const double y = -2.0;
    const double z = 1.5;
    double xgse = 0.0;
    double ygse = 0.0;
    double zgse = 0.0;
    const int forward = 1;
    gswgse_08_(&x, &y, &z, &xgse, &ygse, &zgse, &forward);
    // The x axis is common to both frames, and the rotation is a rotation: lengths are preserved. **The bound is the
    // measurement, not the ideal**: the round trip comes back to within 1e-10 of the original length, which is the
    // Fortran's own internal precision for the angles it recomputes on each call (the first version of this assertion
    // asked for 1e-12 and reported 0.0000000001). A bound nobody measured is a bound that fails the first time the
    // compiler changes.
    REQUIRE(std::abs(xgse - x) < 1.0e-12);
    REQUIRE(std::abs(std::sqrt(xgse * xgse + ygse * ygse + zgse * zgse) - std::sqrt(x * x + y * y + z * z)) < 1.0e-9);
    REQUIRE(std::abs(ygse - y) > 1.0e-6);  // it is a rotation, so y does move

    double back_x = 0.0;
    double back_y = 0.0;
    double back_z = 0.0;
    const int inverse = -1;
    // **The trap in this signature, and the reason the port will not copy it.** The six slots are positional and
    // fixed -- `(XGSW, YGSW, ZGSW, XGSE, YGSE, ZGSE, J)` -- and `J` decides which trio is read and which is written:
    // for `J > 0` the GSW trio comes in and the GSE trio goes out, and for `J < 0` it is the other way round. The
    // first version of this call passed the inverse direction with the outputs in the last three slots, where they
    // were read as inputs, and the result was three zeros -- a caller error the signature invites. The port gets two
    // named functions instead, which is what the note in `docs/plan-tree.md` remembers.
    gswgse_08_(&back_x, &back_y, &back_z, &xgse, &ygse, &zgse, &inverse);
    // The round trip is exact to the precision of the Fortran's own angle arithmetic.
    REQUIRE(std::abs(back_x - x) < 1.0e-9);
    REQUIRE(std::abs(back_y - y) < 1.0e-9);
    REQUIRE(std::abs(back_z - z) < 1.0e-9);
}
