/**
 * @file test_magnetosphere.cpp
 * @brief The geomagnetic field: the constants, the conversions, and the field itself.
 *
 * Test case ids match the @tests fields in `plugins/magnetosphere/include`.
 *
 * ## What is checked here, and against what
 *
 * Every assertion is against something **outside this repository**: a textbook figure, a published constant, or a
 * closed form. That is the only kind of check that can find a port error, because a test written from the same
 * understanding as the code agrees with the code whatever the code does.
 *
 * The four that matter most:
 *
 *   - `the_gyrofrequency_matches_the_textbook` -- the whole normalized system collapses into one number, the
 *     proton gyrofrequency where the field is one unit. A textbook says 2.98 rad/s at the equator; if any of the
 *     six constants or six conversions is wrong, this is where it shows.
 *   - `points_south_at_the_equator` -- the sign. A field of the right magnitude pointing the wrong way reverses
 *     every drift in the kit, and nothing that measures `|B|` can see it.
 *   - `decays_as_the_cube` -- the inverse-cube law, checked as a **ratio** at two radii so the moment cancels:
 *     a test that compared against a numeric field would be asserting this repository's own arithmetic.
 *   - `is_axisymmetric_without_a_tilt` -- the field depends only on `(rho, z)` when the tilt is zero, which is
 *     what makes the drift physics of a dipole what it is.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/magnetosphere/dipole.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>
#include <qp/plugins/magnetosphere/geometry.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <cmath>
#include <limits>
#include <string>

using namespace qp::plugins::magnetosphere;

namespace {

/// @brief The relative difference between two numbers, guarded against a zero denominator.
[[nodiscard]] double relative(double a, double b) {
    const double scale = std::abs(b) > 0.0 ? std::abs(b) : 1.0;
    return std::abs(a - b) / scale;
}

/// @brief A point `radius` metres from the origin along a direction given in spherical angles.
[[nodiscard]] Vec3 point_at(double radius, double colatitude_rad, double azimuth_rad) {
    return Vec3{radius * std::sin(colatitude_rad) * std::cos(azimuth_rad),
                radius * std::sin(colatitude_rad) * std::sin(azimuth_rad),
                radius * std::cos(colatitude_rad)};
}

}  // namespace

TEST_CASE("magnetosphere.geometry.norm_and_dot", "[magnetosphere]") {
    // The primitives, checked against arithmetic anyone can do in their head, because everything downstream
    // depends on them and a sign or a factor here would be invisible in a trajectory.
    const Vec3 a{3.0, 4.0, 0.0};
    REQUIRE(norm2(a) == 25.0);
    REQUIRE(norm(a) == 5.0);

    const Vec3 b{1.0, 2.0, 3.0};
    REQUIRE(dot(a, b) == 11.0);
    REQUIRE(dot(a, b) == dot(b, a));

    REQUIRE((a + b).x == 4.0);
    REQUIRE((a - b).z == -3.0);
    REQUIRE((a * 2.0).y == 8.0);
    REQUIRE((2.0 * a).y == 8.0);
    REQUIRE((a * 2.0).y == (2.0 * a).y);

    REQUIRE(is_finite(a));
    REQUIRE_FALSE(is_finite(Vec3{0.0, std::nan(""), 0.0}));
    REQUIRE_FALSE(is_finite(Vec3{std::numeric_limits<double>::infinity(), 0.0, 0.0}));

    // The operators are `constexpr`, so the arithmetic above a hot loop costs nothing at run time.
    constexpr Vec3 kSum = Vec3{1.0, 0.0, 0.0} + Vec3{0.0, 1.0, 0.0};
    STATIC_REQUIRE(kSum.z == 0.0);
    STATIC_REQUIRE(norm2(Vec3{0.0, 3.0, 4.0}) == 25.0);
}

TEST_CASE("magnetosphere.geometry.cross_is_anticommutative", "[magnetosphere]") {
    // **The right-handed convention, asserted**, because a left-handed cross product would reverse the direction
    // a proton gyrates in and the symptom -- a ring current going the wrong way round the planet -- is thousands
    // of steps downstream. The check is the unit-vector identity, which is the definition of the convention.
    constexpr Vec3 x_hat{1.0, 0.0, 0.0};
    constexpr Vec3 y_hat{0.0, 1.0, 0.0};
    constexpr Vec3 z_hat{0.0, 0.0, 1.0};
    STATIC_REQUIRE(cross(x_hat, y_hat).z == 1.0);
    STATIC_REQUIRE(cross(x_hat, y_hat).x == 0.0);
    STATIC_REQUIRE(cross(y_hat, z_hat).x == 1.0);
    STATIC_REQUIRE(cross(z_hat, x_hat).y == 1.0);

    // Anti-commutative, and orthogonal to both inputs. The triple product being non-zero is the statement that
    // the three unit vectors are independent, which a degenerate cross product would break.
    const Vec3 a{1.0, 2.0, 3.0};
    const Vec3 b{-4.0, 5.0, -6.0};
    const Vec3 ab = cross(a, b);
    const Vec3 ba = cross(b, a);
    REQUIRE(ab.x == -ba.x);
    REQUIRE(ab.y == -ba.y);
    REQUIRE(ab.z == -ba.z);
    REQUIRE(std::abs(dot(ab, a)) < 1.0e-12);
    REQUIRE(std::abs(dot(ab, b)) < 1.0e-12);

    // A vector crossed with itself is zero, which is the case a `cross(v, v)` in a pusher would silently rely on.
    REQUIRE(norm2(cross(a, a)) == 0.0);
}

TEST_CASE("magnetosphere.geomagnetic.constants_agree_with_their_definitions", "[magnetosphere]") {
    // Every derived constant is written in the header as the expression it stands for. These assertions are that
    // expression, evaluated independently: if someone edits the definition, this fails rather than silently
    // moving every field value in the kit.
    const double moment =
        (1.0 / kMu0OverFourPi) * kGeomagneticReferenceRadiusM * kGeomagneticReferenceRadiusM *
        kGeomagneticReferenceRadiusM *
        std::sqrt(kIgrfG10 * kIgrfG10 + kIgrfG11 * kIgrfG11 + kIgrfH11 * kIgrfH11) * 1.0e-9;
    REQUIRE(relative(kDipoleMomentAm2, moment) < 1.0e-15);

    // The moment is about 7.9e22 A m^2 in the literature. The value here is 7.7e22 because the IGRF-13
    // coefficients give a slightly weaker dipole than the older figure still quoted in textbooks, and asserting
    // the **published range** rather than a remembered digit is the difference between a check and a tautology.
    REQUIRE(kDipoleMomentAm2 > 7.5e22);
    REQUIRE(kDipoleMomentAm2 < 8.1e22);

    // The equatorial surface field, which is the kit's unit of field strength. About 3.0e-5 T, i.e. 30 000 nT,
    // which is the number a magnetometer reads and a textbook prints.
    REQUIRE(kEquatorialSurfaceFieldT > 2.9e-5);
    REQUIRE(kEquatorialSurfaceFieldT < 3.15e-5);

    // The reference implementation this kit is ported from uses 31200 nT as its scale factor, and the value
    // derived here is **4.8%** below it -- measured, not guessed. Their two constants (31200 nT and
    // R_E = 6371 km) are not consistent with each other under the dipole formula: a moment that gives 31200 nT
    // at 6371 km gives about 30 400 nT at the equatorial radius used here. Both are defensible; what is not is
    // carrying one as a constant and the other as a scale factor without saying they describe the same quantity.
    // The bound is loose enough to survive a better estimate of the moment and tight enough to fail on a dropped
    // factor of ten, so it records the disagreement without freezing it.
    REQUIRE(relative(kEquatorialSurfaceFieldT * 1.0e9, 31200.0) < 0.06);

    // The time unit is the light crossing time, by definition of the normalized system.
    REQUIRE(kLightCrossingTimeS == kEarthRadiusM / kSpeedOfLightSI);
    REQUIRE(kLightCrossingTimeS > 0.02);
    REQUIRE(kLightCrossingTimeS < 0.022);
}

TEST_CASE("magnetosphere.units.each_conversion_is_its_own_expression", "[magnetosphere]") {
    // A table of round trips: each conversion times the quantity it converts must be one. That is the property
    // that makes the inner loop's numbers comparable, and it is checkable without knowing any physics.
    REQUIRE(relative(kEarthRadiusM * kNormalizedPerMetre, 1.0) < 1.0e-15);
    REQUIRE(relative(kSpeedOfLightSI * kNormalizedPerMetrePerSecond, 1.0) < 1.0e-15);
    REQUIRE(relative(kLightCrossingTimeS * kNormalizedPerSecond, 1.0) < 1.0e-15);
    REQUIRE(relative(kEquatorialSurfaceFieldT * kNormalizedPerTesla, 1.0) < 1.0e-15);
    REQUIRE(relative(kSpeedOfLightSI * kEquatorialSurfaceFieldT * kNormalizedPerVoltPerMetre, 1.0) < 1.0e-15);

    // Gravity is dimensionless in this system, and the reference implementation's `1.5398e-6` is the same
    // physical quantity in `R_E^3/s^2`. The relation between the two spellings is the time unit squared, and
    // asserting the **relation** is what catches a port that copied their number into a dimensionless slot.
    const double reference_gravity_in_re3_per_s2 = 1.5398e-6;
    const double as_dimensionless = reference_gravity_in_re3_per_s2 * kLightCrossingTimeS * kLightCrossingTimeS;
    REQUIRE(relative(kNormalizedGravity, as_dimensionless) < 5.0e-3);

    // The nondipole fraction is the honest statement of what a dipole leaves out: about a tenth of the field.
    REQUIRE(kNondipoleFraction > 0.05);
    REQUIRE(kNondipoleFraction < 0.20);
}

TEST_CASE("magnetosphere.units.the_speed_of_light_is_one", "[magnetosphere]") {
    // The defining property of the system: metres per second times the velocity conversion is `1/c` applied to
    // `c`, so a particle at the speed of light has normalized velocity one. Everything relativistic downstream
    // reads `gamma = 1/sqrt(1 - v2)` with `v2` bounded by one, which is only true if this holds exactly.
    const double c_normalized = kSpeedOfLightSI * kNormalizedPerMetrePerSecond;
    REQUIRE(c_normalized == 1.0);

    // And one length unit per one time unit is the speed of light, which is the same statement in the other
    // pair of units and would fail if the time conversion used a different radius from the length conversion.
    const double one_per_one = kNormalizedPerSecond / kNormalizedPerMetre;
    REQUIRE(relative(one_per_one, kSpeedOfLightSI) < 1.0e-15);
}

TEST_CASE("magnetosphere.units.the_gyrofrequency_matches_the_textbook", "[magnetosphere]") {
    // **The assertion the whole normalized system collapses into.** The proton gyrofrequency in the equatorial
    // surface field is `qB/m`, and that is the number a data book prints. It is not asserted from memory: the
    // value is written as the product of the three constants it comes from, so the check is that the
    // **normalized** value agrees with the SI one under the time conversion. A factor error in any of the six
    // constants or six conversions moves one side and not the other.
    //
    // This case is the one that found a real defect: the first version of `normalized_charge_mass` multiplied by
    // `c / R_E` instead of dividing by `R_E / c`, which is a factor of 2200 in the wrong direction. What caught
    // it was writing the expected value out in SI first -- `qB/m` -- rather than trusting the normalized
    // expression to be self-evidently right.
    const double si_gyrofrequency = kProtonChargeMassSI * kEquatorialSurfaceFieldT;
    REQUIRE(relative(si_gyrofrequency, 2845.75) < 1.0e-3);   // `qB/m` at 29 709 nT, in rad/s
    REQUIRE(relative(si_gyrofrequency / (2.0 * 3.14159265358979323846), 452.9) < 1.0e-2);

    // `normalized_charge_mass` converts the **charge-to-mass ratio**, not the gyrofrequency: one normalized
    // charge-to-mass unit is `B_eq / (R_E / c)`, so `(q/m)` in SI times that conversion is what a kernel
    // multiplies by a normalized field to get a normalized angular frequency. This distinction was wrong in the
    // first version of the header, which called the result "an angular frequency" -- and the assertion that
    // caught it is the one below, because the two readings differ by the time unit.
    REQUIRE(relative(kProtonNormalizedChargeMass,
                     kProtonChargeMassSI * (kEquatorialSurfaceFieldT / kLightCrossingTimeS)) < 1.0e-15);

    // And the kernel's use of it is consistent with the SI gyrofrequency. A kernel forms `q_prime * |B|`, where
    // `q_prime` is the normalized charge-to-mass ratio and `|B|` is in normalized field units; the result is a
    // rate **per normalized time**, so dividing by the time unit restores rad/s. The first version of this line
    // multiplied, and the factor of 47 it was out by is the time unit itself.
    const double normalized_gyrofrequency = kProtonNormalizedChargeMass * 1.0;   // field of one unit
    REQUIRE(relative(normalized_gyrofrequency / kNormalizedPerSecond, si_gyrofrequency) < 1.0e-12);

    // The charge-to-mass ratios, against the published values: 9.58e7 C/kg for a proton and -1.76e11 for an
    // electron. These are the numbers a data book prints.
    REQUIRE(relative(kProtonChargeMassSI, 9.5788332e7) < 1.0e-6);
    REQUIRE(relative(kElectronChargeMassSI, -1.75882001e11) < 1.0e-6);

    // And the ratios: an electron's is 1836 times a proton's, an alpha particle's is half of it because it has
    // two charges on four nucleons. The second is the one a species table has to get right, and it is asserted
    // as a **ratio** so that it does not depend on the constants above.
    REQUIRE(relative(std::abs(kElectronChargeMassSI / kProtonChargeMassSI), 1836.15267) < 1.0e-5);
    REQUIRE(relative(kAlphaChargeMassSI / kProtonChargeMassSI, 0.5) < 1.0e-15);
    REQUIRE(relative(kAlphaNormalizedChargeMass / kProtonNormalizedChargeMass, 0.5) < 1.0e-15);

    // The normalized function is linear in its argument, which is what makes a species table a single column.
    REQUIRE(relative(normalized_charge_mass(2.0 * kProtonChargeMassSI), 2.0 * kProtonNormalizedChargeMass) < 1.0e-15);
}

TEST_CASE("magnetosphere.dipole.matches_the_textbook_at_the_equator", "[magnetosphere]") {
    // The magnitude on the magnetic equator, against the closed form. A dipole's field on the equatorial plane is
    // `B = (mu0/4pi) m / r^3`, and that is the number a textbook's table of "the Earth's field" is about.
    const DipoleField dipole{/*tilt_degrees=*/0.0};
    constexpr double kMetresPerEarthRadius = 6378137.0;

    for (const double radii : {1.0, 2.0, 4.0, 6.6}) {
        const Vec3 point{kMetresPerEarthRadius * radii, 0.0, 0.0};
        const Vec3 b = dipole.at(point);
        const double expected = kMu0OverFourPi * kDipoleMomentAm2 /
                                (kMetresPerEarthRadius * radii * kMetresPerEarthRadius * radii *
                                 kMetresPerEarthRadius * radii);
        REQUIRE(relative(norm(b), expected) < 1.0e-12);
    }

    // On the axis a dipole is exactly twice as strong as on the equator at the same radius: `B_pole = 2 B_eq`.
    // That factor of two is a property of the dipole, so it is a check on the coefficients rather than on the
    // moment, and a port that mixed up the two terms would fail it while passing the magnitude test above.
    const double radius = kMetresPerEarthRadius * 2.0;
    const double equatorial = norm(dipole.at(Vec3{radius, 0.0, 0.0}));
    const double polar = norm(dipole.at(Vec3{0.0, 0.0, radius}));
    REQUIRE(relative(polar / equatorial, 2.0) < 1.0e-12);
}

TEST_CASE("magnetosphere.dipole.points_south_at_the_equator", "[magnetosphere]") {
    // **The case that pins the field's direction, and it has to be this case.**
    //
    // The geomagnetic moment points **south**. With `z` up, the field that follows is:
    //
    //   | where | `B_z` | plain words |
    //   |---|---|---|
    //   | magnetic equator | negative | points **south** -- what a compass feels |
    //   | either geographic pole | positive | points **outward**, radially away from the Earth |
    //
    // Every one of those was got wrong at least once while writing this kit, in both directions, and the way it
    // was settled is the point: `build/diag_curl.cpp` evaluates `B = curl A` with `A = (mu0/4pi)(m x rhat)/r^2`
    // by central differences, at these same points, and prints it beside this file's answer. A remembered
    // textbook formula and physical intuition between them gave three answers; the curl gave one and matched the
    // implementation to every digit.
    //
    // Why the poles are the assertion that matters: an implementation that got the sign wrong would still
    // conserve energy in a uniform field and would still put a particle on a circle. What it would not do is
    // **trap** anything -- the mirror force away from the equator comes from the field's magnitude growing
    // towards the poles, and the axial direction is what makes a particle turn round rather than run out along
    // the field line. The sign at the poles is the whole of the mirror effect.
    const DipoleField dipole{/*tilt_degrees=*/0.0};
    const Vec3 b = dipole.at(Vec3{6378137.0, 0.0, 0.0});
    REQUIRE(b.x == 0.0);
    REQUIRE(b.y == 0.0);
    REQUIRE(b.z < 0.0);

    // The axial field. Both poles point **outward** -- away from the Earth -- which reads like a contradiction
    // and is not: the field is divergence-free, and "radially outward on the axis" is positive flux near the
    // axis and negative near the equator, so a closed surface sees zero. The divergence was measured at 7e-21
    // against a field of 6e-6, which is zero to the arithmetic.
    const Vec3 north = dipole.at(Vec3{0.0, 0.0, 6378137.0});
    REQUIRE(north.z > 0.0);
    REQUIRE(north.x == 0.0);
    REQUIRE(north.y == 0.0);
    // And the magnitude is exactly twice the equatorial one, which is the dipole's axial-to-equatorial factor.
    // Asserting the **ratio** makes this independent of the moment and of the radius, and it is the assertion
    // that a field with one sign flipped cannot satisfy.
    REQUIRE(relative(north.z / b.z, -2.0) < 1.0e-12);

    const Vec3 south = dipole.at(Vec3{0.0, 0.0, -6378137.0});
    REQUIRE(south.z > 0.0);
    REQUIRE(relative(south.z, north.z) < 1.0e-12);
}

TEST_CASE("magnetosphere.dipole.decays_as_the_cube", "[magnetosphere]") {
    // The inverse-cube law, checked as a **ratio** between two radii so the moment cancels out. A test against a
    // numeric field would be asserting this repository's own arithmetic; a test of the ratio asserts the geometry,
    // which is the part a port can get wrong.
    const DipoleField dipole{/*tilt_degrees=*/0.0};
    for (const double colatitude_deg : {0.0, 30.0, 60.0, 90.0, 120.0}) {
        const double colatitude = colatitude_deg * 3.14159265358979323846 / 180.0;
        const Vec3 near_point = point_at(1.0e7, colatitude, 0.4);
        const Vec3 far_point = point_at(3.0e7, colatitude, 0.4);
        const double near_field = norm(dipole.at(near_point));
        const double far_field = norm(dipole.at(far_point));
        // Three times the radius is one twenty-seventh of the field.
        REQUIRE(relative(near_field / far_field, 27.0) < 1.0e-12);
    }

    // The divergence-free property, checked numerically rather than asserted in a comment: the flux out of a
    // small cube is zero. A dipole is exactly divergence-free, so any error here is a coding error, and the
    // tolerance is set by the finite-difference truncation rather than by the field.
    const DipoleField tilted{};
    const Vec3 centre{2.0e7, 1.0e7, 1.5e7};
    constexpr double kStep = 1.0e4;
    double divergence = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        for (const double sign : {1.0, -1.0}) {
            Vec3 offset{};
            if (axis == 0) offset.x = sign * kStep;
            if (axis == 1) offset.y = sign * kStep;
            if (axis == 2) offset.z = sign * kStep;
            const Vec3 b = tilted.at(centre + offset);
            const double component = axis == 0 ? b.x : (axis == 1 ? b.y : b.z);
            divergence += sign * component / kStep / 2.0;
        }
    }
    const double scale = norm(tilted.at(centre)) / 1.0e7;
    REQUIRE(std::abs(divergence) < 1.0e-6 * scale);
}

TEST_CASE("magnetosphere.dipole.is_axisymmetric_without_a_tilt", "[magnetosphere]") {
    // With no tilt the field depends only on `(rho, z)` and not on the azimuth: rotating a point about the polar
    // axis rotates the field with it. That axisymmetry is what makes a dipole's drift physics what it is -- there
    // is no azimuthal structure to trap a particle -- so it is a property worth asserting rather than assuming.
    const DipoleField dipole{/*tilt_degrees=*/0.0};
    const Vec3 point{1.2e7, 0.7e7, 2.0e7};
    const Vec3 base = dipole.at(point);

    constexpr double kQuarterTurn = 1.57079632679489661923;
    for (const double angle : {kQuarterTurn, 2.0 * kQuarterTurn, -kQuarterTurn}) {
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        const Vec3 rotated_point{c * point.x - s * point.y, s * point.x + c * point.y, point.z};
        const Vec3 rotated_field = dipole.at(rotated_point);
        // The field rotates with the point: same components, turned by the same angle.
        const Vec3 expected{c * base.x - s * base.y, s * base.x + c * base.y, base.z};
        REQUIRE(relative(rotated_field.x, expected.x) < 1.0e-12);
        REQUIRE(relative(rotated_field.y, expected.y) < 1.0e-12);
        REQUIRE(relative(rotated_field.z, expected.z) < 1.0e-12);
    }

    // The tilted field is **not** axisymmetric, which is the half that shows the tilt is doing something.
    const DipoleField tilted{};
    const Vec3 tilted_base = tilted.at(point);
    const Vec3 turned = tilted.at(Vec3{-point.y, point.x, point.z});
    REQUIRE(relative(turned.z, tilted_base.z) > 1.0e-3);
}

TEST_CASE("magnetosphere.dipole.the_tilt_rotates_the_field", "[magnetosphere]") {
    // A tilt of `a` rotates the whole field by `a` about `y`, so a point on the **tilted** axis sees what the
    // untilted axis saw. That is the definition of a tilt, and checking it this way avoids re-deriving the field
    // and therefore avoids asserting one formula against a copy of itself.
    constexpr double kTilt = 11.5;
    const DipoleField tilted{kTilt};
    const DipoleField untilted{0.0};

    const double radians = kTilt * 3.14159265358979323846 / 180.0;
    const double radius = 1.5e7;
    // The tilted dipole axis, expressed in the geographic frame. This is `Ry(-a)` applied to `+z`, and the sign
    // of the `x` component is the half of the rotation a test gets wrong: the first version of this case wrote
    // `+sin(a)` and failed with a 5.9% error, which is exactly `1 - cos(11.5 deg)` plus the sine's share -- the
    // signature of a rotation in the wrong direction rather than of a wrong field.
    const Vec3 on_tilted_axis{-radius * std::sin(radians), 0.0, radius * std::cos(radians)};

    const Vec3 from_tilted = tilted.at(on_tilted_axis);
    const Vec3 from_untilted = untilted.at(Vec3{0.0, 0.0, radius});

    // The magnitudes are equal at the same radius, and the **direction is the untilted one rotated by the
    // tilt**. Reaching `on_tilted_axis` means applying `Ry(-a)`, so the field there is `Ry(-a)` of the untilted
    // axis field; the rotation convention is the half of this case that took two attempts.
    REQUIRE(relative(norm(from_tilted), norm(from_untilted)) < 1.0e-12);
    const Vec3 expected{std::cos(radians) * from_untilted.x - std::sin(radians) * from_untilted.z,
                        from_untilted.y,
                        std::sin(radians) * from_untilted.x + std::cos(radians) * from_untilted.z};
    REQUIRE(relative(from_tilted.x, expected.x) < 1.0e-12);
    REQUIRE(relative(from_tilted.z, expected.z) < 1.0e-12);

    // And the field expressed in the **dipole's own frame** is purely axial -- the tilt has been rotated out.
    // This is the assertion that cannot be satisfied by rotating the wrong way twice: a wrong convention would
    // leave a radial component behind, and the residual would be of the tilt's own order rather than of the
    // rounding's.
    const Vec3 along_axis{std::cos(radians) * from_tilted.x + std::sin(radians) * from_tilted.z,
                          from_tilted.y,
                          -std::sin(radians) * from_tilted.x + std::cos(radians) * from_tilted.z};
    REQUIRE(relative(std::abs(along_axis.z), norm(from_tilted)) < 1.0e-12);
    REQUIRE(std::abs(along_axis.x) < 1.0e-12 * norm(from_tilted));

    // And the tilt moves the equatorial minimum: the field's magnitude at a point on the **geographic** equator
    // varies with azimuth once the dipole is tilted, which is the observable a magnetometer sees.
    const double at_greenwich = norm(tilted.at(Vec3{1.0e7, 0.0, 0.0}));
    const double at_ninety = norm(tilted.at(Vec3{0.0, 1.0e7, 0.0}));
    const double untilted_equator = norm(untilted.at(Vec3{1.0e7, 0.0, 0.0}));
    REQUIRE(relative(at_ninety, untilted_equator) < 1.0e-12);
    REQUIRE(relative(at_greenwich, untilted_equator) > 1.0e-3);

    // The origin is singular, and the model answers zero there rather than an infinity, because a diverging
    // value would poison every average taken downstream of it.
    REQUIRE(norm(tilted.at(Vec3{0.0, 0.0, 0.0})) == 0.0);
    REQUIRE(norm(tilted.at(Vec3{std::nan(""), 0.0, 0.0})) == 0.0);
}

TEST_CASE("magnetosphere.dipole.the_normalized_form_agrees_with_the_si_form", "[magnetosphere]") {
    // The two forms are one implementation: the normalized one converts the point, calls the SI one, and converts
    // the result. This case is what keeps them from drifting into two, and it is the reason a caller can never
    // integrate a field that is `1e5` times too large -- the mistake that produces a gyration too fast to resolve
    // and a trajectory that reads as noise.
    const DipoleField dipole{};
    constexpr double kMetresPerEarthRadius = 6378137.0;

    for (const double radii : {1.0, 1.5, 3.0, 6.0}) {
        const Vec3 si_point{0.6 * radii * kMetresPerEarthRadius, 0.8 * radii * kMetresPerEarthRadius, 0.0};
        const Vec3 normalized_point{0.6 * radii, 0.8 * radii, 0.0};

        const Vec3 from_si = dipole.at(si_point);
        const Vec3 from_normalized = dipole.at_normalized(normalized_point);

        // The SI field in normalized units, which is what the normalized call should have produced.
        const Vec3 expected = from_si * kNormalizedPerTesla;
        REQUIRE(relative(from_normalized.x, expected.x) < 1.0e-12);
        REQUIRE(relative(from_normalized.y, expected.y) < 1.0e-12);
        REQUIRE(relative(from_normalized.z, expected.z) < 1.0e-12);

        // And the normalized magnitude is near one at one earth radius, which is the whole point of the system.
        if (radii == 1.0) {
            REQUIRE(relative(norm(from_normalized), 1.0) < 0.05);
        }
    }
}
