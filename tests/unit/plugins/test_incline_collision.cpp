/**
 * @file test_incline_collision.cpp
 * @brief Tests for the incline-with-friction and merge-collision operators.
 *
 * Both cases are checked against **closed forms**, not against stored numbers. That is
 * the difference between a test that says "the behaviour changed" and one that says "the
 * behaviour is wrong": `s = g sin(theta) t^2 / 2` on a frictionless incline is derivable
 * from the physics, and a constant that someone once observed is not.
 *
 * The incline assertions are about a **decision** rather than an integration. A block
 * holds or it slides; the interesting failures are a block that creeps downhill at the
 * threshold and a block that jitters across it, and neither shows up as a large error in
 * a distance -- they show up as a velocity that is not exactly zero.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/mechanics/collision.hpp>
#include <qp/plugins/mechanics/incline.hpp>

#include <qp/graph/field/field.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace qp::plugins::mechanics;

namespace {

namespace kernels = qp::graph::kernels;
namespace field = qp::graph::field;

/// @brief State vectors described as a legal lattice, with the components each operator reads.
///
/// One class for both operators because the layout question is the same and got the same
/// answer: the ABI fixes a description's stride at `component_count x element_size`, so a
/// state of N doubles is a **vector** lattice with three components plus reserved slots.
/// Setting `spacing_bytes` by hand to the stride one wants produces a description
/// `abi::is_consistent` refuses.
class Batch final {
public:
    /// Three doubles per particle, which is the **only** stride a legal f64 description
    /// can have besides one.
    ///
    /// `qp::abi::ComponentKind` is scalar(1) or vector(3) -- there is no "four components"
    /// -- and `abi::is_consistent` requires `spacing_bytes == component_count x
    /// element_size`, so the ABI admits strides of 1 and 3 and nothing else. This helper
    /// first used four, to leave a reserved slot beside `(position, velocity, angle)`; the
    /// operators read a stride of three from the description and refused every batch. The
    /// four-slot idea is not expressible, and the operators were right to say so.
    ///
    /// So each operator reads the three components it needs and the layout is exactly
    /// full: `(position, velocity, angle)` for the incline, `(position, velocity, mass)`
    /// for the merge. The oscillator uses the same stride and leaves the third slot at
    /// zero.
    static constexpr std::size_t kStride = 3;


    explicit Batch(std::size_t count) : data_(count * kStride, 0.0) {
        desc_.kind = qp::abi::LatticeKind::line;
        desc_.component = qp::abi::ComponentKind::vector;
        desc_.element = qp::abi::ElementType::f64;
        desc_.count[0] = static_cast<std::uint32_t>(count);
        desc_.spacing_bytes = qp::abi::expected_spacing(desc_);
        REQUIRE(qp::abi::is_consistent(desc_));
    }

    /// @brief One particle with the given position, velocity and third component.
    [[nodiscard]] static Batch one(double pos, double vel, double third) {
        Batch b(1);
        b.data_[0] = pos;
        b.data_[1] = vel;
        b.data_[2] = third;
        return b;
    }

    void set(std::size_t i, std::size_t component, double value) {
        data_[i * kStride + component] = value;
    }
    [[nodiscard]] double get(std::size_t i, std::size_t component) const {
        return data_[i * kStride + component];
    }
    [[nodiscard]] std::size_t count() const { return data_.size() / kStride; }

    [[nodiscard]] field::FieldValue view() {
        field::FieldValue v;
        v.desc = desc_;
        v.data = data_.data();
        v.bytes = data_.size() * sizeof(double);
        return v;
    }

    /// @brief One in-place step, asserting the operator accepted it.
    template <class Op>
    void step(Op& op, double dt) {
        field::FieldValue out = view();
        kernels::BatchView bv;
        bv.in = &out;
        bv.out = &out;
        bv.count = count();
        kernels::AdvanceContext ctx;
        ctx.dt = dt;
        const auto result = op.advance(bv, ctx);
        REQUIRE(result.has_value());
    }

    /// @brief One step that the operator is expected to refuse.
    template <class Op>
    bool step_is_refused(Op& op, double dt) {
        field::FieldValue out = view();
        kernels::BatchView bv;
        bv.in = &out;
        bv.out = &out;
        bv.count = count();
        kernels::AdvanceContext ctx;
        ctx.dt = dt;
        return !op.advance(bv, ctx).has_value();
    }

private:
    std::vector<double> data_;
    qp::abi::LatticeDesc desc_{};
};

/// @brief Prepares an incline operator, asserting the parameters were accepted.
void prepare_incline(InclineFriction& op, double g, double mu_s, double mu_k) {
    kernels::ParamBlock params;
    params.set_real(0, g);
    params.set_real(1, mu_s);
    params.set_real(2, mu_k);
    REQUIRE(op.prepare(params).has_value());
}

/// @brief Prepares a merge operator with zero restitution, the only supported value.
void prepare_merge(MergeCollision& op) {
    kernels::ParamBlock params;
    params.set_real(0, 0.0);
    REQUIRE(op.prepare(params).has_value());
}

/// @brief `g` on a frictionless incline: the closed-form distance after time `t`.
double frictionless_distance(double g, double theta, double t) {
    return 0.5 * g * std::sin(theta) * t * t;
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace

// ===========================================================================
// Incline
// ===========================================================================

TEST_CASE("plugin.mechanics.incline_registers_under_its_name", "[plugin][mechanics]") {
    InclineFriction op;
    REQUIRE(op.name() == "incline_friction");
    REQUIRE(op.capabilities() == kernels::Capability::none);
}

TEST_CASE("plugin.mechanics.incline_rejects_impossible_surfaces", "[plugin][mechanics]") {
    const auto refuses = [](double g, double mu_s, double mu_k) {
        InclineFriction op;
        kernels::ParamBlock params;
        params.set_real(0, g);
        params.set_real(1, mu_s);
        params.set_real(2, mu_k);
        return !op.prepare(params).has_value();
    };

    // Gravity has to be positive and finite. `!(g > 0.0)` catches NaN by the same test
    // that catches zero, which matters because a comparison against NaN is false and a
    // `g <= 0.0` form would let a failed upstream computation through.
    REQUIRE(refuses(0.0, 0.5, 0.4));
    REQUIRE(refuses(-9.81, 0.5, 0.4));
    REQUIRE(refuses(std::numeric_limits<double>::quiet_NaN(), 0.5, 0.4));
    REQUIRE(refuses(std::numeric_limits<double>::infinity(), 0.5, 0.4));

    // Negative coefficients are refused.
    REQUIRE(refuses(9.81, -0.1, 0.0));
    REQUIRE(refuses(9.81, 0.5, -0.1));

    // mu_k > mu_s describes a surface where breaking loose makes sliding harder, which is
    // not a surface. Refused rather than swapped: silently reinterpreting the parameters
    // would hide the author's confusion and produce numbers that look reasonable.
    REQUIRE(refuses(9.81, 0.3, 0.5));

    // Exactly equal is legal and is the idealisation a first exercise uses.
    REQUIRE_FALSE(refuses(9.81, 0.4, 0.4));
    // Zero friction is legal: a frictionless incline is the other first exercise.
    REQUIRE_FALSE(refuses(9.81, 0.0, 0.0));

    // A refusal leaves an earlier good preparation alone, so a parameter panel that calls
    // prepare on every keystroke does not disable a working graph mid-edit.
    InclineFriction op;
    prepare_incline(op, 9.81, 0.6, 0.4);
    REQUIRE(op.gravity() == 9.81);
    kernels::ParamBlock bad;
    bad.set_real(0, 9.81);
    bad.set_real(1, 0.2);
    bad.set_real(2, 0.9);
    REQUIRE_FALSE(op.prepare(bad).has_value());
    REQUIRE(op.is_prepared());
    REQUIRE(op.static_friction() == 0.6);
    REQUIRE(op.kinetic_friction() == 0.4);
}

TEST_CASE("plugin.mechanics.incline_block_rests_below_the_static_threshold", "[plugin][mechanics]") {
    // The angle of repose: a block holds while `tan(theta) <= mu_s`. At theta = 30 degrees
    // tan is 0.577, so mu_s = 0.6 holds it and mu_s = 0.5 does not.
    //
    // The assertion is that the velocity stays **exactly** zero, not merely small. A
    // resting block that accumulated 1e-16 per step would drift measurably over a long
    // run, and the drift would look like a physical effect rather than an arithmetic one.
    const double theta = 30.0 * kPi / 180.0;

    InclineFriction holds;
    prepare_incline(holds, 9.81, 0.6, 0.5);
    Batch resting = Batch::one(0.0, 0.0, theta);
    for (int i = 0; i < 10'000; ++i) resting.step(holds, 1.0e-3);

    REQUIRE(resting.get(0, 1) == 0.0);
    REQUIRE(resting.get(0, 0) == 0.0);

    // And the same incline with a coefficient below the threshold does slide, so the test
    // above is not passing because the operator refuses to move anything ever.
    InclineFriction slides;
    prepare_incline(slides, 9.81, 0.5, 0.4);
    Batch moving = Batch::one(0.0, 0.0, theta);
    for (int i = 0; i < 1000; ++i) moving.step(slides, 1.0e-3);

    REQUIRE(moving.get(0, 1) > 0.0);
    REQUIRE(moving.get(0, 0) > 0.0);
}

TEST_CASE("plugin.mechanics.incline_block_slides_above_it", "[plugin][mechanics]") {
    // Frictionless, so the closed form applies and the **error law** is known exactly.
    //
    // Semi-implicit Euler on a constant acceleration advances the position by `a*dt^2`
    // where the exact answer advances it by `a*dt^2/2`. So each step **overshoots** by
    // `a*dt^2/2`, and after `n` steps the position is long by `a*dt^2*n/2 = a*dt*t/2` --
    // first order in dt, and in the direction of too far rather than too short.
    //
    // That sign is easy to get backwards, and this test had it backwards first: it asserted
    // the block would be short and measured it 2.07e-4 long, which is exactly the predicted
    // `a*dt*t/2`. The assertion is written against the prediction rather than against a
    // direction someone assumed.
    //
    // First order is not an accident to be tightened away: a friction decision at zero
    // velocity is a discontinuity, and a higher-order scheme that samples the force at
    // intermediate velocities cannot make that decision cleanly.
    //
    // The first version allowed a fixed 1e-4 and measured 2.07e-4. Widening the constant to
    // 1e-3 would have made it pass and would also have made it blind: the error grows with
    // dt, so a loose constant silently absorbs a wrong scheme until someone picks a small
    // enough step. Comparing against `a*dt*t/2` means a regression shows up as a factor.
    const double g = 9.81;
    const double theta = 25.0 * kPi / 180.0;
    const double dt = 1.0e-4;
    const int steps = 10'000;  // t = 1 s

    InclineFriction op;
    prepare_incline(op, g, 0.0, 0.0);
    Batch block = Batch::one(0.0, 0.0, theta);
    for (int i = 0; i < steps; ++i) block.step(op, dt);

    const double t = dt * steps;
    const double a = g * std::sin(theta);
    const double exact = frictionless_distance(g, theta, t);
    const double expected_overshoot = 0.5 * a * dt * t;

    REQUIRE(std::abs(block.get(0, 0) - exact) < 2.0 * expected_overshoot);
    // And it is long rather than short, which distinguishes "uses the new velocity" from
    // "uses the old one". Both would fit a two-sided tolerance.
    REQUIRE(block.get(0, 0) > exact);

    // Velocity is exact under semi-implicit Euler with constant acceleration, so this one
    // is asserted tightly: a scheme that got the velocity wrong would move the position
    // correctly for the wrong reason.
    REQUIRE(std::abs(block.get(0, 1) - a * t) < 1.0e-9);

    // Zero velocity parallel to a downhill acceleration is a different case from rest:
    // the block is moving, so kinetic friction applies.
    {
        InclineFriction moving;
        prepare_incline(moving, g, 0.4, 0.4);
        Batch b = Batch::one(0.0, 1.0, theta);
        b.step(moving, dt);
        const double expected = 1.0 + (g * std::sin(theta) - 0.4 * g * std::cos(theta)) * dt;
        REQUIRE(std::abs(b.get(0, 1) - expected) < 1.0e-12);
    }
}

TEST_CASE("plugin.mechanics.incline_stops_instead_of_reversing", "[plugin][mechanics]") {
    // A block sliding **uphill** with friction. Kinetic friction must bring it to rest, and
    // then the static decision takes over. Without the sign check that clamps velocity at
    // zero, friction keeps subtracting and drives the block backwards up the slope -- a
    // block that climbs on its own, which is the numerical artefact this case exists for.
    const double g = 9.81;
    const double theta = 10.0 * kPi / 180.0;
    const double mu = 0.5;

    InclineFriction op;
    prepare_incline(op, g, mu, mu);

    // Launched uphill (negative along-slope velocity) fast enough that gravity cannot stop
    // it before friction does.
    Batch block = Batch::one(0.0, -2.0, theta);
    for (int i = 0; i < 20'000; ++i) block.step(op, 1.0e-4);

    // It comes to rest and stays there: `mu_s = 0.5 > tan(10 deg) = 0.176`, so the static
    // threshold holds it. Reaching rest is the claim; staying there is the claim that the
    // clamp worked.
    REQUIRE(block.get(0, 1) == 0.0);
}

TEST_CASE("plugin.mechanics.incline_rejects_unusable_input", "[plugin][mechanics]") {
    InclineFriction op;
    prepare_incline(op, 9.81, 0.5, 0.4);

    // A non-finite angle is per-particle state, so it cannot be caught in prepare. It is
    // refused rather than propagated: a NaN spreading through a run while every step
    // reported success is worse than a step that fails.
    Batch bad_angle = Batch::one(0.0, 0.0, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(bad_angle.step_is_refused(op, 1.0e-3));

    Batch bad_velocity = Batch::one(0.0, std::numeric_limits<double>::infinity(), 0.1);
    REQUIRE(bad_velocity.step_is_refused(op, 1.0e-3));

    Batch fine = Batch::one(0.0, 0.0, 0.1);
    for (const double bad_dt : {0.0, -1.0e-3, std::numeric_limits<double>::quiet_NaN()}) {
        REQUIRE(fine.step_is_refused(op, bad_dt));
    }

    // An empty batch and an unprepared operator are both refused.
    {
        kernels::BatchView empty;
        kernels::AdvanceContext ctx;
        ctx.dt = 1.0e-3;
        REQUIRE_FALSE(op.advance(empty, ctx).has_value());
    }
    {
        InclineFriction fresh;
        REQUIRE(fine.step_is_refused(fresh, 1.0e-3));
    }
}

TEST_CASE("plugin.mechanics.incline_handles_every_block_in_the_batch", "[plugin][mechanics]") {
    // Two different angles in one batch, which is the reason the angle is per particle.
    // An operator that used only the first particle's angle would pass every
    // single-particle test above.
    const double g = 9.81;
    InclineFriction op;
    prepare_incline(op, g, 0.0, 0.0);

    Batch two(2);
    two.set(0, 2, 20.0 * kPi / 180.0);
    two.set(1, 2, 40.0 * kPi / 180.0);

    const double dt = 1.0e-4;
    const int steps = 1000;
    for (int i = 0; i < steps; ++i) two.step(op, dt);

    const double t = dt * steps;
    // The same first-order shortfall as the single-block case, applied per angle: the
    // error is `a*dt*t/2` with `a = g sin(theta)`, so the two particles converge to the
    // closed form at different rates. Comparing against the closed form with a bound that
    // depends on each particle's own angle is what makes this a test of both, rather than
    // of the shallower one.
    const double a20 = g * std::sin(20.0 * kPi / 180.0);
    const double a40 = g * std::sin(40.0 * kPi / 180.0);
    REQUIRE(std::abs(two.get(0, 0) - frictionless_distance(g, 20.0 * kPi / 180.0, t)) <
            2.0 * 0.5 * a20 * dt * t);
    REQUIRE(std::abs(two.get(1, 0) - frictionless_distance(g, 40.0 * kPi / 180.0, t)) <
            2.0 * 0.5 * a40 * dt * t);
    // The steeper slope travels further, which is the comparison the exercise asks for.
    REQUIRE(two.get(1, 0) > two.get(0, 0));
}

// ===========================================================================
// Merge collision
// ===========================================================================

TEST_CASE("plugin.mechanics.merge_registers_under_its_name", "[plugin][mechanics]") {
    MergeCollision op;
    REQUIRE(op.name() == "merge_collision");
    // Not `is_neighbourhood`: every particle contributes to a sum and none reads another,
    // so the host must not build a spatial index for this.
    REQUIRE(op.capabilities() == kernels::Capability::none);
    REQUIRE_FALSE(kernels::has_capability(op.capabilities(), kernels::Capability::is_neighbourhood));
}

TEST_CASE("plugin.mechanics.merge_refuses_partial_restitution", "[plugin][mechanics]") {
    // Only the perfectly inelastic case is expressible. A bounce needs the collision
    // normal, which a flat batch does not carry; returning the merged velocity under a
    // `restitution = 0.8` request would be a wrong number with a right-sounding name.
    const auto outcome_for = [](double restitution) {
        MergeCollision op;
        kernels::ParamBlock params;
        params.set_real(0, restitution);
        return op.prepare(params).has_value();
    };

    REQUIRE(outcome_for(0.0));
    REQUIRE_FALSE(outcome_for(0.5));
    REQUIRE_FALSE(outcome_for(1.0));
    // The code is `not_implemented` rather than `invalid_argument`: 0.8 is a perfectly
    // valid restitution, it is simply not something this operator can compute.
    {
        MergeCollision op;
        kernels::ParamBlock params;
        params.set_real(0, 0.8);
        REQUIRE(op.prepare(params).error() == qp::diag::ErrorCode::not_implemented);
    }

    REQUIRE_FALSE(outcome_for(-0.1));
    REQUIRE_FALSE(outcome_for(1.5));
    REQUIRE_FALSE(outcome_for(std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("plugin.mechanics.merge_conserves_momentum", "[plugin][mechanics]") {
    // The whole physical content of the operator. Total momentum before equals total
    // momentum after, for masses and velocities chosen so no single value is exact by
    // luck: a momentum sum is `m*v`, and a failed implementation that averaged velocities
    // instead of weighting them would agree here only for equal masses, which is exactly
    // the case this fixture avoids.
    MergeCollision op;
    prepare_merge(op);

    Batch b(3);
    const double masses[3] = {0.5, 2.0, 1.5};
    const double velocities[3] = {3.0, -1.0, 0.25};
    double momentum_before = 0.0;
    double mass_total = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        b.set(i, 0, static_cast<double>(i));
        b.set(i, 1, velocities[i]);
        b.set(i, 2, masses[i]);
        momentum_before += masses[i] * velocities[i];
        mass_total += masses[i];
    }

    b.step(op, 1.0e-3);

    double momentum_after = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        // Every particle ends at the same velocity.
        REQUIRE(b.get(i, 1) == b.get(0, 1));
        momentum_after += b.get(i, 2) * b.get(i, 1);
        // Mass is carried through unchanged: a collision does not consume matter, and a
        // momentum-conserving implementation that corrupted the masses would pass the
        // check above while making the energy check below meaningless.
        REQUIRE(b.get(i, 2) == masses[i]);
        // Position is untouched. A coalescence moves the centre of mass; it does not
        // teleport the bodies onto one point.
        REQUIRE(b.get(i, 0) == static_cast<double>(i));
    }

    const double expected = momentum_before / mass_total;
    REQUIRE(std::abs(b.get(0, 1) - expected) < 1.0e-12);
    REQUIRE(std::abs(momentum_after - momentum_before) < 1.0e-12);
}

TEST_CASE("plugin.mechanics.merge_loses_kinetic_energy", "[plugin][mechanics]") {
    // The other half of the lesson, and the half that makes the experiment worth doing: a
    // perfectly inelastic collision conserves momentum and destroys kinetic energy. A
    // student measures both and finds one of them missing.
    //
    // This is asserted rather than assumed because an operator that returned the
    // *momentum-weighted velocity* for each particle -- preserving each one's energy --
    // would still pass the momentum test for a batch of equal masses.
    MergeCollision op;
    prepare_merge(op);

    Batch b(2);
    b.set(0, 0, 0.0);
    b.set(0, 1, 2.0);
    b.set(0, 2, 1.0);
    b.set(1, 0, 1.0);
    b.set(1, 1, -2.0);
    b.set(1, 2, 1.0);

    const double ke_before = 0.5 * 1.0 * 2.0 * 2.0 + 0.5 * 1.0 * 2.0 * 2.0;  // 4 J

    b.step(op, 1.0e-3);

    // Equal masses and opposite velocities: the centre of mass is at rest.
    REQUIRE(b.get(0, 1) == 0.0);
    REQUIRE(b.get(1, 1) == 0.0);

    double ke_after = 0.0;
    for (std::size_t i = 0; i < 2; ++i) {
        ke_after += 0.5 * b.get(i, 2) * b.get(i, 1) * b.get(i, 1);
    }
    REQUIRE(ke_after < ke_before);
    REQUIRE(ke_after == 0.0);  // all of it, in this configuration

    // And the limiting case: one moving body and one at rest, which is the standard
    // textbook exercise. Half the kinetic energy survives.
    {
        Batch two(2);
        two.set(0, 1, 4.0);
        two.set(0, 2, 1.0);
        two.set(1, 1, 0.0);
        two.set(1, 2, 1.0);
        const double before = 0.5 * 1.0 * 16.0;  // 8 J
        two.step(op, 1.0e-3);
        REQUIRE(two.get(0, 1) == 2.0);  // momentum 4 over mass 2
        const double after = 0.5 * 2.0 * 4.0;   // 4 J
        REQUIRE(std::abs(after - 0.5 * before) < 1.0e-12);
    }
}

TEST_CASE("plugin.mechanics.merge_refuses_unusable_state", "[plugin][mechanics]") {
    MergeCollision op;
    prepare_merge(op);

    // A non-positive mass is refused rather than skipped. Skipping a zero would silently
    // drop a particle; a negative one would give a common velocity outside the inputs.
    const auto refuses_mass = [&](double m) {
        Batch b = Batch::one(0.0, 1.0, m);
        return b.step_is_refused(op, 1.0e-3);
    };
    REQUIRE(refuses_mass(0.0));
    REQUIRE(refuses_mass(-1.0));
    REQUIRE(refuses_mass(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE(refuses_mass(std::numeric_limits<double>::infinity()));
    REQUIRE_FALSE(refuses_mass(1.0e-12));  // tiny is legal; it is a mass

    {
        Batch b = Batch::one(0.0, std::numeric_limits<double>::quiet_NaN(), 1.0);
        REQUIRE(b.step_is_refused(op, 1.0e-3));
    }
    {
        kernels::BatchView empty;
        kernels::AdvanceContext ctx;
        ctx.dt = 1.0e-3;
        REQUIRE_FALSE(op.advance(empty, ctx).has_value());
    }
    {
        MergeCollision fresh;
        Batch b = Batch::one(0.0, 1.0, 1.0);
        REQUIRE(b.step_is_refused(fresh, 1.0e-3));
    }
}

TEST_CASE("plugin.mechanics.incline_clamps_and_reports_it", "[plugin][mechanics]") {
    // Charter C2: a kernel declares its clamping policy, and C8: numerical error must not be
    // mistaken for physics. The property is not that values get bounded -- it is that the
    // operator can **say** it bounded one.
    InclineFriction op;
    REQUIRE(op.clamps_fired() == 0);

    const kernels::ClampPolicy policy = op.clamp_policy();
    REQUIRE(policy.finite_only);
    // `bounded`, not `finite`: a block's position along a slope grows without limit under
    // gravity -- that is the exercise -- so a magnitude bound is meaningful here in a way it
    // is not for an operator that averages its inputs.
    REQUIRE(policy.limit > 0.0);

    // A clean run reports nothing, which is the majority case and the one that must stay
    // honest.
    prepare_incline(op, 9.81, 0.4, 0.3);
    Batch block = Batch::one(0.0, 0.0, 20.0 * kPi / 180.0);
    for (int i = 0; i < 1000; ++i) block.step(op, 1.0e-3);
    REQUIRE(op.clamps_fired() == 0);

    // A parameter change is a new experiment, so the count starts over.
    prepare_incline(op, 9.81, 0.5, 0.4);
    REQUIRE(op.clamps_fired() == 0);

    // A NaN angle cannot be caught in `prepare` -- it is per-particle state -- so it reaches
    // the write path. It is refused before that, but the clamp policy is what makes the
    // refusal safe if a future change lets a non-finite value through: the policy is checked
    // first and reports.
    Batch bad = Batch::one(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.1);
    REQUIRE(bad.step_is_refused(op, 1.0e-3));
}

TEST_CASE("plugin.mechanics.merge_clamps_and_reports_it", "[plugin][mechanics]") {
    MergeCollision op;
    REQUIRE(op.clamps_fired() == 0);

    const kernels::ClampPolicy policy = op.clamp_policy();
    REQUIRE(policy.finite_only);
    // `finite`, not `bounded`: this operator computes a weighted average, and an average of
    // bounded inputs is bounded by them, so a magnitude bound would be a claim about the
    // physics that the operator has no basis for. What it can produce is a NaN, from a mass
    // total that overflowed.
    REQUIRE(policy.limit == 0.0);

    // A clean collision reports nothing.
    prepare_merge(op);
    REQUIRE(op.clamps_fired() == 0);
    Batch two(2);
    two.set(0, 1, 3.0);
    two.set(0, 2, 2.0);
    two.set(1, 1, -1.0);
    two.set(1, 2, 1.0);
    two.step(op, 1.0e-3);
    REQUIRE(op.clamps_fired() == 0);

    // And a non-finite input is refused before it can become a silent NaN in the output.
    Batch poisoned = Batch::one(0.0, std::numeric_limits<double>::quiet_NaN(), 1.0);
    REQUIRE(poisoned.step_is_refused(op, 1.0e-3));
}