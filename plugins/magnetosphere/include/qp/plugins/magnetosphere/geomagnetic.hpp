/**
 * @file geomagnetic.hpp
 * @brief The Earth, as numbers: its size, its dipole moment, its tilt, and its gravity.
 *
 * ## The rule this file follows
 *
 * **One defining source per constant, and every derived number written as the expression that derives it.** The
 * reference implementation this kit is ported from works in normalized units and carries five magic constants --
 * `q' = (q/m) * 2988.5959`, `c = 299792.458 / 6371`, `GM = 1.5398e-6`, a surface field of `31200 nT` and an
 * implicit time unit of 2.1377 s. Each of them is close to right and none can be checked by reading it, because
 * the definition it stands for lives in a comment somewhere else. Two of them turn out not to be consistent with
 * each other, which is the kind of defect a file like this one exists to make visible.
 *
 * So this file has two layers. The upper one is **physical constants with their SI values and units**, each with
 * the source it comes from. The lower one holds **derived quantities written out as expressions** over those, and
 * the test verifies each derived quantity against the expression rather than against a remembered digit.
 *
 * ## Why the dipole moment is derived rather than quoted
 *
 * The moment **is** the degree-1 Gauss coefficient:
 *
 *     m = (4 pi / mu_0) * R_ref^3 * sqrt(g10^2 + g11^2 + h11^2)
 *
 * The `4 pi / mu_0` is the inverse of the `mu_0/4pi` in the field formula, so a moment derived this way and a
 * field computed from it agree by construction. Quoting a moment from the literature and a coefficient from
 * another source is how a few per cent of discrepancy gets in that nobody can then locate -- and the value that
 * comes out here, 7.71e22 A m^2, is indeed a few per cent below the 7.9e22 the older textbooks print, because
 * the IGRF-13 coefficients describe a slightly weaker dipole than the figure still quoted.
 *
 * ## The one number that is quoted rather than derived, and why
 *
 * `kNonDipoleFraction`. See the comment beside it: the derivation was attempted, its normalization was wrong in a
 * way the arithmetic itself revealed, and a stated range with a source is worth more than a formula nobody has
 * checked.
 *
 * @ownership   pure (constants)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every derived quantity equals the expression written beside it
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.geomagnetic.constants_agree_with_their_definitions
 */
#pragma once

#include <cmath>

namespace qp::plugins::magnetosphere {

// -- Physical constants: the values, with the source of each ------------------

/// @brief The Earth's equatorial radius, in metres. WGS84's semi-major axis.
///
/// The equatorial value rather than the mean, because the dipole frame's equator is where `L`-shells are counted
/// from and a mean radius would put every `L` out by the flattening.
inline constexpr double kEarthRadiusM = 6378137.0;

/// @brief The speed of light in vacuum, in metres per second. Exact by definition since 1983.
inline constexpr double kSpeedOfLightSI = 299792458.0;

/// @brief The Earth's gravitational parameter, in cubic metres per square second. `GM`, IERS conventions.
///
/// `GM` and not `G` or `M`: a gravitational acceleration is `GM * r_hat / r^2`, and the pusher never needs the two
/// separately.
inline constexpr double kEarthGravityParameterSI = 3.986004418e14;

/// @brief The length of a sidereal day, in seconds. IERS: 86164.0905 s, which is one rotation against the stars
/// rather than against the Sun.
///
/// The distinction is the whole reason this constant exists rather than 86400: corotation is the plasma moving
/// **with the planet**, and the planet turns once per sidereal day. Using the solar day would put every corotation
/// drift 0.27% slow -- a discrepancy no picture would show and every measurement of a drift period would.
inline constexpr double kSiderealDayS = 86164.0905;

/// @brief The Earth's rotation rate, in radians per second: `2 pi / kSiderealDayS`.
///
/// Written as the expression it stands for, as every constant in this file is: the number below is the quotient,
/// and a reader who wants to check it against a handbook needs the division rather than its result.
inline const double kEarthRotationRateSI = 2.0 * 3.14159265358979323846 / kSiderealDayS;

/// @brief The vacuum magnetic permeability over four pi, in henries per metre.
///
/// The `mu_0 / 4pi` that turns a magnetic dipole moment into a field. It is `1e-7` exactly in the SI
/// redefinition, which is why it is written as an expression rather than as a literal with digits.
inline constexpr double kMu0OverFourPi = 1.0e-7;

/// @brief The geomagnetic reference radius, in metres.
///
/// Geomagnetic models are defined on this sphere; it is not the equatorial radius, it is the conventional
/// reference sphere the IGRF coefficients are fitted on. The difference is 7 km out of 6378, which shifts a field
/// magnitude by 0.3% -- below what a course measures and above what a careful check would forgive, so it is
/// named rather than conflated.
inline constexpr double kGeomagneticReferenceRadiusM = 6371200.0;

/// @brief The geomagnetic dipole's tilt from the rotation axis, in degrees.
///
/// About eleven degrees, the standard first-order statement a textbook makes. The exact value drifts with the
/// epoch because the field does; this is the round figure a course quotes, and the reopening condition is a
/// question that needs the epoch's value.
inline constexpr double kMagneticTiltDegrees = 11.5;

/// @brief The IGRF-13 (epoch 2020) degree-1 Gauss coefficients, in nanotesla.
///
/// `g10`, `g11` and `h11` are the whole of what a dipole keeps. `g10` is **negative**, and that sign is the
/// single most consequential number in this file: it says the geomagnetic moment points **south**, which is why a
/// compass needle's north pole points north.
inline constexpr double kIgrfG10 = -29404.8;

/// @brief The `g11` coefficient, in nanotesla.
inline constexpr double kIgrfG11 = -1450.9;

/// @brief The `h11` coefficient, in nanotesla.
inline constexpr double kIgrfH11 = 4652.5;

/**
 * @brief The fraction of the surface field that a centred dipole does **not** describe.
 *
 * About 11%, and it is a **measured** quantity rather than one derived here: the non-dipole part is the
 * difference between the real field and its degree-1 term, and published values for the 2020 epoch are of order
 * 10-15% of the total.
 *
 * ## Why this is a stated constant and not a sum over coefficients
 *
 * The first version of this file computed it from the IGRF quadrature sums `L` and `M`, and the arithmetic was
 * wrong: with the normalization used there, the degree-1 sum came out **larger than the surface field itself**,
 * which is a contradiction rather than a small error. Rather than guess at a second normalization, the number is
 * stated with the range it is quoted over and that range is asserted in the test. A constant whose provenance is
 * written down is worth more than a formula whose normalization nobody checked.
 *
 * It is here at all because a report should be able to say **how wrong the field model is** beside a model that
 * is known to be an approximation.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Between 0 and 1
 * @invariant   Constant
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.each_conversion_is_its_own_expression
 */
inline constexpr double kNonDipoleFraction = 0.11;

// -- Derived quantities: each written as the expression it stands for ---------

/**
 * @brief The geomagnetic dipole moment, in ampere square metres.
 *
 * `m = (4 pi / mu_0) * R_ref^3 * sqrt(g10^2 + g11^2 + h11^2)`, derived from the Gauss coefficients rather than
 * quoted, so that the moment and the field computed from it agree by construction.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 7.7e22 A m^2
 * @invariant   Equals the expression above
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geomagnetic.constants_agree_with_their_definitions
 */
inline const double kDipoleMomentAm2 =
    (1.0 / kMu0OverFourPi) * kGeomagneticReferenceRadiusM * kGeomagneticReferenceRadiusM *
    kGeomagneticReferenceRadiusM *
    std::sqrt(kIgrfG10 * kIgrfG10 + kIgrfG11 * kIgrfG11 + kIgrfH11 * kIgrfH11) * 1.0e-9;

/**
 * @brief The equatorial surface field of a dipole with moment `kDipoleMomentAm2`, in tesla.
 *
 * `B = (mu_0 / 4pi) * m / R_eq^3`: the field at the equator on the **equatorial** radius, with the moment along
 * the polar axis. It is the kit's unit of magnetic field strength, so it is derived rather than quoted.
 *
 * It comes out 4.8% below the `31200 nT` the reference implementation uses as its scale factor, and that gap is
 * a finding rather than a rounding difference: the reference's two constants are not consistent with each other
 * under the dipole formula. A moment that gives 31200 nT at 6371 km cannot give 31200 nT at the equatorial
 * radius. The test records the disagreement so that nobody later "fixes" one to match the other by accident.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 3.0e-5 T, i.e. 29 700 nT
 * @invariant   Equals the expression above
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geomagnetic.constants_agree_with_their_definitions
 */
inline const double kEquatorialSurfaceFieldT =
    kMu0OverFourPi * kDipoleMomentAm2 / (kEarthRadiusM * kEarthRadiusM * kEarthRadiusM);

/**
 * @brief One earth radius expressed as the distance light travels in it: the normalized system's time unit.
 *
 * `R_E / c`, about 21 milliseconds. Not a named physical unit; it is the choice that makes `c == 1` in normalized
 * lengths per normalized time, which is what keeps every number in a Boris step near unity where a double has
 * the most room. Named here so that `units.hpp` divides by it rather than repeating it.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 0.021 s
 * @invariant   Equals `kEarthRadiusM / kSpeedOfLightSI`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.geomagnetic.constants_agree_with_their_definitions
 */
inline const double kLightCrossingTimeS = kEarthRadiusM / kSpeedOfLightSI;

}  // namespace qp::plugins::magnetosphere
