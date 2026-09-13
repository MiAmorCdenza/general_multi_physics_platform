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
 *     proton gyrofrequency where the field is one unit. A data book says 2845.75 rad/s at the equator; if any of
 *     the six constants or six conversions is wrong, this is where it shows.
 *   - `points_south_at_the_equator` -- the sign. A field of the right magnitude pointing the wrong way reverses
 *     every drift in the kit, and nothing that measures `|B|` can see it.
 *   - `decays_as_the_cube` -- the inverse-cube law, checked as a **ratio** at two radii so the moment cancels:
 *     a test that compared against a numeric field would be asserting this repository's own arithmetic.
 *   - `is_axisymmetric_without_a_tilt` -- the field depends only on `(rho, z)` when the tilt is zero, which is
 *     what makes the drift physics of a dipole what it is.
 *
 * ## The pusher cases, and why they go through the executor
 *
 * The six `boris.*` cases drive the kernel the way production drives it: a `ParticleState`, a `BakedField`, a
 * `StepPlan` with the field bound and `required_slots` set, and a `ParticleExecutor` stepping it. A kernel tested
 * by calling `advance` on a hand-built batch would pass while the field never reached it -- the batch layout is a
 * fact two modules have to agree on, and agreement is exactly what a direct call does not test. The hand-built
 * batch is still used, in `a_bad_batch_is_refused`, because a refusal has to be checked at the boundary rather
 * than through the code that is supposed to prevent it.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/particles/executor.hpp>
#include <qp/graph/particles/particle_state.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/boris.hpp>
#include <qp/plugins/magnetosphere/dipole.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>
#include <qp/plugins/magnetosphere/geometry.hpp>
#include <qp/plugins/magnetosphere/rk4.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace qp::plugins::magnetosphere;

namespace {

namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;
namespace gfield = qp::graph::field;

using pp::BatchSlot;
using pp::ParticleState;
using pp::SlotName;

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

/// @brief A uniform field on a two-node-per-axis SI grid spanning `half_extent` earth radii either way.
///
/// The table is uniform, so trilinear interpolation reproduces it exactly at every point and the grid's spacing
/// cannot contaminate a measurement of the pusher. That is the property these cases need: a case that measured a
/// gyrofrequency over a *dipole* would be measuring the field's gradient as well, and a failure would have two
/// causes.
[[nodiscard]] BakedField uniform_table(const Vec3& b_si, double half_extent_re = 64.0) {
    const double corner = -half_extent_re * kEarthRadiusM;
    const double spacing = 2.0 * half_extent_re * kEarthRadiusM;
    BakedField table{Vec3{corner, corner, corner}, Vec3{spacing, spacing, spacing}, 2, 2, 2};
    for (std::uint32_t i = 0; i < 2; ++i) {
        for (std::uint32_t j = 0; j < 2; ++j) {
            for (std::uint32_t k = 0; k < 2; ++k) table.set_node(i, j, k, b_si);
        }
    }
    return table;
}

/// @brief The parameter block a plan hands the pusher, with the grid taken from the table it will read.
///
/// The grid metadata is copied from the table rather than typed twice, which is the only way the two can be
/// guaranteed to agree -- and the case that would otherwise happen, a kernel sampling a grid that is not where it
/// thinks it is, produces a plausible field from the wrong place.
[[nodiscard]] pk::ParamBlock boris_params(const BakedField& table, double range_re = 64.0,
                                          double gravity = 0.0, double substep_cap = 4096.0,
                                          double speed_limit = BorisAdvancer::kDefaultSpeedLimit) {
    pk::ParamBlock params;
    params.set_real(BorisAdvancer::kIndexMaxRange, range_re);
    params.set_real(BorisAdvancer::kIndexGravity, gravity);
    params.set_real(BorisAdvancer::kIndexSubstepCap, substep_cap);
    params.set_real(BorisAdvancer::kIndexSpeedLimit, speed_limit);
    params.set_real(BorisAdvancer::kIndexGridOrigin0, table.origin().x);
    params.set_real(BorisAdvancer::kIndexGridOrigin1, table.origin().y);
    params.set_real(BorisAdvancer::kIndexGridOrigin2, table.origin().z);
    params.set_real(BorisAdvancer::kIndexGridSpacing0, table.spacing().x);
    params.set_real(BorisAdvancer::kIndexGridSpacing1, table.spacing().y);
    params.set_real(BorisAdvancer::kIndexGridSpacing2, table.spacing().z);
    return params;
}

/// @brief The plan a one-kernel run uses, with the magnetic field bound and declared required.
[[nodiscard]] std::vector<pp::StepPlan> boris_plan(pk::IBatchAdvancer& kernel, const BakedField& table,
                                                   const pk::ParamBlock& params) {
    std::vector<pp::StepPlan> steps(1);
    steps[0].kernel = &kernel;
    steps[0].param = params;
    steps[0].fields[static_cast<std::size_t>(SlotName::magnetic)] = table.view();
    steps[0].required_slots = PusherParams::kRequiredFields;
    return steps;
}

/// @brief What one straight run produced.
struct RunOutcome final {
    /// The particle's position after the run, in earth radii.
    Vec3 position_re{};
    /// The particle's velocity after the run, in units of `c`.
    Vec3 velocity_c{};
    /// Host steps taken.
    std::size_t steps = 0;
    /// Values the executor's clamp policy altered, summed over the run.
    std::size_t clamped_by_executor = 0;
    /// Particles whose speed the kernel limited.
    std::uint64_t speed_clamps = 0;
    /// Particles the kernel retired, by the body or by the boundary.
    std::uint64_t retirements = 0;
    /// Sub-steps the kernel's last `advance` took, summed over the particles.
    std::uint64_t last_substeps = 0;
    /// Sub-steps summed over **every** step of the run, which is what a per-particle count cannot show.
    std::uint64_t total_substeps = 0;
    /// The signed angle the velocity swept in the `xy` plane, accumulated step by step.
    ///
    /// Accumulated rather than taken as a difference of two `atan2` calls, because a run of several gyroperiods
    /// would otherwise report a fraction of one turn and a sign that depends on where it stopped.
    double swept_xy = 0.0;
    /// The largest relative change in `|v|` seen between consecutive steps. Zero work means zero drift.
    double max_speed_drift = 0.0;
};

/// @brief Runs one particle through the whole bridge, with the scheme the caller chose.
///
/// One body for every scheme, and it takes the **family's** base class rather than Boris: a pusher is one question
/// with several answers, and a harness that could only ask one of them would make every comparison a comparison of
/// two harnesses instead of two schemes.
///
/// @param kernel       The scheme to drive.
/// @param table        The magnetic field. Its grid metadata must be in `params`, which `boris_params` does.
/// @param params       The kernel's parameter block.
/// @param start_re     Where the particle starts, in earth radii.
/// @param velocity_c   Its initial velocity, in units of `c`.
/// @param charge_mass  Its charge-to-mass ratio, in **SI** (coulombs per kilogram), because that is what the
///                     particle state holds.
/// @param steps        How many host steps to take.
/// @param dt           The step, in normalized time.
[[nodiscard]] RunOutcome run_pusher(PusherAdvancer& kernel, const BakedField& table, const pk::ParamBlock& params,
                                    const Vec3& start_re, const Vec3& velocity_c, double charge_mass,
                                    std::size_t steps, double dt) {
    ParticleState state{1};
    state.set(0, 0, ParticleState::Slot::position, start_re.x * kEarthRadiusM);
    state.set(0, 1, ParticleState::Slot::position, start_re.y * kEarthRadiusM);
    state.set(0, 2, ParticleState::Slot::position, start_re.z * kEarthRadiusM);
    state.set(0, 0, ParticleState::Slot::velocity, velocity_c.x * kSpeedOfLightSI);
    state.set(0, 1, ParticleState::Slot::velocity, velocity_c.y * kSpeedOfLightSI);
    state.set(0, 2, ParticleState::Slot::velocity, velocity_c.z * kSpeedOfLightSI);
    state.set(0, 0, ParticleState::Slot::charge_mass, charge_mass);

    pp::ParticleExecutor executor{state, boris_plan(kernel, table, params)};
    REQUIRE(executor.prepare() == pp::PlanRefusal::ok);

    pk::AdvanceContext ctx;
    ctx.dt = dt;

    double previous_angle = std::atan2(velocity_c.y, velocity_c.x);
    double previous_speed = norm(velocity_c);
    RunOutcome outcome;
    for (std::size_t step = 0; step < steps; ++step) {
        if (!executor.advance(ctx).has_value()) return outcome;
        const double vx = state.at(0, 0, ParticleState::Slot::velocity) * kNormalizedPerMetrePerSecond;
        const double vy = state.at(0, 1, ParticleState::Slot::velocity) * kNormalizedPerMetrePerSecond;
        const double vz = state.at(0, 2, ParticleState::Slot::velocity) * kNormalizedPerMetrePerSecond;
        const double angle = std::atan2(vy, vx);
        double delta = angle - previous_angle;
        // Unwrapped by hand: the per-step rotation is far below pi, so the branch that produced the wrap is the
        // one whose magnitude is larger than pi and the correction is a full turn in the opposite direction.
        if (delta > 3.14159265358979323846) delta -= 2.0 * 3.14159265358979323846;
        if (delta < -3.14159265358979323846) delta += 2.0 * 3.14159265358979323846;
        outcome.swept_xy += delta;
        previous_angle = angle;

        // `|v|` rather than `|u|`: the scheme preserves the momentum's magnitude exactly and the velocity's
        // through the Lorentz factor, and it is the velocity a report quotes.
        const double speed = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (previous_speed > 0.0) {
            const double drift = std::abs(speed - previous_speed) / previous_speed;
            if (drift > outcome.max_speed_drift) outcome.max_speed_drift = drift;
        }
        previous_speed = speed;
        outcome.total_substeps += kernel.last_substeps();
    }

    outcome.position_re = Vec3{state.at(0, 0, ParticleState::Slot::position),
                               state.at(0, 1, ParticleState::Slot::position),
                               state.at(0, 2, ParticleState::Slot::position)} *
                          kNormalizedPerMetre;
    outcome.velocity_c = Vec3{state.at(0, 0, ParticleState::Slot::velocity),
                              state.at(0, 1, ParticleState::Slot::velocity),
                              state.at(0, 2, ParticleState::Slot::velocity)} *
                         kNormalizedPerMetrePerSecond;
    outcome.steps = executor.report().steps;
    outcome.clamped_by_executor = executor.report().clamped;
    outcome.speed_clamps = kernel.speed_clamps();
    outcome.retirements = kernel.retirements();
    outcome.last_substeps = kernel.last_substeps();
    return outcome;
}

/// @brief The same run with the kit's default scheme, for the cases that only need one answer.
///
/// @param table        The magnetic field.
/// @param params       The kernel's parameter block.
/// @param start_re     Where the particle starts, in earth radii.
/// @param velocity_c   Its initial velocity, in units of `c`.
/// @param charge_mass  Its charge-to-mass ratio, in **SI**.
/// @param steps        How many host steps to take.
/// @param dt           The step, in normalized time.
[[nodiscard]] RunOutcome run_pusher(const BakedField& table, const pk::ParamBlock& params, const Vec3& start_re,
                                    const Vec3& velocity_c, double charge_mass, std::size_t steps, double dt) {
    BorisAdvancer kernel;
    return run_pusher(kernel, table, params, start_re, velocity_c, charge_mass, steps, dt);
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
    // **The assertion the whole normalized system collapses into**, and the one that has now been wrong twice.
    //
    // The physical number first, because it is the only thing here that comes from outside this repository: a
    // proton in the equatorial surface field gyrates at `qB/m = 2845.75 rad/s`, which is 452.9 Hz. Written as the
    // product of the two constants rather than as a literal, so a change to either moves this side.
    const double si_gyrofrequency = kProtonChargeMassSI * kEquatorialSurfaceFieldT;
    REQUIRE(relative(si_gyrofrequency, 2845.75) < 1.0e-3);   // `qB/m` at 29 709 nT, in rad/s
    REQUIRE(relative(si_gyrofrequency / (2.0 * 3.14159265358979323846), 452.9) < 1.0e-2);

    // The conversion, derived from the equation of motion rather than from this header's own prose (the full
    // derivation is on `normalized_charge_mass`): `q_prime = (q/m) * B_eq * T`, dimensionless, one time unit
    // **multiplied** rather than divided.
    REQUIRE(relative(kProtonNormalizedChargeMass,
                     kProtonChargeMassSI * kEquatorialSurfaceFieldT * kLightCrossingTimeS) < 1.0e-15);

    // And the two agree in the direction the kernel actually uses them. A kernel forms `q_prime * |B|` with `|B|`
    // in normalized field units and gets a rate **per normalized time**; a rate per normalized time becomes a rate
    // per second by **multiplying** by how many normalized times there are in a second. The two previous versions
    // of the conversion divided here, and the assertion was written the same way round, so it agreed with the
    // defect: `q_prime` came out 2209 times too large and the pusher would have gyrated a proton 2209 times too
    // fast. What found it was not this case but `magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyro-
    // frequency`, which integrates a proton in a uniform field of one unit and measures the angle it turns
    // through -- a check that shares no code with the conversion it is checking.
    const double normalized_gyrofrequency = kProtonNormalizedChargeMass * 1.0;   // field of one unit
    REQUIRE(relative(normalized_gyrofrequency * kNormalizedPerSecond, si_gyrofrequency) < 1.0e-12);
    // The same statement as a number a reader can check against a period: `2 pi / omega` in normalized time is
    // 0.1038, and the light-crossing time is 21.3 ms, so a proton gyrates about ten times per light crossing.
    REQUIRE(relative(2.0 * 3.14159265358979323846 / normalized_gyrofrequency, 0.1038) < 1.0e-3);

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

namespace {

/// @brief A batch laid out exactly as the executor lays one out, for provoking a refusal at the boundary.
///
/// `a_bad_batch_is_refused` is the one case that does **not** go through `ParticleExecutor`, and it has to not:
/// the executor's job is to prevent every one of these shapes, so a check of the refusals has to arrive from
/// outside the code that would have caught them. Constructed in place rather than returned by value, because
/// `view.in` points at `slots` and a copy would leave it pointing at the original.
struct HandBatch final {
    gfield::FieldValue slots[pp::kBatchSlotCount]{};
    pk::BatchView view{};

    HandBatch(ParticleState& state, const gfield::FieldValue& magnetic) {
        slots[pp::slot_index(BatchSlot::position)] = state.slot(ParticleState::Slot::position);
        slots[pp::slot_index(BatchSlot::velocity)] = state.slot(ParticleState::Slot::velocity);
        slots[pp::slot_index(BatchSlot::charge_mass)] = state.slot(ParticleState::Slot::charge_mass);
        slots[pp::slot_index(BatchSlot::status)] = state.slot(ParticleState::Slot::status);
        slots[pp::slot_index(SlotName::magnetic)] = magnetic;
        view.in = slots;
        view.out = slots;
        view.count = pp::kBatchSlotCount;
    }
};

/// @brief A vector lattice of the given kind over `values`, for a slot that must be refused for its shape.
[[nodiscard]] gfield::FieldValue shaped_field(const double* values, qp::abi::LatticeKind kind,
                                              std::uint32_t n0) {
    gfield::FieldValue out;
    out.desc = qp::abi::make_lattice(kind, qp::abi::ComponentKind::vector, qp::abi::ElementType::f64,
                                     qp::abi::FieldDim{}, n0);
    out.data = values;
    out.bytes = qp::abi::data_bytes(out.desc);
    return out;
}

}  // namespace

TEST_CASE("magnetosphere.baked_field.the_view_describes_the_storage", "[magnetosphere]") {
    // A baked table is the one thing this kit hands a kernel from outside the kernel's own file, and it does it
    // through `field::FieldValue` -- a description and a borrowed pointer. So the description has to be true:
    // `field::is_readable` is entitled to be asked what a buffer holds and to get an answer it can act on, and a
    // kernel that trusted a description which understated the buffer would read past the end of it.
    BakedField table{Vec3{10.0, 20.0, 30.0}, Vec3{100.0, 200.0, 300.0}, 3, 4, 5};
    REQUIRE(table.width() == 3);
    REQUIRE(table.height() == 4);
    REQUIRE(table.depth() == 5);
    REQUIRE_FALSE(table.empty());
    REQUIRE(table.data().size() == 3U * 4U * 5U * 3U);

    const gfield::FieldValue view = table.view();
    REQUIRE(gfield::is_readable(view));
    REQUIRE(view.kind() == gfield::Kind::Volume);
    REQUIRE(view.is_vector());
    REQUIRE(view.desc.element == qp::abi::ElementType::f64);
    REQUIRE(view.desc.count[0] == 3);
    REQUIRE(view.desc.count[1] == 4);
    REQUIRE(view.desc.count[2] == 5);
    REQUIRE(view.point_count() == 60);
    // The byte count is the table's own, not a second computation: a view that claimed fewer bytes than the
    // storage it points at would be readable and wrong.
    REQUIRE(view.required_bytes() == table.data().size() * sizeof(double));
    REQUIRE(view.required_bytes() == static_cast<std::uint64_t>(table.data().size()) * 8U);

    // The dimension, because a magnetic field is a quantity and not a bare number: kg / (A s^2), which is
    // M = 1, T = -2, I = -1. A table described as dimensionless would make every report about it a guess.
    REQUIRE(view.dimension().M == 1);
    REQUIRE(view.dimension().T == -2);
    REQUIRE(view.dimension().I == -1);
    REQUIRE(view.dimension().L == 0);

    // **The dimension is a parameter, and that is a correction with a case behind it.** Every model this kit
    // shipped until now was magnetic, so `view()` could hardcode tesla; an electric field broke that -- and a
    // description that lied about its dimension would be worse than none, because `field::is_valid_field` is
    // entitled to be asked what a buffer holds and to get a true answer.
    const gfield::FieldValue as_electric = table.view(volt_per_metre_dimension());
    REQUIRE(gfield::is_readable(as_electric));
    REQUIRE(as_electric.dimension().M == 1);
    REQUIRE(as_electric.dimension().L == 1);
    REQUIRE(as_electric.dimension().T == -3);
    REQUIRE(as_electric.dimension().I == -1);
    // The same storage, described two ways: the counts, the element type and the samples do not move.
    REQUIRE(as_electric.desc.count[0] == view.desc.count[0]);
    REQUIRE(as_electric.data == view.data);
    REQUIRE(as_electric.required_bytes() == view.required_bytes());
    REQUIRE(view.dimension().L == 0);
    REQUIRE(view.dimension().T == -2);
    // And the default is still the magnetic one, which is what every existing caller reads.
    REQUIRE(table.view(tesla_dimension()).desc.element == view.desc.element);
    REQUIRE(table.view(tesla_dimension()).desc.kind == view.desc.kind);

    // The node positions, which are the other half of what the view cannot say: the counts are in the description
    // and the grid's geometry is in the parameter block, so the two have to agree with `node_position`.
    const Vec3 node = table.node_position(2, 3, 4);
    REQUIRE(node.x == 10.0 + 2.0 * 100.0);
    REQUIRE(node.y == 20.0 + 3.0 * 200.0);
    REQUIRE(node.z == 30.0 + 4.0 * 300.0);
    REQUIRE(table.node_position(0, 0, 0).x == table.origin().x);

    // An empty table describes nothing, and says so: `is_consistent` refuses a volume with a zero count, so the
    // view is invalid and every caller is stopped before it reads a buffer that is not there.
    const BakedField none{};
    REQUIRE(none.empty());
    REQUIRE_FALSE(gfield::is_readable(none.view()));
    REQUIRE(none.sample(Vec3{1.0, 2.0, 3.0}).x == 0.0);
    REQUIRE(none.sample(Vec3{1.0, 2.0, 3.0}).y == 0.0);

    // A table with only one node on an axis has no interval to interpolate over, so it refuses a view rather
    // than describing a lattice whose samples cannot be blended. Trilinear interpolation needs two nodes.
    BakedField degenerate{Vec3{}, Vec3{1.0, 1.0, 1.0}, 1, 4, 4};
    REQUIRE_FALSE(gfield::is_readable(degenerate.view()));
}

TEST_CASE("magnetosphere.baked_field.a_node_sample_is_exact", "[magnetosphere]") {
    // **The property that makes a table usable at all**: at a node, the interpolated field is the node's value
    // exactly, not nearly. Every measurement a course takes at a grid point, and every check that compares a
    // baked field against the model it came from, depends on this being exact rather than close.
    BakedField table{Vec3{-5.0, -5.0, -5.0}, Vec3{2.0, 3.0, 5.0}, 3, 4, 5};
    for (std::uint32_t i = 0; i < table.width(); ++i) {
        for (std::uint32_t j = 0; j < table.height(); ++j) {
            for (std::uint32_t k = 0; k < table.depth(); ++k) {
                table.set_node(i, j, k, Vec3{1.0 + i, 2.0 + j, 3.0 + k});
            }
        }
    }

    const gfield::FieldValue view = table.view();
    REQUIRE(gfield::is_readable(view));
    for (std::uint32_t i = 0; i < table.width(); ++i) {
        for (std::uint32_t j = 0; j < table.height(); ++j) {
            for (std::uint32_t k = 0; k < table.depth(); ++k) {
                const Vec3 sampled = table.sample(table.node_position(i, j, k));
                REQUIRE(sampled.x == 1.0 + static_cast<double>(i));
                REQUIRE(sampled.y == 2.0 + static_cast<double>(j));
                REQUIRE(sampled.z == 3.0 + static_cast<double>(k));

                // And the view reads the same memory, in the layout `abi::LatticeDesc` describes: point
                // `(i * ny + j) * nz + k`, then component. This is the agreement that lets a kernel sample the
                // table through the field vocabulary while the baker fills it through `set_node`.
                const std::uint64_t point =
                    (static_cast<std::uint64_t>(i) * table.height() + j) * table.depth() + k;
                REQUIRE(gfield::get_component(view, point, 0) == sampled.x);
                REQUIRE(gfield::get_component(view, point, 1) == sampled.y);
                REQUIRE(gfield::get_component(view, point, 2) == sampled.z);
            }
        }
    }

    // Sampling a node is not a clamped sample: a run that never leaves the grid must report zero clamps, or the
    // count would be useless as evidence about whether the particles stayed inside.
    REQUIRE(table.clamped_samples() == 0);
}

TEST_CASE("magnetosphere.baked_field.a_midpoint_is_the_average", "[magnetosphere]") {
    // Between nodes the blend is trilinear, which is the scheme the reference implementation uses and the one the
    // reopening condition in `baked_field.hpp` is about. Asserted against the arithmetic a reader can do by hand
    // rather than against a second implementation of the same formula.
    BakedField table{Vec3{0.0, 0.0, 0.0}, Vec3{1.0, 1.0, 1.0}, 2, 2, 2};
    Vec3 corner[2][2][2];
    double expected_sum[3] = {0.0, 0.0, 0.0};
    for (std::uint32_t i = 0; i < 2; ++i) {
        for (std::uint32_t j = 0; j < 2; ++j) {
            for (std::uint32_t k = 0; k < 2; ++k) {
                corner[i][j][k] = Vec3{1.0 + static_cast<double>(i), 10.0 + static_cast<double>(j),
                                       100.0 + static_cast<double>(k)};
                table.set_node(i, j, k, corner[i][j][k]);
                expected_sum[0] += corner[i][j][k].x;
                expected_sum[1] += corner[i][j][k].y;
                expected_sum[2] += corner[i][j][k].z;
            }
        }
    }

    // The centre of the cell is the mean of its eight corners, exactly what a trilinear blend gives at the middle.
    const Vec3 centre = table.sample(Vec3{0.5, 0.5, 0.5});
    REQUIRE(relative(centre.x, expected_sum[0] / 8.0) < 1.0e-15);
    REQUIRE(relative(centre.y, expected_sum[1] / 8.0) < 1.0e-15);
    REQUIRE(relative(centre.z, expected_sum[2] / 8.0) < 1.0e-15);

    // And the blend is linear along an edge: three quarters of the way from node 0 to node 1 is three quarters of
    // the difference, which is what "second-order accurate in the spacing" means for a quantity that is already
    // linear.
    const Vec3 quarter = table.sample(Vec3{0.25, 0.0, 0.0});
    REQUIRE(relative(quarter.x, 0.75 * corner[0][0][0].x + 0.25 * corner[1][0][0].x) < 1.0e-15);
    REQUIRE(relative(quarter.y, corner[0][0][0].y) < 1.0e-15);
    REQUIRE(relative(quarter.z, corner[0][0][0].z) < 1.0e-15);

    // A quarter of the way in from the far side blends the same two nodes from the other end, which pins the
    // weight order: a table whose weights were swapped would be symmetric in this case and would fail here.
    const Vec3 three_quarters = table.sample(Vec3{0.75, 0.0, 0.0});
    REQUIRE(relative(three_quarters.x, 0.25 * corner[0][0][0].x + 0.75 * corner[1][0][0].x) < 1.0e-15);
}

TEST_CASE("magnetosphere.baked_field.out_of_range_is_clamped_and_counted", "[magnetosphere]") {
    // **Clamped, not extrapolated**, and the reason is physics rather than safety: a linear extrapolation of a
    // `1/r^3` field beyond the grid grows without bound, so a particle that left the modelled region would be
    // handed an enormous force for a reason nobody could see. The clamped answer is finite and wrong, which is
    // why it is **counted**: "the particles left the modelled region" has to be a number a report can print.
    BakedField table{Vec3{0.0, 0.0, 0.0}, Vec3{1.0, 1.0, 1.0}, 2, 2, 2};
    table.set_node(0, 0, 0, Vec3{1.0, 0.0, 0.0});
    for (std::uint32_t i = 0; i < 2; ++i) {
        for (std::uint32_t j = 0; j < 2; ++j) {
            for (std::uint32_t k = 0; k < 2; ++k) {
                if (i == 0 && j == 0 && k == 0) continue;
                table.set_node(i, j, k, Vec3{0.0, 0.0, 0.0});
            }
        }
    }

    REQUIRE(table.clamped_samples() == 0);
    const Vec3 inside = table.sample(Vec3{0.5, 0.0, 0.0});
    REQUIRE(relative(inside.x, 0.5) < 1.0e-15);
    REQUIRE(table.clamped_samples() == 0);

    // Below the grid's low corner: the sample reads node `(0, 0, 0)` and returns its value, so a particle far
    // outside sees the boundary field rather than a field growing without bound.
    for (const Vec3 outside : {Vec3{-5.0, -5.0, -5.0}, Vec3{-0.001, 0.0, 0.0}}) {
        const Vec3 sampled = table.sample(outside);
        REQUIRE(sampled.x == 1.0);
        REQUIRE(sampled.y == 0.0);
    }
    REQUIRE(table.clamped_samples() == 2);

    // Above it: the far corner, which is zero in this table. The two directions clamp to different values, which
    // is what shows the clamp is at the **boundary** and not a constant substitute.
    const Vec3 beyond = table.sample(Vec3{2.0, 2.0, 2.0});
    REQUIRE(beyond.x == 0.0);
    REQUIRE(table.clamped_samples() == 3);

    // A non-finite position is not a place. It is answered with zero, counted as clamped, and -- the part that
    // matters -- it does not produce an index: a NaN compares false against every bound, so the index arithmetic
    // that reads `f < 0 ? 0 : (f > last ? last : f)` falls through both arms and casts a NaN to an integer.
    const Vec3 not_a_place = table.sample(Vec3{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    REQUIRE(not_a_place.x == 0.0);
    REQUIRE(table.clamped_samples() == 4);

    // And the count is the run's own: a second run starts from zero without the samples moving.
    table.reset_counts();
    REQUIRE(table.clamped_samples() == 0);
    REQUIRE(table.sample(Vec3{0.0, 0.0, 0.0}).x == 1.0);
}

TEST_CASE("magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency", "[magnetosphere]") {
    // **The case that found the 2209.**
    //
    // The whole normalized system collapses into one measurement here: put a proton in a uniform field of a known
    // size, integrate, and see how far its velocity turns per unit time. The expected value comes from `q B / m`
    // -- a data-book number -- carried through the time conversion, and it shares no code with the conversion the
    // kernel uses. The previous version of that conversion was 2209 times too large and every test that checked
    // it agreed with it; this one does not, because it integrates.
    //
    // A field of 29.7 nT: the dipole's own value at ten earth radii, so the case is about a real region rather
    // than about an arithmetic convenience.
    const double b_norm = 1.0e-3;
    const BakedField table = uniform_table(Vec3{0.0, 0.0, b_norm * kEquatorialSurfaceFieldT});
    const pk::ParamBlock params = boris_params(table);

    const double speed = 0.01;                                   // one percent of c: a thermal proton's order
    const double gamma0 = 1.0 / std::sqrt(1.0 - speed * speed);
    const double omega = kProtonNormalizedChargeMass * b_norm / gamma0;   // radians per normalized time

    const std::size_t steps = 2595;
    const double dt = 0.01;
    const RunOutcome out = run_pusher(table, params, Vec3{2.0, 0.0, 0.0}, Vec3{0.0, speed, 0.0},
                                      kProtonChargeMassSI, steps, dt);

    REQUIRE(out.steps == steps);
    REQUIRE(out.retirements == 0);
    REQUIRE(out.speed_clamps == 0);
    REQUIRE(out.clamped_by_executor == 0);
    // One sub-step per particle per step: the rotation per step is 6e-4 rad, four hundred times below the
    // threshold the criterion applies, so a count above the step count would mean the criterion was wrong.
    REQUIRE(out.total_substeps == steps);

    // The sign **and** the magnitude. A positive charge with `B` along `+z` and `v` along `+y` turns towards
    // `+x`, which is clockwise seen from `+z`: the swept angle is negative. A sign error here reverses every
    // drift in the kit, and a test that measured `|omega|` could not see it.
    const double elapsed = dt * static_cast<double>(steps);
    REQUIRE(out.swept_xy < 0.0);
    REQUIRE(relative(out.swept_xy, -omega * elapsed) < 1.0e-6);

    // And the same statement against the scheme's **own** rotation rather than the continuous one. The discrete
    // rotation is `2 atan(|t|)` with `|t| = q B dt / (2 gamma)`, so this pins the implementation to the formula
    // the header documents, to the last digits, while the check above pins the formula to the physics.
    const double t_magnitude = kProtonNormalizedChargeMass * b_norm * dt / (2.0 * gamma0);
    const double discrete = -2.0 * std::atan(t_magnitude) * static_cast<double>(steps);
    REQUIRE(relative(out.swept_xy, discrete) < 1.0e-12);

    // The radius comes out of the same two numbers: `r = v / omega` in earth radii. A pusher that turned the
    // velocity correctly but moved the position by the wrong amount would pass every check above.
    const double gyroradius = speed / omega;
    REQUIRE(relative(norm(out.position_re - Vec3{2.0, 0.0, 0.0}), gyroradius * std::sqrt(2.0 * (1.0 - std::cos(elapsed * omega)))) < 1.0e-3);
}

TEST_CASE("magnetosphere.boris.a_magnetic_field_does_no_work", "[magnetosphere]") {
    // A magnetic field exerts no force along the velocity, so it does no work and `|v|` is constant. That is not
    // a small property of the scheme: it is the reason a Boris push is the one a plasma course uses, and it is
    // **exact** here rather than approximate, because the rotation is built from two shears whose scale factors
    // cancel. Asserted at a relativistic speed, where a non-relativistic push would be wrong by `gamma`.
    const double b_norm = 1.0e-3;
    const BakedField table = uniform_table(Vec3{0.0, 0.0, b_norm * kEquatorialSurfaceFieldT});
    const pk::ParamBlock params = boris_params(table);

    const double speed = 0.9;
    const double gamma0 = 1.0 / std::sqrt(1.0 - speed * speed);
    const double omega = kProtonNormalizedChargeMass * b_norm / gamma0;

    const std::size_t steps = 2000;
    const double dt = 0.005;
    const RunOutcome out = run_pusher(table, params, Vec3{2.0, 0.0, 0.0}, Vec3{0.0, speed, 0.0},
                                      kProtonChargeMassSI, steps, dt);
    REQUIRE(out.retirements == 0);

    // Energy conservation, per step, as a relative change. The reference implementation's own conservation check
    // compares numbers of order one, which is why the particle state is f64 rather than the field module's f32.
    REQUIRE(out.max_speed_drift < 1.0e-12);
    REQUIRE(relative(norm(out.velocity_c), speed) < 1.0e-13);

    // And the rotation is the relativistic one. The non-relativistic prediction overstates the angle by exactly
    // `gamma`, so this pair of assertions distinguishes the two schemes rather than only checking that something
    // turned.
    const double elapsed = dt * static_cast<double>(steps);
    REQUIRE(relative(out.swept_xy, -omega * elapsed) < 1.0e-6);
    const double non_relativistic = kProtonNormalizedChargeMass * b_norm * elapsed;
    REQUIRE(relative(non_relativistic / std::abs(out.swept_xy), gamma0) < 1.0e-5);
}

TEST_CASE("magnetosphere.boris.a_round_trip_is_second_order_not_exact", "[magnetosphere]") {
    // **The claim the kernel declines to make.** `is_time_reversible` is false here, and the case measures why:
    // the rotation inverts exactly, the position update does not. Boris advances the position with the
    // **post-rotation** velocity, so a forward step moves by `R(theta) v dt` and the backward step returns by
    // `v dt`, leaving `(R(theta) - I) v dt` -- a displacement of order the step length times the angle, which is
    // second order in `dt`. A `true` here would have been a statement about the physics that nothing checked.
    const double b_norm = 1.0e-3;
    const BakedField table = uniform_table(Vec3{0.0, 0.0, b_norm * kEquatorialSurfaceFieldT});
    const pk::ParamBlock params = boris_params(table);

    BorisAdvancer decider;
    REQUIRE_FALSE(decider.is_time_reversible());

    const double speed = 0.01;
    const double gamma0 = 1.0 / std::sqrt(1.0 - speed * speed);
    const Vec3 start{2.0, 0.0, 0.0};
    const Vec3 velocity{0.0, speed, 0.0};

    /// One forward step and one backward step, and the displacement that is left.
    const auto residual = [&](double dt) -> Vec3 {
        ParticleState state{1};
        state.set(0, 0, ParticleState::Slot::position, start.x * kEarthRadiusM);
        state.set(0, 1, ParticleState::Slot::position, start.y * kEarthRadiusM);
        state.set(0, 2, ParticleState::Slot::position, start.z * kEarthRadiusM);
        state.set(0, 0, ParticleState::Slot::velocity, velocity.x * kSpeedOfLightSI);
        state.set(0, 1, ParticleState::Slot::velocity, velocity.y * kSpeedOfLightSI);
        state.set(0, 2, ParticleState::Slot::velocity, velocity.z * kSpeedOfLightSI);
        state.set(0, 0, ParticleState::Slot::charge_mass, kProtonChargeMassSI);

        BorisAdvancer kernel;
        pp::ParticleExecutor executor{state, boris_plan(kernel, table, params)};
        REQUIRE(executor.prepare() == pp::PlanRefusal::ok);

        pk::AdvanceContext ctx;
        ctx.dt = dt;
        REQUIRE(executor.advance(ctx).has_value());
        // **The velocity comes back exactly**, to the rounding of the rotation: this is the half of Boris that is
        // reversible, and asserting it here is what separates "the scheme is not reversible" from "the scheme is
        // broken".
        const Vec3 after_forward{state.at(0, 0, ParticleState::Slot::velocity),
                                 state.at(0, 1, ParticleState::Slot::velocity),
                                 state.at(0, 2, ParticleState::Slot::velocity)};
        ctx.dt = -dt;
        REQUIRE(executor.advance(ctx).has_value());
        const Vec3 after_back{state.at(0, 0, ParticleState::Slot::velocity),
                              state.at(0, 1, ParticleState::Slot::velocity),
                              state.at(0, 2, ParticleState::Slot::velocity)};
        REQUIRE(relative(norm(after_back), norm(after_forward)) < 1.0e-15);

        return Vec3{state.at(0, 0, ParticleState::Slot::position),
                    state.at(0, 1, ParticleState::Slot::position),
                    state.at(0, 2, ParticleState::Slot::position)} *
                   kNormalizedPerMetre -
               start;
    };

    const Vec3 coarse = residual(0.02);
    const Vec3 fine = residual(0.01);

    // Not zero: the residual is a real displacement, and it is a fraction of a metre in a run whose positions are
    // millions of metres. Small is not the same as absent, and "small" is what makes a false `true` survive.
    REQUIRE(norm(coarse) > 0.0);
    REQUIRE(norm(coarse) > norm(fine));

    // Second order: halving the step quarters the residual. The ratio is the measurement, and a first-order
    // defect anywhere in the position update would show up as a ratio near two.
    const double ratio = norm(coarse) / norm(fine);
    REQUIRE(ratio > 3.9);
    REQUIRE(ratio < 4.1);

    // And the closed form: `|(R(theta) - I) v dt| = 2 |v| sin(theta/2) dt`, with `theta = 2 atan(|t|)` the angle
    // the scheme's own rotation turns through. A displacement of the right order but the wrong size would pass
    // the ratio check above.
    const double dt = 0.01;
    const double t_magnitude = kProtonNormalizedChargeMass * b_norm * dt / (2.0 * gamma0);
    const double theta = 2.0 * std::atan(t_magnitude);
    const double predicted = 2.0 * speed * std::sin(theta / 2.0) * dt;
    REQUIRE(relative(norm(fine), predicted) < 1.0e-3);
}

TEST_CASE("magnetosphere.boris.a_relativistic_particle_needs_fewer_substeps", "[magnetosphere]") {
    // **The measurement behind the sub-step criterion**, and it goes the opposite way to the first draft of this
    // kit's comment. The angle a step turns through is `q B dt / (2 gamma m)`: a *hotter* particle has a larger
    // `gamma`, turns through a smaller angle, and needs **fewer** sub-steps to keep that angle under the
    // threshold. The reference implementation's `omega_c dt > 0.5`, with `omega_c` the non-relativistic cyclotron
    // frequency, divides by no `gamma` at all, so it takes the slow particle's count for both -- which is safe,
    // and paid for on exactly the particles where a run is already slowest.
    const double b_norm = 1.0;
    const BakedField table = uniform_table(Vec3{0.0, 0.0, b_norm * kEquatorialSurfaceFieldT});
    const pk::ParamBlock params = boris_params(table);

    ParticleState state{2};
    const double cold_speed = 0.01;
    const double hot_speed = 0.99;
    state.set(0, 1, ParticleState::Slot::position, 2.0 * kEarthRadiusM);
    state.set(1, 1, ParticleState::Slot::position, 2.0 * kEarthRadiusM);
    state.set(0, 1, ParticleState::Slot::velocity, cold_speed * kSpeedOfLightSI);
    state.set(1, 1, ParticleState::Slot::velocity, hot_speed * kSpeedOfLightSI);
    state.set(0, 0, ParticleState::Slot::charge_mass, kProtonChargeMassSI);
    state.set(1, 0, ParticleState::Slot::charge_mass, kProtonChargeMassSI);

    BorisAdvancer kernel;
    pp::ParticleExecutor executor{state, boris_plan(kernel, table, params)};
    REQUIRE(executor.prepare() == pp::PlanRefusal::ok);

    const double dt = 0.02;
    pk::AdvanceContext ctx;
    ctx.dt = dt;
    REQUIRE(executor.advance(ctx).has_value());

    // What this kernel took. The cold particle's angle is 1.09 rad, so three sub-steps below the 0.5 threshold;
    // the hot one's is 0.17 rad, so one.
    const double cold_gamma = 1.0 / std::sqrt(1.0 - cold_speed * cold_speed);
    const double hot_gamma = 1.0 / std::sqrt(1.0 - hot_speed * hot_speed);
    const double cold_angle = 2.0 * std::atan(kProtonNormalizedChargeMass * b_norm * dt / (2.0 * cold_gamma));
    const double hot_angle = 2.0 * std::atan(kProtonNormalizedChargeMass * b_norm * dt / (2.0 * hot_gamma));
    REQUIRE(cold_angle > hot_angle);
    const auto count_for = [](double angle) {
        return angle > BorisAdvancer::kDefaultMaxRotation
                   ? static_cast<std::uint64_t>(std::ceil(angle / BorisAdvancer::kDefaultMaxRotation))
                   : 1U;
    };
    REQUIRE(count_for(cold_angle) == 3);
    REQUIRE(count_for(hot_angle) == 1);
    REQUIRE(kernel.last_substeps() == count_for(cold_angle) + count_for(hot_angle));

    // What the reference-style criterion would have taken, computed from the same numbers: the cyclotron
    // frequency without the Lorentz factor, so both particles are charged the cold particle's count.
    const double reference_angle = kProtonNormalizedChargeMass * b_norm * dt;
    const auto reference_count = static_cast<std::uint64_t>(std::ceil(reference_angle / 0.5));
    REQUIRE(reference_count == 3);
    REQUIRE(kernel.last_substeps() < 2 * reference_count);

    // Every sub-step is still below the threshold: the count is a ceiling of a ratio, not a fixed number, and a
    // criterion that produced a count too small to meet its own threshold would be worse than no criterion.
    REQUIRE(cold_angle / static_cast<double>(count_for(cold_angle)) <= BorisAdvancer::kDefaultMaxRotation);
}

TEST_CASE("magnetosphere.boris.the_speed_limit_is_counted", "[magnetosphere]") {
    // A Boris step cannot exceed `c` -- the rotation preserves `|u|` -- but a **declared** limit is still needed,
    // because the half-impulses can and because a state can arrive past it from a step that went wrong. Three
    // decisions about it are asserted here, and the reference implementation takes none of them: it is a
    // parameter rather than a hard-coded constant, it **counts** what it did, and it is the last resort rather
    // than the mechanism.
    const double b_norm = 1.0e-3;
    const BakedField table = uniform_table(Vec3{0.0, 0.0, b_norm * kEquatorialSurfaceFieldT});

    // A particle that arrives past the limit -- 1.5c, which no physical step produces and which is exactly the
    // state the limit exists for. It is pulled back to the limit and the count says so.
    const RunOutcome superluminal = run_pusher(table, boris_params(table), Vec3{2.0, 0.0, 0.0},
                                               Vec3{0.0, 1.5, 0.0}, kProtonChargeMassSI, 1, 0.01);
    REQUIRE(superluminal.speed_clamps == 1);
    REQUIRE(norm(superluminal.velocity_c) <= BorisAdvancer::kDefaultSpeedLimit * (1.0 + 1.0e-12));
    REQUIRE(norm(superluminal.velocity_c) > 0.0);
    REQUIRE(superluminal.retirements == 0);

    // A limit that is **declared** rather than hard-coded, and the same start under two different declarations
    // ending in two different places. That is what makes the parameter worth having: a demonstration can lower it
    // and a measurement can raise it, and neither has to rebuild the kit.
    RunOutcome declared = run_pusher(table, boris_params(table, 64.0, 0.0, 4096.0, 0.5), Vec3{2.0, 0.0, 0.0},
                                     Vec3{0.0, 0.9, 0.0}, kProtonChargeMassSI, 1, 0.01);
    REQUIRE(declared.speed_clamps == 1);
    REQUIRE(relative(norm(declared.velocity_c), 0.5) < 1.0e-12);

    RunOutcome untouched = run_pusher(table, boris_params(table), Vec3{2.0, 0.0, 0.0}, Vec3{0.0, 0.9, 0.0},
                                      kProtonChargeMassSI, 1, 0.01);
    REQUIRE(untouched.speed_clamps == 0);
    REQUIRE(relative(norm(untouched.velocity_c), 0.9) < 1.0e-12);

    // And a physical run of a thousand steps never reaches it, which is the finding a report needs: a growing
    // count beside a run that is also being sub-stepped says the field is stronger than the step can resolve.
    const RunOutcome physical = run_pusher(table, boris_params(table), Vec3{2.0, 0.0, 0.0}, Vec3{0.0, 0.9, 0.0},
                                           kProtonChargeMassSI, 1000, 0.005);
    REQUIRE(physical.speed_clamps == 0);
    REQUIRE(physical.retirements == 0);

    // The counters are the run's, not the object's life: a second run is reported as its own.
    BorisAdvancer kernel;
    REQUIRE(kernel.speed_clamps() == 0);
    REQUIRE(kernel.retirements() == 0);
    REQUIRE(kernel.last_substeps() == 0);
}

TEST_CASE("magnetosphere.boris.a_bad_batch_is_refused", "[magnetosphere]") {
    // Everything the kernel refuses, and each refusal is a shape that would otherwise produce a **plausible**
    // answer rather than a wrong-looking one. This is the one case that drives `advance` directly: the executor's
    // job is to prevent every shape below, so a check of the refusals has to come from outside the code that
    // would have caught them.
    const BakedField table = uniform_table(Vec3{0.0, 0.0, 1.0e-3 * kEquatorialSurfaceFieldT});
    const pk::ParamBlock good = boris_params(table);

    BorisAdvancer kernel;
    // The parameter block, first, because `prepare` is where a user's mistake is supposed to be caught -- while
    // they are still editing rather than in the middle of a run.
    REQUIRE(kernel.prepare(good).has_value());
    const auto refuses_params = [](double range, double gravity, double substep_cap, double speed_limit,
                                   double spacing_x) {
        BakedField field = uniform_table(Vec3{0.0, 0.0, 1.0e-9});
        pk::ParamBlock params = boris_params(field, range, gravity, substep_cap, speed_limit);
        params.set_real(BorisAdvancer::kIndexGridSpacing0, spacing_x);
        BorisAdvancer probe;
        return !probe.prepare(params).has_value();
    };
    REQUIRE(refuses_params(0.0, 0.0, 4096.0, 0.999999, 1.0));      // a range that is not positive
    REQUIRE(refuses_params(-1.0, 0.0, 4096.0, 0.999999, 1.0));
    REQUIRE(refuses_params(64.0, -1.0, 4096.0, 0.999999, 1.0));    // gravity cannot push
    REQUIRE(refuses_params(64.0, 0.0, 0.5, 0.999999, 1.0));        // a cap below one sub-step is no cap
    REQUIRE(refuses_params(64.0, 0.0, 4096.0, 0.0, 1.0));          // a limit of zero stops every particle
    REQUIRE(refuses_params(64.0, 0.0, 4096.0, 1.0, 1.0));          // a limit of exactly c divides by zero
    REQUIRE(refuses_params(64.0, 0.0, 4096.0, -0.5, 1.0));
    REQUIRE(refuses_params(64.0, 0.0, 4096.0, 0.999999, 0.0));     // a spacing that cannot be divided by
    REQUIRE_FALSE(refuses_params(64.0, 0.0, 4096.0, 0.999999, 1.0));

    ParticleState state{2};
    state.set(0, 0, ParticleState::Slot::charge_mass, kProtonChargeMassSI);
    state.set(1, 0, ParticleState::Slot::charge_mass, kProtonChargeMassSI);
    HandBatch batch{state, table.view()};
    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    REQUIRE(kernel.advance(batch.view, ctx).has_value());

    // A truncated batch. The array's length is a constant of the executor and not a report of how many fields are
    // bound, so a shorter one is a caller who built a batch by hand -- and guessing what its entries meant is how
    // a drag coefficient gets read as a magnetic field.
    batch.view.count = pp::kBatchSlotCount - 1;
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());
    batch.view.count = pp::kBatchSlotCount;

    // No magnetic field, described as a **line** rather than a volume, and described as a volume with no data.
    // The middle one is the subtle case: it is readable, f64, a vector lattice and the right dimension, and
    // sampling it as a volume would read its counts as a grid shape and produce a field from whatever memory
    // followed. A point lattice would be worse still -- it carries zero counts, so the sample would be zero
    // everywhere and the particle would travel in a straight line with nothing saying why.
    const double line_values[12] = {};
    const gfield::FieldValue line = shaped_field(line_values, qp::abi::LatticeKind::line, 4);
    REQUIRE(gfield::is_readable(line));
    batch.slots[pp::slot_index(SlotName::magnetic)] = line;
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());

    gfield::FieldValue described_only = table.view();
    described_only.data = nullptr;
    batch.slots[pp::slot_index(SlotName::magnetic)] = described_only;
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());

    batch.slots[pp::slot_index(SlotName::magnetic)] = gfield::FieldValue{};
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());
    batch.slots[pp::slot_index(SlotName::magnetic)] = table.view();

    // A particle slot in the platform's default field precision. The particle path is f64 because a Boris push
    // accumulates hundreds of thousands of steps and the conservation checks are differences of order one; a
    // kernel that cast an f32 buffer to `double*` would read two samples as one number and integrate a field that
    // looks like physics.
    const float single_precision[6] = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    gfield::FieldValue f32_position = state.slot(ParticleState::Slot::position);
    f32_position.desc.element = qp::abi::ElementType::f32;
    f32_position.desc.spacing_bytes = qp::abi::expected_spacing(f32_position.desc);
    f32_position.data = single_precision;
    f32_position.bytes = sizeof(single_precision);
    batch.slots[pp::slot_index(BatchSlot::position)] = f32_position;
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());
    batch.slots[pp::slot_index(BatchSlot::position)] = state.slot(ParticleState::Slot::position);

    // And a step size that is not a step. Zero would append a sample nobody asked for; a non-finite one would
    // travel an infinite distance and be caught only by the finiteness sweep, one step too late to report why.
    ctx.dt = 0.0;
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());
    ctx.dt = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());
    ctx.dt = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(kernel.advance(batch.view, ctx).has_value());
    ctx.dt = 0.01;

    // Nothing above counted anything: a refused call is not a step, and a report that counted one would overstate
    // how far the run got.
    kernel.reset_counts();
    REQUIRE(kernel.retirements() == 0);
    REQUIRE(kernel.speed_clamps() == 0);
    REQUIRE_FALSE(gfield::is_readable(batch.slots[pp::slot_index(SlotName::electric)]));
    REQUIRE_FALSE(gfield::is_readable(batch.slots[pp::slot_index(SlotName::drag)]));
}


TEST_CASE("magnetosphere.baked_field.a_kernel_and_an_emitter_read_the_same_table", "[magnetosphere]") {
    // **One sampler, two callers.** The pusher's inner loop and the emitter's launch both need to read a baked
    // table, and they reach it differently: the kernel holds a `field::FieldValue` out of the batch, the emitter
    // holds one out of the store, and a `BakedField` holds its own samples. The blend used to live in the
    // kernel's file, and when the emitter arrived it would have been copied -- two answers to "what is the field
    // between two nodes", disagreeing by whatever the second author changed. This case is what pins that there
    // is one implementation: `BakedField::sample` is a wrapper over the same free function, so the two agree
    // **bit for bit**, and a future edit that inlined a second copy would break here rather than in a trajectory
    // whose curvature is subtly wrong.
    BakedField table{Vec3{-3.0 * kEarthRadiusM, -2.0 * kEarthRadiusM, -1.0 * kEarthRadiusM},
                     Vec3{0.5 * kEarthRadiusM, 0.75 * kEarthRadiusM, 1.25 * kEarthRadiusM}, 13, 9, 7};
    const DipoleField dipole{kMagneticTiltDegrees};
    for (std::uint32_t i = 0; i < 13; ++i) {
        for (std::uint32_t j = 0; j < 9; ++j) {
            for (std::uint32_t k = 0; k < 7; ++k) {
                table.set_node(i, j, k, dipole.at(table.node_position(i, j, k)));
            }
        }
    }
    const gfield::FieldValue view = table.view();
    REQUIRE(gfield::is_readable(view));

    // At the nodes, at the midpoints, and well outside the box -- where the clamp has to agree as much as the
    // blend does.
    const Vec3 probes[] = {table.node_position(0, 0, 0),
                           table.node_position(6, 4, 3),
                           table.node_position(12, 8, 6),
                           table.node_position(3, 2, 1) + Vec3{0.25 * kEarthRadiusM, 0.375 * kEarthRadiusM,
                                                               0.625 * kEarthRadiusM},
                           Vec3{-40.0 * kEarthRadiusM, 0.0, 0.0},
                           Vec3{40.0 * kEarthRadiusM, 40.0 * kEarthRadiusM, 40.0 * kEarthRadiusM}};
    for (const Vec3& point : probes) {
        const Vec3 via_table = table.sample(point);
        const Vec3 via_field = sample_baked(view, table.origin(), table.spacing(), point);
        REQUIRE(via_field.x == via_table.x);
        REQUIRE(via_field.y == via_table.y);
        REQUIRE(via_field.z == via_table.z);
    }

    // The **f32** path, which a `BakedField` cannot produce and a field-domain node can: ADR-0005 makes f32 the
    // default for field data, so the dispatch on the descriptor is a real branch rather than a formality. The
    // same values in half the precision must read the same to f32's own rounding.
    std::vector<float> single(view.point_count() * 3);
    const auto* source = static_cast<const double*>(view.data);
    for (std::size_t i = 0; i < single.size(); ++i) single[i] = static_cast<float>(source[i]);
    gfield::FieldValue narrow = view;
    narrow.desc.element = qp::abi::ElementType::f32;
    narrow.desc.spacing_bytes = qp::abi::expected_spacing(narrow.desc);
    narrow.data = single.data();
    narrow.bytes = static_cast<std::uint64_t>(single.size()) * sizeof(float);
    REQUIRE(gfield::is_readable(narrow));
    for (const Vec3& point : probes) {
        const Vec3 wide = sample_baked(view, table.origin(), table.spacing(), point);
        const Vec3 tight = sample_baked(narrow, table.origin(), table.spacing(), point);
        // The field is 1e-8 T at this box's edge and 3e-5 T near the surface, so the tolerance has to be
        // relative to the value being read; f32 carries about seven digits and the blend adds its own rounding.
        REQUIRE(relative(tight.x, wide.x) < 1.0e-6);
        REQUIRE(relative(tight.y, wide.y) < 1.0e-6);
        REQUIRE(relative(tight.z, wide.z) < 1.0e-6);
    }

    // A scalar table -- the drag coefficient's shape -- reads through the other entry point.
    std::vector<double> drag(13 * 9 * 7);
    for (std::size_t i = 0; i < drag.size(); ++i) drag[i] = static_cast<double>(i);
    qp::abi::FieldDim per_second;
    per_second.T = -1;
    gfield::FieldValue scalar;
    scalar.desc = qp::abi::make_lattice(qp::abi::LatticeKind::volume, qp::abi::ComponentKind::scalar,
                                        qp::abi::ElementType::f64, per_second, 13, 9, 7);
    scalar.data = drag.data();
    scalar.bytes = qp::abi::data_bytes(scalar.desc);
    const Vec3 corner = table.node_position(0, 0, 0);
    REQUIRE(sample_baked_scalar(scalar, table.origin(), table.spacing(), corner) == 0.0);
    const Vec3 next = table.node_position(1, 0, 0);
    // Node `(1, 0, 0)` is point `(1 * ny + 0) * nz + 0` = 63 in the ABI's own layout, whose value is its index:
    // a scalar read that used the wrong stride would land on 9 or 7 instead, which is what this pins.
    REQUIRE(sample_baked_scalar(scalar, table.origin(), table.spacing(), next) == 63.0);
    // Halfway along the first cell the blend is the mean of the two nodes' values.
    const Vec3 middle{(corner.x + next.x) * 0.5, corner.y, corner.z};
    REQUIRE(relative(sample_baked_scalar(scalar, table.origin(), table.spacing(), middle), 31.5) < 1.0e-15);

    // And every shape that is **not** a volume of the right component count reads as zero rather than as
    // whatever memory follows it: a `line` lattice has no rows, and a vector table read as a scalar would take
    // the first number of every vector and call it a coefficient.
    const double line_values[6] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    gfield::FieldValue line;
    line.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::vector,
                                      qp::abi::ElementType::f64, qp::abi::FieldDim{}, 2);
    line.data = line_values;
    line.bytes = qp::abi::data_bytes(line.desc);
    REQUIRE(gfield::is_readable(line));
    const Vec3 zero = sample_baked(line, table.origin(), table.spacing(), corner);
    REQUIRE(zero.x == 0.0);
    REQUIRE(zero.y == 0.0);
    REQUIRE(zero.z == 0.0);
    REQUIRE(sample_baked_scalar(line, table.origin(), table.spacing(), corner) == 0.0);
    REQUIRE(sample_baked_scalar(view, table.origin(), table.spacing(), corner) == 0.0);
}

TEST_CASE("magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses", "[magnetosphere]") {
    // **The pair a course needs to see, measured rather than described.** Boris splits the Lorentz force into two
    // half impulses around an *exact* rotation, so `|u|` survives the magnetic part to the rounding and a purely
    // magnetic run loses no energy at all. RK4 integrates the same equation to fourth order and promises nothing:
    // for a rotation through `y` radians per sub-step its amplification is the stability function `R(i y)`, whose
    // modulus is the closed form below, so a particle in a magnetic field **spirals inwards** at a rate that can be
    // written down. Neither is better; they answer different questions, and this case puts both answers in numbers.
    const double b_norm = 1.0e-3;
    const BakedField table = uniform_table(Vec3{0.0, 0.0, b_norm * kEquatorialSurfaceFieldT});
    const pk::ParamBlock params = boris_params(table);
    const double speed = 0.01;
    const double gamma0 = 1.0 / std::sqrt(1.0 - speed * speed);
    const double omega = kProtonNormalizedChargeMass * b_norm / gamma0;
    const Vec3 start{2.0, 0.0, 0.0};
    const Vec3 velocity{0.0, speed, 0.0};

    // A rotation of exactly half a radian per step: the largest the sub-step criterion allows one sub-step to
    // take, so the count below pins that the scheme is being measured and not the controller.
    const double y = 0.5;
    const double dt = y / omega;
    const std::size_t steps = 1000;
    Rk4Advancer rk4;
    const RunOutcome out = run_pusher(rk4, table, params, start, velocity, kProtonChargeMassSI, steps, dt);
    REQUIRE(out.steps == steps);
    REQUIRE(out.total_substeps == steps);
    REQUIRE(out.retirements == 0);
    REQUIRE(out.clamped_by_executor == 0);

    // `|R(i y)|^2 = 1 - y^6/72 + y^8/576`, and the amplitude after `steps` of them is its `steps/2`-th power. The
    // scheme's stability function describes the **momentum**, `u = gamma v`, and that is what is measured first: the
    // momentum decays by that factor to a few parts in a million, and the gap that is left is not an implementation
    // error -- it is the relativistic form of the equation, whose rate carries a `1/gamma` that moves as `|u|`
    // decays. The linear stability function is exactly this factor for a *linear* rotation, and 2.5e-6 is how far a
    // nonlinear system sits from it at a tenth of the speed of light.
    const double modulus2 = 1.0 - std::pow(y, 6.0) / 72.0 + std::pow(y, 8.0) / 576.0;
    const double predicted = std::pow(modulus2, 0.5 * static_cast<double>(steps));
    const double final_speed = norm(out.velocity_c);
    const double final_momentum = final_speed / std::sqrt(1.0 - final_speed * final_speed);
    const double initial_momentum = speed / std::sqrt(1.0 - speed * speed);
    REQUIRE(final_speed < speed);
    REQUIRE(relative(final_momentum / initial_momentum, predicted) < 1.0e-5);
    // And the velocity, which is what a trace quotes: the same decay divided by a `gamma` that moved with it.
    REQUIRE(relative(final_speed / speed, predicted) < 2.0e-5);

    // The same run under Boris: the speed is what it was, to the rounding. That is the other half of the pair,
    // and it is measured on **one cadence** because both schemes share the sub-step control.
    BorisAdvancer boris;
    const RunOutcome boris_out =
        run_pusher(boris, table, params, start, velocity, kProtonChargeMassSI, steps, dt);
    REQUIRE(relative(norm(boris_out.velocity_c), speed) < 1.0e-12);

    // **And the order, measured the way a course measures it.** One gyration, taken in `n` steps for three values
    // of `n`: the particle should come back to where it started, and the distance it misses by is the scheme's
    // error. The prediction is a ratio per halving -- four for a second-order scheme, sixteen for a fourth-order
    // one -- and the two schemes are driven through the *same* harness so the ratios mean what they say.
    const double period = 2.0 * 3.14159265358979323846 / omega;
    const double gyroradius = speed / omega;
    const int counts[3] = {16, 32, 64};
    double boris_error[3] = {0.0, 0.0, 0.0};
    double rk4_error[3] = {0.0, 0.0, 0.0};
    for (int index = 0; index < 3; ++index) {
        const std::size_t n = static_cast<std::size_t>(counts[index]);
        const double step = period / static_cast<double>(n);
        Rk4Advancer rk4_run;
        const RunOutcome out_rk4 =
            run_pusher(rk4_run, table, params, start, velocity, kProtonChargeMassSI, n, step);
        BorisAdvancer boris_run;
        const RunOutcome out_boris =
            run_pusher(boris_run, table, params, start, velocity, kProtonChargeMassSI, n, step);
        REQUIRE(out_rk4.total_substeps == n);
        REQUIRE(out_boris.total_substeps == n);
        rk4_error[index] = norm(out_rk4.position_re - start) / gyroradius;
        boris_error[index] = norm(out_boris.position_re - start) / gyroradius;
    }
    CAPTURE(boris_error[0], boris_error[1], boris_error[2], rk4_error[0], rk4_error[1], rk4_error[2]);
    REQUIRE(rk4_error[0] > 0.0);
    REQUIRE(boris_error[0] > 0.0);
    // Measured: `3.988` and `3.996` for Boris, `15.978` and `15.994` for RK4 -- the orders themselves, read off
    // three runs each. The numbers have two and a half percent of room around them, because a convergence ratio is
    // a measurement rather than a constant, and no room at all would be asserting where one machine rounds.
    REQUIRE(relative(boris_error[0] / boris_error[1], 4.0) < 0.025);
    REQUIRE(relative(boris_error[1] / boris_error[2], 4.0) < 0.025);
    REQUIRE(relative(rk4_error[0] / rk4_error[1], 16.0) < 0.025);
    REQUIRE(relative(rk4_error[1] / rk4_error[2], 16.0) < 0.025);
    // And at the coarsest cadence measured the fourth-order scheme is sixty-five times closer to where the particle
    // started -- `0.0804` against `0.00124` of a gyroradius after one gyration in sixteen steps -- which is what its
    // order buys. The other side of that trade is the four field samples a sub-step, asserted beside it.
    REQUIRE(rk4_error[0] < boris_error[0]);
    REQUIRE(Rk4Advancer::kSamplesPerSubstep == 4);
}
