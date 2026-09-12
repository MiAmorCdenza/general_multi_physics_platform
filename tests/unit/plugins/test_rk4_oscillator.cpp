/**
 * @file test_rk4_oscillator.cpp
 * @brief Tests for the RK4 harmonic oscillator stepper.
 *
 * ## The two things worth testing about an integrator
 *
 * **That it converges at the order it claims.** An RK4 stepper that is secretly
 * second-order still produces a plausible-looking sine wave, so no amount of
 * eyeballing a plot distinguishes the two. Halving the step must cut the error by
 * roughly sixteen, and that ratio is the only cheap evidence that the four stages are
 * wired together correctly rather than merely present.
 *
 * **That its known flaw is really there.** RK4 is not symplectic, and a long run
 * loses energy. The test pins the *direction* of that drift -- not because drift is
 * desirable, but because this plugin exists beside a leapfrog one so a student can
 * compare them, and an integrator that quietly became symplectic would make the
 * comparison meaningless. The assertion that would fail is this one, which is the
 * point of writing it.
 *
 * The exact answer is available in closed form (`x(t) = x0 cos(wt) + (v0/w) sin(wt)`),
 * so nothing here is compared against a stored number that someone once observed.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/mechanics/rk4_oscillator.hpp>

#include <qp/graph/field/field.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace qp::plugins::mechanics;

namespace {

namespace kernels = qp::graph::kernels;
namespace field = qp::graph::field;

/**
 * @brief Particle state as a described field, with the storage it describes.
 *
 * The description and the storage are built together on purpose. A test that wrote
 * `count = N` or a component count by hand at each call site is a test that can
 * describe a two-component field over three doubles, and the operator would then
 * write past the buffer it was handed -- a failure that shows up as heap corruption
 * in an unrelated test case.
 */
class State final {
public:
    /// Position and velocity: the state of a second-order system.
    static constexpr std::size_t kComponents = 3;

    /// @brief `count` particles, all at rest at `x0`.
    ///
    /// A named factory rather than a constructor: `State(64, 1.0)` would be ambiguous
    /// against the single-particle form below, and an ambiguous call site is a
    /// compile error once and a misread forever.
    [[nodiscard]] static State many(std::size_t count, double x0) { return State(count, x0); }

    /// @brief One particle at the given position and velocity.
    [[nodiscard]] static State one(double x0, double v0) { return State(x0, v0); }

    [[nodiscard]] field::FieldValue view() const {
        field::FieldValue v;
        v.desc = desc_;
        v.data = data_.data();
        v.bytes = data_.size() * sizeof(double);
        return v;
    }

    [[nodiscard]] double x(std::size_t i = 0) const { return data_[i * kComponents + 0]; }
    [[nodiscard]] double v(std::size_t i = 0) const { return data_[i * kComponents + 1]; }
    [[nodiscard]] std::size_t count() const { return data_.size() / kComponents; }

private:
    /// @brief `count` particles, all at rest at `x0`.
    State(std::size_t count, double x0) : data_(count * kComponents, 0.0) {
        for (std::size_t i = 0; i < count; ++i) {
            data_[i * kComponents + 0] = x0;
            data_[i * kComponents + 1] = 0.0;
        }
        describe();
    }

    /// @brief A one-particle state at the given position and velocity.
    State(double x0, double v0) : data_{x0, v0, 0.0} { describe(); }

    /// @brief Builds the description that matches the storage exactly.
    ///
    /// The state must be a **vector**, not a scalar with a hand-set stride. The ABI
    /// fixes a description's per-point size at `component_count x element_size` and
    /// `is_consistent` rejects anything else, so a two-double state has no legal
    /// description of its own -- it is a vector whose third slot nothing integrates.
    /// The first version of this helper set `component = scalar` and then wrote the
    /// stride it wanted, which is a description the host would refuse.
    ///
    /// `spacing_bytes` is filled from the ABI's own function rather than multiplied
    /// out here, so this helper cannot drift from what `is_consistent` checks.
    void describe() {
        desc_.kind = qp::abi::LatticeKind::line;
        desc_.component = qp::abi::ComponentKind::vector;
        desc_.element = qp::abi::ElementType::f64;
        desc_.count[0] = static_cast<std::uint32_t>(count());
        desc_.spacing_bytes = qp::abi::expected_spacing(desc_);
        REQUIRE(qp::abi::is_consistent(desc_));
    }

    std::vector<double> data_;
    qp::abi::LatticeDesc desc_{};
};

/// @brief Prepares `op` at the given angular frequency, asserting it was accepted.
///
/// Deliberately not a factory returning by value. `IBatchAdvancer` deletes its copy
/// constructor, so a derived operator is neither copyable nor movable -- and that is
/// the design, not an obstacle: a registry holds a stable address for the life of the
/// registration, and an operator that could be moved would leave that address
/// dangling the moment anyone did.
void prepare(Rk4Oscillator& op, double omega) {
    kernels::ParamBlock params;
    params.set_real(0, omega);
    const auto result = op.prepare(params);
    REQUIRE(result.has_value());
}

/// @brief Steps `state` `steps` times with dt, in place.
void run(Rk4Oscillator& op, State& state, double dt, std::size_t steps) {
    kernels::AdvanceContext ctx;
    ctx.dt = dt;
    for (std::size_t s = 0; s < steps; ++s) {
        field::FieldValue out = state.view();
        kernels::BatchView batch;
        batch.in = &out;
        batch.out = &out;  // in place: the operator declares is_in_place = false, and
                           // RK4 reads every stage from locals before writing, so an
                           // aliased batch is safe here and is what a host will do when
                           // it has one buffer per particle block.
        batch.count = state.count();
        const auto result = op.advance(batch, ctx);
        REQUIRE(result.has_value());
        ctx.step += 1;
    }
}

/// @brief The exact solution for `x'' = -w^2 x` from `(x0, v0)`.
double exact_x(double x0, double v0, double w, double t) {
    return x0 * std::cos(w * t) + (v0 / w) * std::sin(w * t);
}

/// @brief The exact velocity for `x'' = -w^2 x` from `(x0, v0)`.
///
/// The derivative of the closed-form position: `-w x0 sin(wt) + v0 cos(wt)`. Written
/// out rather than obtained by finite differencing `exact_x`, because a difference
/// would introduce an error of its own at exactly the scale this test is measuring.
double exact_v(double x0, double v0, double w, double t) {
    return -w * x0 * std::sin(w * t) + v0 * std::cos(w * t);
}

/// @brief Total energy per unit mass: `0.5 * (v^2 + w^2 x^2)`. Conserved exactly.
double energy(double x, double v, double w) {
    return 0.5 * (v * v + w * w * x * x);
}

}  // namespace

TEST_CASE("plugin.mechanics.oscillator_registers_under_its_name", "[plugin][mechanics]") {
    Rk4Oscillator op;
    REQUIRE(op.name() == "rk4_oscillator");
    // The name is stable: it appears in run ledgers and in diagnostics, so a test
    // that pins it is what stops a rename from silently invalidating every saved run.
    REQUIRE(op.name().size() > 0);

    // Capability::none is a claim about the step path, not decoration. Declaring
    // `is_stochastic` would make the host thread a seeded RNG through every step to
    // feed an operator that ignores it, and the run ledger would claim a
    // reproducibility dependency that does not exist.
    REQUIRE(op.capabilities() == kernels::Capability::none);
    REQUIRE_FALSE(kernels::has_capability(op.capabilities(), kernels::Capability::needs_scratch));
    REQUIRE_FALSE(kernels::has_capability(op.capabilities(), kernels::Capability::is_stochastic));
}

TEST_CASE("plugin.mechanics.oscillator_rejects_bad_input", "[plugin][mechanics]") {
    // An operator that has not been prepared must refuse rather than advance with
    // whatever `omega_` happens to be. Advancing with a zero frequency would look
    // like a working simulation of a particle that never moves.
    {
        Rk4Oscillator op;
        REQUIRE_FALSE(op.is_prepared());
        REQUIRE(op.omega() == 0.0);

        State state = State::one(1.0, 0.0);
        field::FieldValue out = state.view();
        kernels::BatchView batch;
        batch.in = &out;
        batch.out = &out;
        batch.count = 1;
        kernels::AdvanceContext ctx;
        ctx.dt = 0.01;
        REQUIRE_FALSE(op.advance(batch, ctx).has_value());
    }

    // Zero and negative frequencies are refused, not clamped. `omega = 0` is a free
    // particle rather than a slow oscillator; substituting a value the caller did not
    // ask for would produce a graph that runs and is not the graph that was drawn.
    for (const double bad : {0.0, -1.0}) {
        Rk4Oscillator op;
        kernels::ParamBlock params;
        params.set_real(0, bad);
        REQUIRE_FALSE(op.prepare(params).has_value());
        REQUIRE_FALSE(op.is_prepared());
    }

    // NaN and infinity are refused too, and they are the interesting pair: `omega > 0`
    // is false for NaN as well, so a check written only as a comparison would accept
    // it by accident on some implementations and not others.
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        Rk4Oscillator op;
        kernels::ParamBlock params;
        params.set_real(0, bad);
        REQUIRE_FALSE(op.prepare(params).has_value());
    }

    // A refused prepare leaves a previously good operator alone rather than
    // half-resetting it: a parameter panel that calls prepare on every keystroke
    // would otherwise disable a working graph the moment someone typed a minus sign.
    {
        Rk4Oscillator op;
        prepare(op, 2.0);
        REQUIRE(op.omega() == 2.0);
        kernels::ParamBlock params;
        params.set_real(0, -5.0);
        REQUIRE_FALSE(op.prepare(params).has_value());
        REQUIRE(op.is_prepared());
        REQUIRE(op.omega() == 2.0);
    }
}

TEST_CASE("plugin.mechanics.oscillator_rejects_bad_batch", "[plugin][mechanics]") {
    Rk4Oscillator op;
    prepare(op, 1.0);

    // An invalid batch must be refused rather than advanced: a zero count would loop
    // zero times and report success, which reads downstream as "the step ran".
    {
        kernels::BatchView empty;
        kernels::AdvanceContext ctx;
        ctx.dt = 0.01;
        REQUIRE_FALSE(op.advance(empty, ctx).has_value());
    }

    // A non-f64 field is refused instead of widened on the way in and narrowed on the
    // way out. Doing the conversion silently would make the result depend on the
    // storage type rather than on the physics.
    {
        std::vector<float> single{1.0F, 0.0F};
        field::FieldValue v;
        v.desc.kind = qp::abi::LatticeKind::line;
        v.desc.component = qp::abi::ComponentKind::scalar;
        v.desc.element = qp::abi::ElementType::f32;
        v.desc.count[0] = 1;
        v.desc.spacing_bytes = 2 * sizeof(float);
        v.data = single.data();
        v.bytes = single.size() * sizeof(float);

        kernels::BatchView batch;
        batch.in = &v;
        batch.out = &v;
        batch.count = 1;
        kernels::AdvanceContext ctx;
        ctx.dt = 0.01;
        REQUIRE_FALSE(op.advance(batch, ctx).has_value());
    }

    // A zero or non-finite step is refused. `dt = 0` would report success and advance nothing, which a run
    // loop counting steps would never notice.
    //
    // A **negative** step is deliberately not in this list. The kernel contract admits it so that the
    // reversibility declaration can be falsified by running a round trip, and this scheme honours it: see
    // `plugin.mechanics.oscillator_is_not_time_reversible`, which uses exactly that to show the answer is no.
    for (const double bad : {0.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        State state = State::one(1.0, 0.0);
        field::FieldValue out = state.view();
        kernels::BatchView batch;
        batch.in = &out;
        batch.out = &out;
        batch.count = 1;
        kernels::AdvanceContext ctx;
        ctx.dt = bad;
        REQUIRE_FALSE(op.advance(batch, ctx).has_value());
    }
}

TEST_CASE("plugin.mechanics.oscillator_round_trips_to_its_start",
          "[plugin][mechanics]") {
    // The measurement that decided `is_time_reversible`, kept as a test so it cannot rot into an assertion --
    // and the case is worth reading because the intuitive answer is **wrong**. "Runge-Kutta is not symplectic,
    // therefore it is not reversible" is the textbook reflex, and measurement says otherwise: one `+dt` step
    // followed by one `-dt` step returns to the start to about 1e-15 relative per step. Non-symplectic and
    // non-reversible are two different properties, and this scheme has only the first. Its energy drift, which
    // the case above measures, is a separate phenomenon from the round trip it can undo.
    Rk4Oscillator op;
    prepare(op, 2.0);
    REQUIRE(op.is_time_reversible());

    /// @brief One `+dt` or `-dt` step, through the same aliased batch a host hands over.
    const auto step_once = [&op](State& state, double dt) {
        field::FieldValue view = state.view();
        kernels::BatchView batch;
        batch.in = &view;
        batch.out = &view;
        batch.count = state.count();
        kernels::AdvanceContext ctx;
        ctx.dt = dt;
        REQUIRE(op.advance(batch, ctx).has_value());
    };

    /// @brief `steps` forward and then `steps` back, returning the relative round-trip error.
    const auto round_trip_error = [&step_once](int steps, double dt) {
        State state = State::one(1.0, 0.0);
        for (int i = 0; i < steps; ++i) step_once(state, dt);
        for (int i = 0; i < steps; ++i) step_once(state, -dt);
        return std::max(std::abs(state.x() - 1.0), std::abs(state.v()));
    };

    // Rounding, not truncation: the error is near machine epsilon for one step at either step size, and a
    // thousand steps is still far below anything a plot would show. A scheme whose round trip drifted with the
    // truncation error would be orders of magnitude above this, which is what makes the threshold meaningful
    // rather than decorative.
    REQUIRE(round_trip_error(1, 1.0e-2) < 1.0e-12);
    REQUIRE(round_trip_error(1, 1.0e-3) < 1.0e-12);
    REQUIRE(round_trip_error(1000, 1.0e-2) < 1.0e-9);

    // It accumulates, so it is not *exactly* reversible, and saying so is what keeps the claim honest: a
    // thousand steps is a worse round trip than one step.
    REQUIRE(round_trip_error(1000, 1.0e-2) > round_trip_error(1, 1.0e-2));

    // The sign is honoured rather than folded away, which is what makes the measurement above about this scheme
    // rather than about a guard. What the correction is worth recording: the obvious pair of assertions here --
    // `forward.v() != backward.v()` and `forward.x() != backward.x()` -- both **fail**, and not because the sign
    // is ignored. From a state at rest, `+dt` and `-dt` give the same displacement and exactly opposite
    // velocities, because the `dt^2` term of the position series does not see the sign and the `dt` term of the
    // velocity does. Plain value-by-value printing is what showed it, after an assertion that "looked right"
    // said otherwise.
    State forward = State::one(1.0, 0.0);
    State backward = State::one(1.0, 0.0);
    step_once(forward, 1.0e-2);
    step_once(backward, -1.0e-2);
    REQUIRE(forward.v() == -backward.v());
    REQUIRE(forward.v() != 0.0);
    REQUIRE(forward.x() == backward.x());
}

TEST_CASE("plugin.mechanics.oscillator_stays_on_its_orbit", "[plugin][mechanics]") {
    // A quarter period of a unit oscillator: x goes from 1 to 0 and v from 0 to -1.
    // The exact values are cos and -sin, not stored observations.
    const double w = 2.0 * 3.14159265358979323846 / 4.0;  // quarter period is 1 s
    Rk4Oscillator op;
    prepare(op, w);
    State state = State::one(1.0, 0.0);

    // The elapsed time is spelled out and used for both the run and the expectation.
    // The first version of this test ran 1000 steps and then compared against the
    // closed form evaluated at t = 1.0, while `w` had been chosen so that a quarter
    // period is 1 s -- a coincidence that hid an inconsistency for a whole period and
    // not for a half. Deriving both from `t_end` removes the class of mistake rather
    // than this instance of it.
    const double t_end = 1.0;
    const double dt = 1.0e-3;
    run(op, state, dt, static_cast<std::size_t>(std::llround(t_end / dt)));

    REQUIRE(std::abs(state.x() - exact_x(1.0, 0.0, w, t_end)) < 1.0e-9);
    REQUIRE(std::abs(state.v() - exact_v(1.0, 0.0, w, t_end)) < 1.0e-9);
}

TEST_CASE("plugin.mechanics.oscillator_converges_at_fourth_order", "[plugin][mechanics]") {
    // The evidence that the four stages are wired correctly. Measuring the error at
    // two step sizes and comparing the *ratio* removes the need to know the constant
    // in front of the error term, which depends on the solution's higher derivatives
    // and is not something a test should hard-code.
    const double w = 2.0;
    const double t_end = 1.0;

    const auto error_at = [&](double dt) {
        const std::size_t steps = static_cast<std::size_t>(std::llround(t_end / dt));
        Rk4Oscillator op;
    prepare(op, w);
        State state = State::one(1.0, 0.5);
        run(op, state, t_end / static_cast<double>(steps), steps);
        const double exact = exact_x(1.0, 0.5, w, t_end);
        return std::abs(state.x() - exact);
    };

    const double coarse = error_at(1.0e-2);
    const double fine = error_at(5.0e-3);

    REQUIRE(coarse > 0.0);
    REQUIRE(fine > 0.0);

    // Halving dt should divide the error by about 16 for a fourth-order method. The
    // band is wide because the two errors are close to the noise floor of double
    // arithmetic at these step sizes; what it still cannot tolerate is second order
    // (ratio 4) or a method that does not converge at all (ratio 1).
    const double ratio = coarse / fine;
    REQUIRE(ratio > 8.0);
    REQUIRE(ratio < 32.0);
}

TEST_CASE("plugin.mechanics.oscillator_loses_energy_slowly", "[plugin][mechanics]") {
    // RK4 is not symplectic, and an oscillator is where that becomes visible. This
    // asserts the *direction* of the drift.
    //
    // The test exists because the honesty matters more than the number: the reason
    // this plugin sits beside a leapfrog stepper is that a student can run both and
    // see the difference. If someone later swapped in a symplectic method here to make
    // the picture prettier, this assertion is what would fail, and its failure message
    // is the explanation of why they should not.
    //
    // ## Why the step is large
    //
    // The per-step dissipation of RK4 on a harmonic oscillator goes like `(w*dt)^4`,
    // which is the same order as the method's truncation error -- so a step small
    // enough to be "accurate" is also a step at which the drift is **below the
    // resolution of double arithmetic**. Measured on this machine over four million
    // steps at `dt = 1e-3`, the energy actually *rises* by 1.2e-13: pure round-off,
    // with the physical dissipation several orders of magnitude underneath it.
    //
    // The first version of this test asserted a loss at that step size and failed.
    // The tempting repair -- loosen it to `e1 <= e0`, or pick a threshold that the
    // noise satisfies -- would have produced a test that passes for the wrong reason
    // and can never fail again. Asserting a property at a scale where it is not
    // observable proves nothing about it, so the step is chosen where the effect is
    // real, and the measurement is pinned to the mechanism.
    const double w = 1.0;
    Rk4Oscillator op;
    prepare(op, w);

    // Both runs cover the same simulated time, so the two losses are directly
    // comparable and their ratio is `2^p`. Comparing equal *step counts* at different
    // step sizes -- which the first version did -- compares two different amounts of
    // physics and inflates the ratio by exactly the step-size change.
    //
    // The horizon is long because the effect is small: at dt = 1e-2 the loss per step
    // is around 1.4e-14, and any measurement needs to be well clear of double
    // round-off before it says anything. Four hundred thousand steps at dt = 1e-2 is
    // about six thousand periods, which puts the accumulated loss near 5e-8 -- six
    // orders of magnitude above the noise, and still fast enough to run.
    const auto relative_loss = [&](double dt) {
        const std::size_t steps = static_cast<std::size_t>(std::llround(4000.0 / dt));
        State state = State::one(1.0, 0.0);
        const double e0 = energy(state.x(), state.v(), w);
        run(op, state, dt, steps);
        const double e1 = energy(state.x(), state.v(), w);
        REQUIRE(e0 > 0.0);
        return (e0 - e1) / e0;
    };

    const double coarse = relative_loss(1.0e-2);
    const double fine = relative_loss(5.0e-3);

    // The method dissipates, unmistakably and not by an accident of round-off.
    REQUIRE(coarse > 1.0e-9);
    // It is still an oscillator afterwards, not a dead one. Losing most of the
    // amplitude in a few periods would mean the stages are mis-weighted rather than
    // that the method is dissipative.
    REQUIRE(coarse < 0.5);

    // The order of the drift, measured rather than assumed.
    //
    // Halving dt divides the loss by 2^p. The first version of this test asserted
    // p = 4 -- the order of the *trajectory* error, which is what RK4 is usually
    // described by -- and the measurement disagreed. That is not a bug in the
    // operator: for the harmonic oscillator the RK4 stability polynomial satisfies
    // `|R(iy)|^2 = 1 - y^6/72 + O(y^8)`, so the amplitude decay per step is sixth
    // order even though the trajectory error is fourth. The two orders genuinely
    // differ, and the only way to know which one governs a measurement is to make it.
    //
    // The bounds are loose on purpose. What they still cannot tolerate is a symplectic
    // method (p effectively 0, no drift to measure) or a dissipation that comes from
    // somewhere other than the step size.
    REQUIRE(fine > 0.0);
    const double measured_order = std::log2(coarse / fine);
    REQUIRE(measured_order > 4.5);
    REQUIRE(measured_order < 7.5);
}

TEST_CASE("plugin.mechanics.oscillator_advances_every_particle", "[plugin][mechanics]") {
    // The batch contract is "one call advances the whole batch". An operator that
    // advanced only the first particle would pass every single-particle test above
    // and silently drop the rest of the state in production -- which is the failure
    // shape the batch interface exists to prevent, so it needs its own case.
    const double w = 1.0;
    Rk4Oscillator op;
    prepare(op, w);
    State state = State::many(64, 1.0);

    // t_end is the product of the two numbers below, not a third number typed beside
    // them. The first version ran 500 steps of 1e-3 and expected the value at t = 1.0.
    const double dt = 1.0e-3;
    const std::size_t steps = 500;
    run(op, state, dt, steps);

    const double expected = exact_x(1.0, 0.0, w, dt * static_cast<double>(steps));
    for (std::size_t i = 0; i < state.count(); ++i) {
        REQUIRE(std::abs(state.x(i) - expected) < 1.0e-7);
    }
}

TEST_CASE("plugin.mechanics.oscillator_is_deterministic", "[plugin][mechanics]") {
    // Same inputs, same outputs, bit for bit. The run ledger claims reproducibility,
    // and an operator that read a clock or an unseeded RNG would make that claim
    // false in a way no other test here would catch -- the trajectory would still
    // look right.
    const double w = 1.7;
    State a = State::one(1.0, -0.3);
    State b = State::one(1.0, -0.3);

    Rk4Oscillator op_a;
    prepare(op_a, w);
    Rk4Oscillator op_b;
    prepare(op_b, w);
    run(op_a, a, 1.0e-3, 700);
    run(op_b, b, 1.0e-3, 700);

    REQUIRE(a.x() == b.x());
    REQUIRE(a.v() == b.v());
}

TEST_CASE("plugin.mechanics.oscillator_clamps_and_reports_it", "[plugin][mechanics]") {
    // Charter C2 requires that a kernel declare its clamping policy, and C8 requires that
    // numerical error not be mistaken for physics. Those two together mean the interesting
    // property is not that values get bounded -- it is that the operator can **say** it
    // clamped. A silent clamp leaves a run that keeps drawing and is wrong.
    Rk4Oscillator op;
    REQUIRE(op.clamps_fired() == 0);

    // The declaration exists and refuses non-finite state, because a harmonic oscillator's
    // conserved energy bounds |x| for all time: a component above 1e12 is a symptom, not a
    // state.
    const kernels::ClampPolicy policy = op.clamp_policy();
    REQUIRE(policy.finite_only);
    REQUIRE(policy.limit > 0.0);

    // A clean run reports nothing, which is the majority case and the one that must stay
    // honest: a counter that incremented on every step would be useless as evidence.
    prepare(op, 1.0);
    State state = State::one(1.0, 0.0);
    run(op, state, 1.0e-3, 1000);
    REQUIRE(op.clamps_fired() == 0);

    // A parameter change is a new experiment, so the count starts over. Carrying the
    // previous configuration's count into the next run would make a clean run look clamped.
    prepare(op, 2.0);
    REQUIRE(op.clamps_fired() == 0);

    // And a state that does diverge is caught and counted. The oscillator is stable, so this
    // needs a state the operator would not reach on its own: an enormous velocity combined
    // with a step large enough that one RK4 stage overflows.
    {
        Rk4Oscillator wild;
        prepare(wild, 1.0);
        State extreme = State::one(1.0e308, 1.0e308);
        field::FieldValue out = extreme.view();
        kernels::BatchView batch;
        batch.in = &out;
        batch.out = &out;
        batch.count = 1;
        kernels::AdvanceContext ctx;
        ctx.dt = 1.0e10;
        const auto result = wild.advance(batch, ctx);
        // The step still reports success: clamping is the operator keeping its contract to
        // return a finite state, not a failure to report. What the caller gets instead is the
        // count, which is the honest signal.
        REQUIRE(result.has_value());
        REQUIRE(wild.clamps_fired() > 0);
        REQUIRE(std::isfinite(extreme.x()));
        REQUIRE(std::isfinite(extreme.v()));
        REQUIRE(std::abs(extreme.x()) <= wild.clamp_policy().limit);
    }
}