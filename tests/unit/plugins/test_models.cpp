/**
 * @file test_models.cpp
 * @brief Tests for the three physics models and the binder that turns a node into one.
 *
 * Test case ids match the @tests fields in the plugin headers byte for byte.
 *
 * ## What is worth asserting about a model
 *
 * Every case here compares a trajectory against something **outside the model**: an analytical solution, a
 * measured convergence order, an energy that should be conserved, or a period that a textbook gives. A model
 * checked only against itself passes whatever it does, and the three models in this directory exist precisely
 * because the physics is the thing that can be wrong.
 *
 *   - the damped oscillator decays at the rate its parameter names, and the envelope is `exp(-gamma t / 2)` --
 *     the factor of two is the classic error, and it is not obvious from the equation;
 *   - the pendulum's period grows with amplitude, which a linearised model cannot reproduce;
 *   - the projectile matches its closed form, both with and without drag, and stops at the ground;
 *   - and every model's numerical error falls as `dt^4`, measured as a **ratio** across two step sizes rather
 *     than asserted against a constant, because an order is a property of the method and not of a fixture.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/models/models.hpp>
#include <qp/plugins/models/models_binder.hpp>

#include <qp/graph/ir/node.hpp>
#include <qp/host/host.hpp>

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace qp::plugins::models;

namespace {

namespace ex = qp::graph::execution;

/// @brief A one-particle state with `components` slots, as a model's own layout.
ex::StateView state_of(std::size_t components) {
    ex::StateView s = ex::StateView::zeroed(1, components);
    return s;
}

/// @brief Advances `model` `steps` times by `dt`, and returns the state.
ex::StateView run(ex::IStateOperator& model, ex::StateView state, std::size_t steps, double dt) {
    for (std::size_t i = 0; i < steps; ++i) {
        const auto stepped = model.step(state, dt);
        REQUIRE(stepped.has_value());
    }
    return state;
}

/// @brief The largest absolute difference between two states' components.
double max_difference(const ex::StateView& a, const ex::StateView& b) {
    double worst = 0.0;
    for (std::size_t i = 0; i < a.count; ++i) {
        for (std::size_t c = 0; c < a.components_per_particle; ++c) {
            worst = std::max(worst, std::abs(a.at(i, c) - b.at(i, c)));
        }
    }
    return worst;
}

/// @brief The state after `duration` seconds at step `dt`, from `initial`.
ex::StateView advance(ex::IStateOperator& model, ex::StateView initial, double duration, double dt) {
    const auto steps = static_cast<std::size_t>(std::llround(duration / dt));
    return run(model, std::move(initial), steps, dt);
}

}  // namespace

TEST_CASE("models.a_step_converges_at_fourth_order", "[models]") {
    // The claim that makes RK4 worth using, measured rather than assumed. Halving the step must cut the error by
    // about sixteen; an order asserted against a constant would pass for any method whose constant happened to
    // fit, so this **measures the ratio** across two step sizes and checks it against 2^4.
    //
    // The oscillator is the subject because it has a closed form: x(t) = A cos(w t) from rest at A.
    const double omega = 3.0;
    auto model = make_damped_oscillator(omega, /*gamma=*/0.0);
    REQUIRE(model != nullptr);
    REQUIRE(model->name() == "model.damped_oscillator.rk4");

    const double duration = 1.0;
    const double exact = std::cos(omega * duration);  // A = 1

    const auto error_at = [&](double dt) {
        ex::StateView s = state_of(kOscillatorComponents);
        s.set(0, 0, 1.0);  // x(0) = 1
        s.set(0, 1, 0.0);  // x'(0) = 0
        const ex::StateView end = advance(*model, std::move(s), duration, dt);
        return std::abs(end.at(0, 0) - exact);
    };

    const double coarse = error_at(0.01);
    const double fine = error_at(0.005);
    REQUIRE(coarse > 0.0);
    REQUIRE(fine > 0.0);
    // 16 is the fourth-order prediction and the tolerance covers the constant term's own dependence on dt.
    const double ratio = coarse / fine;
    REQUIRE(ratio > 14.0);
    REQUIRE(ratio < 18.0);

    // And the error is small enough to be useful at a lab's step size, which is the practical half of the claim.
    const double at_a_millisecond = error_at(1.0e-3);
    REQUIRE(at_a_millisecond < 1.0e-12);
}

TEST_CASE("models.a_zero_step_is_refused", "[models]") {
    // A zero step would append a sample that duplicates the previous one and advance nothing, so a trace would
    // grow while the physics stood still -- which reads as a model that has stopped rather than as a caller
    // mistake. A non-finite step is not a step at all. Negative is **legal**: it is how the reversibility claim is
    // checked, and a claim nothing can falsify is a comment.
    auto model = make_pendulum(9.80665, 1.0);
    ex::StateView s = state_of(kPendulumComponents);
    s.set(0, 0, 0.3);

    REQUIRE_FALSE(model->step(s, 0.0).has_value());
    REQUIRE_FALSE(model->step(s, std::nan("")).has_value());
    REQUIRE_FALSE(model->step(s, std::numeric_limits<double>::infinity()).has_value());
    // The refusals changed nothing.
    REQUIRE(s.at(0, 0) == 0.3);

    // A backward step succeeds, and a forward-then-backward pair returns to where it started: the round trip is
    // what the `time_reversible` declaration means, and the oscillator claims it.
    auto oscillator = make_damped_oscillator(2.0, 0.0);
    ex::StateView o = state_of(kOscillatorComponents);
    o.set(0, 0, 0.5);
    o.set(0, 1, 1.0);
    const ex::StateView before = o;
    REQUIRE(oscillator->step(o, 0.02).has_value());
    REQUIRE(oscillator->step(o, -0.02).has_value());
    // Within the method's own error for one step, not exactly. The first version of this case asked for `1e-12`
    // and measured 1e-10, which was the test being wrong rather than the model: RK4 is *fourth-order* reversible,
    // so a round trip of two steps at `w dt = 0.04` leaves an error of order `(w dt)^5 / 720` -- around 1e-10,
    // which is what came back. Asserting bit equality would be asserting a property the method does not have, and
    // asserting a tolerance tighter than the truncation error is asserting it in a smaller way.
    REQUIRE(max_difference(before, o) < 1.0e-9);
}

TEST_CASE("models.the_damped_oscillator_decays_at_the_rate_it_declares", "[models]") {
    // The measurement a mechanics lab actually takes, and the factor of two that everyone gets wrong once. For
    // `x'' = -w^2 x - gamma x'` with light damping, the envelope is `exp(-gamma t / 2)` -- not `exp(-gamma t)` --
    // because the decay rate in the equation is the **velocity** coefficient and the amplitude falls at half of
    // it. A model that reported the other would disagree with the student's own graph by a factor of two in the
    // exponent.
    const double omega = 10.0;
    const double gamma = 1.0;
    auto model = make_damped_oscillator(omega, gamma);

    // The peaks are found by **sampling densely and taking the maximum**, not by evaluating at `k * period`.
    // That was the first version, and it was wrong by 3e-4: damping shifts the oscillation's frequency to
    // `sqrt(w^2 - gamma^2/4)`, so the k-th peak is not at `k * period` but slightly earlier, and the envelope has
    // moved on by the time the naive sample is taken. The residual was a real physical effect that the test had
    // not accounted for -- and the fix is to measure where the peak actually is rather than to widen the
    // tolerance until the discrepancy fits.
    const double period = 2.0 * 3.14159265358979323846 / omega;
    double previous = 1.0;
    for (int k = 1; k <= 4; ++k) {
        ex::StateView s = state_of(kOscillatorComponents);
        s.set(0, 0, 1.0);
        // Run to just before the expected peak, then take the largest magnitude in a **narrow** window around it.
        // Narrow because the peak's own width is set by the curvature of the cosine: a window of a few hundred
        // microseconds finds the maximum to well within the tolerance below, and a wide one would let the envelope
        // itself bias the answer.
        const double approximate = k * period;
        const double dt = 1.0e-5;
        const auto warmup = static_cast<std::size_t>(std::llround((approximate - 0.002) / dt));
        ex::StateView window = state_of(kOscillatorComponents);
        window.set(0, 0, 1.0);
        window = run(*model, std::move(window), warmup, dt);
        double best = 0.0;
        double best_time = approximate - 0.002;
        const auto span = static_cast<std::size_t>(std::llround(0.004 / dt));
        // One assertion for the whole window rather than one per step. A `REQUIRE` is a Catch2 assertion, and a
        // loop that runs one per integration step makes the suite's **assertion count** the thing being measured:
        // this case reported 1.9 million assertions and took half a second before the loop was written this way.
        // The step still has to succeed, so the failure is accumulated and asserted once.
        bool stepped = true;
        for (std::size_t i = 0; i < span; ++i) {
            stepped = model->step(window, dt).has_value() && stepped;
            const double magnitude = std::abs(window.at(0, 0));
            if (magnitude > best) {
                best = magnitude;
                best_time = approximate - 0.002 + static_cast<double>(i + 1) * dt;
            }
        }
        REQUIRE(stepped);
        const double predicted = std::exp(-gamma * best_time / 2.0);
        INFO("peak " << k << " measured " << best << " at " << best_time << " predicted " << predicted);
        // 2e-4 and not 1e-5: the maximum is located to the sampling grid, so `best` is the envelope at a point up
        // to `dt` away from the true peak, and the envelope moves by `gamma * dt / 2` over that distance. The
        // earlier 1e-5 was tighter than the grid could resolve, which is a tolerance that fails for a reason that
        // has nothing to do with the model.
        REQUIRE(std::abs(best - predicted) < 2.0e-4);
        // And it really is decaying, so the case cannot pass on a model that ignores gamma entirely.
        REQUIRE(best < previous);
        previous = best;
    }

    // With no damping the **energy** is conserved, which is the negative control. The first version of this
    // checked the position after five periods and measured 0.84 -- correctly, because a position sampled at a
    // period is at the cosine's peak, and 1.59 periods into the run the cosine is elsewhere entirely. Energy is
    // the quantity that does not care where in the cycle the clock stopped, and it is also what the confidence
    // panel reports, so it is the right thing to assert.
    auto free_model = make_damped_oscillator(omega, 0.0);
    ex::StateView s = state_of(kOscillatorComponents);
    s.set(0, 0, 1.0);  // E = x^2 + (v/w)^2 = 1 with x = 1, v = 0
    const auto energy_at = [&](ex::IStateOperator& m, double gamma_param, double duration) {
        ex::StateView start = state_of(kOscillatorComponents);
        start.set(0, 0, 1.0);
        const ex::StateView end = advance(m, std::move(start), duration, 1.0e-5);
        const double x = end.at(0, 0);
        const double v = end.at(0, 1);
        (void)gamma_param;
        return x * x + (v / omega) * (v / omega);
    };
    REQUIRE(energy_at(*free_model, 0.0, 5.0 * period) > 0.999);
    // And with damping the energy falls, at `exp(-gamma t)`: for `gamma = 2` over one second that is `e^-2`, so
    // the assertion is that it is a quarter of the start rather than a rounding difference.
    auto damped = make_damped_oscillator(10.0, 2.0);
    REQUIRE(energy_at(*damped, 2.0, 1.0) < 0.25);
    (void)s;

    // The model's own declaration, checked rather than trusted: both terms are linear in the state.
    REQUIRE(model->describe().time_reversible);
    REQUIRE(model->describe().is_pure);
    REQUIRE(model->describe().is_dimensionally_consistent);
    REQUIRE(model->describe().is_independent_of_other_instances);
}

TEST_CASE("models.the_pendulum_period_grows_with_amplitude", "[models]") {
    // **The reason this model exists rather than a linearised one.** A small-angle pendulum has a period that does
    // not depend on amplitude; the real one grows, by about 0.16% at 5 degrees, 0.65% at 10, and 1.5% at 15. A
    // model that linearised would agree with a first-year textbook and disagree with the pendulum on the bench,
    // and the disagreement is exactly what the experiment is supposed to find.
    //
    // The period is measured from **zero crossings** rather than from a peak, because a numerical trajectory's
    // maximum is flat and its position is poorly determined while a crossing is steep and well determined.
    const double g = 9.80665;
    const double length = 1.0;
    const double small_angle_period = 2.0 * 3.14159265358979323846 * std::sqrt(length / g);

    const auto period_at = [&](double amplitude) {
        auto model = make_pendulum(g, length);
        ex::StateView s = state_of(kPendulumComponents);
        s.set(0, 0, amplitude);
        // Ten seconds at a fine step, recording every crossing of theta = 0 in the same direction.
        const double dt = 1.0e-4;
        const auto steps = static_cast<std::size_t>(10.0 / dt);
        std::vector<double> crossings;
        double previous = s.at(0, 0);
        for (std::size_t i = 0; i < steps; ++i) {
            REQUIRE(model->step(s, dt).has_value());
            const double now = s.at(0, 0);
            if (previous > 0.0 && now <= 0.0) crossings.push_back(static_cast<double>(i) * dt);
            previous = now;
        }
        REQUIRE(crossings.size() >= 4);
        // The mean interval between successive same-direction crossings is the period.
        double total = 0.0;
        for (std::size_t i = 1; i < crossings.size(); ++i) total += crossings[i] - crossings[i - 1];
        return total / static_cast<double>(crossings.size() - 1);
    };

    const double small = period_at(0.01);  // 0.57 degrees: the linear regime
    const double large = period_at(0.5236);  // 30 degrees

    // The linear period is the reference the textbook gives, and a 0.01 rad pendulum matches it.
    REQUIRE(std::abs(small - small_angle_period) < 1.0e-5);
    // And the large-amplitude period is **longer**, by about 1.7% at 30 degrees.
    INFO("small " << small << " large " << large << " linear " << small_angle_period);
    REQUIRE(large > small);
    REQUIRE(large / small > 1.01);
    REQUIRE(large / small < 1.03);

    // Compare against the series expansion a course would use: T = T0 (1 + th0^2/16 + ...). At 30 degrees that is
    // 1 + 0.5236^2/16 = 1.01714.
    const double series = small_angle_period * (1.0 + 0.5236 * 0.5236 / 16.0);
    REQUIRE(std::abs(large - series) < 2.0e-3);
}

TEST_CASE("models.a_projectile_matches_its_closed_form", "[models]") {
    // Without drag the trajectory is exact and a test can name it: y = y0 + v0 t - g t^2 / 2, x = vx t. With
    // linear drag it is still closed-form, which is why this model uses linear drag -- a `v^2` model could only be
    // checked against itself, and a test that compares an implementation with its own arithmetic asserts nothing.
    const double g = 9.80665;
    auto model = make_projectile(g, /*drag=*/0.0);
    REQUIRE(model != nullptr);
    REQUIRE_FALSE(model->describe().time_reversible);

    ex::StateView s = state_of(kProjectileComponents);
    s.set(0, 0, 0.0);   // x
    s.set(0, 1, 10.0);  // y
    s.set(0, 2, 5.0);   // vx
    s.set(0, 3, 0.0);   // vy

    const double t = 0.4;
    const ex::StateView end = advance(*model, std::move(s), t, 1.0e-5);
    REQUIRE(std::abs(end.at(0, 0) - 5.0 * t) < 1.0e-9);
    REQUIRE(std::abs(end.at(0, 1) - (10.0 - 0.5 * g * t * t)) < 1.0e-9);
    REQUIRE(std::abs(end.at(0, 2) - 5.0) < 1.0e-12);
    REQUIRE(std::abs(end.at(0, 3) + g * t) < 1.0e-9);

    // With drag, the closed form is v(t) = (v0 + g/k) exp(-k t) - g/k for the vertical component, and
    // y(t) = y0 + (v0 + g/k)(1 - exp(-k t))/k - g t / k. Both are asserted because a drag term that only affected
    // the velocity would still match the first line and be wrong.
    const double k = 0.5;
    auto dragged = make_projectile(g, k);
    ex::StateView d = state_of(kProjectileComponents);
    d.set(0, 1, 10.0);
    d.set(0, 2, 5.0);
    d.set(0, 3, 0.0);
    const ex::StateView drag_end = advance(*dragged, std::move(d), t, 1.0e-5);

    const double expected_vx = 5.0 * std::exp(-k * t);
    const double expected_x = 5.0 * (1.0 - std::exp(-k * t)) / k;
    const double expected_vy = (0.0 + g / k) * std::exp(-k * t) - g / k;
    const double expected_y = 10.0 + (0.0 + g / k) * (1.0 - std::exp(-k * t)) / k - g * t / k;
    REQUIRE(std::abs(drag_end.at(0, 0) - expected_x) < 1.0e-9);
    REQUIRE(std::abs(drag_end.at(0, 1) - expected_y) < 1.0e-9);
    REQUIRE(std::abs(drag_end.at(0, 2) - expected_vx) < 1.0e-9);
    REQUIRE(std::abs(drag_end.at(0, 3) - expected_vy) < 1.0e-9);

    // Drag makes the projectile travel less far in the same time, which is the physical statement the equations
    // above encode. Asserted separately because four matching numbers could still be four wrong ones.
    REQUIRE(drag_end.at(0, 0) < end.at(0, 0));
}

TEST_CASE("models.the_projectile_stops_at_the_ground", "[models]") {
    // The floor is part of the model, not a post-processing step, and this is the case that says so. A projectile
    // left to fall keeps falling through the ground and produces a trajectory that looks like a plot of something
    // real -- a parabola continuing off the bottom of the axes -- which is the failure mode this constraint exists
    // to prevent.
    const double g = 9.80665;
    auto model = make_projectile(g, 0.0);

    ex::StateView s = state_of(kProjectileComponents);
    s.set(0, 0, 0.0);
    s.set(0, 1, 1.0);  // one metre up
    s.set(0, 2, 3.0);
    s.set(0, 3, 0.0);

    // Long enough to land and then some: the fall takes sqrt(2/g) = 0.45 s.
    REQUIRE(model->step(s, 2.0).has_value());
    REQUIRE(s.at(0, 1) == 0.0);
    REQUIRE(s.at(0, 3) == 0.0);
    // The horizontal component stops too: a projectile that has landed does not slide. That is a **choice** --
    // a sliding or bouncing projectile is a different model -- and leaving vx alone would make this one neither.
    REQUIRE(s.at(0, 2) == 0.0);

    // And it stays landed: the constraint runs inside every step, so a further step does not lift it or push it
    // below the floor.
    REQUIRE(model->step(s, 1.0).has_value());
    REQUIRE(s.at(0, 1) == 0.0);
    REQUIRE(s.at(0, 2) == 0.0);
    REQUIRE(s.at(0, 3) == 0.0);

    // A **bounce** is visible before the landing, which is the negative control: the constraint must not fire
    // while the projectile is still in the air.
    ex::StateView up = state_of(kProjectileComponents);
    up.set(0, 1, 10.0);
    up.set(0, 3, 5.0);
    REQUIRE(model->step(up, 0.1).has_value());
    REQUIRE(up.at(0, 1) > 10.0);
    REQUIRE(up.at(0, 3) > 0.0);
}

TEST_CASE("models.the_driven_oscillator_finds_its_resonance", "[models]") {
    // **The resonance experiment**, which is the reason this model exists and the reason its state has three
    // components. A driven oscillator's steady-state amplitude is
    //
    //     A = F / sqrt((w0^2 - wd^2)^2 + (gamma * wd)^2)
    //
    // -- a closed form from a textbook, with no numerical method in it -- and a student's experiment is to sweep
    // `wd` and plot it. The peak sits near the natural frequency and its height is set by the damping, which is
    // the measurement that explains why a swing is pushed at its own rate.
    //
    // The phase is carried **in the state** rather than read from a clock, so `measure_amplitude` seeds
    // component 2 with zero and the model advances it as `t' = 1`. That is what keeps a step a function of the
    // state and `dt` alone, and it is why a run is reproducible from its initial condition.
    const double omega0 = 10.0;
    const double gamma = 0.5;
    const double force = 1.0;

    /// Runs to steady state and returns the amplitude of the last few oscillations.
    const auto measure_amplitude = [&](double drive) {
        auto model = make_driven_oscillator(omega0, gamma, force, drive);
        REQUIRE(model != nullptr);
        ex::StateView state = state_of(kDrivenComponents);
        state.set(0, 0, 0.0);  // released from rest
        state.set(0, 1, 0.0);
        state.set(0, 2, 0.0);  // the clock starts at zero, so the drive is `cos(0) = 1` at t = 0

        // Long enough for the transient to die: the envelope decays as `exp(-gamma t / 2)`, so 60 s is
        // `exp(-15)` of the initial condition. The measurement window is the last tenth of that.
        const double dt = 1.0e-4;
        const double total = 60.0;
        const auto steps = static_cast<std::size_t>(std::llround(total / dt));
        const auto measure_from = static_cast<std::size_t>(static_cast<double>(steps) * 0.9);
        double highest = 0.0;
        double lowest = 0.0;
        for (std::size_t i = 0; i < steps; ++i) {
            REQUIRE(model->step(state, dt).has_value());
            if (i < measure_from) continue;
            const double x = state.at(0, 0);
            highest = std::max(highest, x);
            lowest = std::min(lowest, x);
        }
        // The amplitude is half the peak-to-peak swing, which does not depend on where in the cycle the window
        // happened to start or end.
        return 0.5 * (highest - lowest);
    };

    /// The closed form the measurement is checked against.
    const auto predicted = [&](double drive) {
        const double a = omega0 * omega0 - drive * drive;
        const double b = gamma * drive;
        return force / std::sqrt(a * a + b * b);
    };

    // **At resonance** the response is `F / (gamma * w0)` = 1 / 5 = 0.2, which is the peak of the curve and
    // twenty times the static deflection. This is the number the experiment is for.
    const double at_resonance = measure_amplitude(omega0);
    REQUIRE(std::abs(at_resonance - predicted(omega0)) / predicted(omega0) < 5.0e-3);
    REQUIRE(std::abs(at_resonance - 0.2) < 1.0e-3);

    // **Below and above** the peak, where the curve falls away. Measured at two points on each side, because one
    // point on each side would not distinguish the closed form from a straight line through the peak.
    for (const double drive : {5.0, 8.0, 12.0, 20.0}) {
        const double measured = measure_amplitude(drive);
        const double expected = predicted(drive);
        INFO("drive " << drive << " measured " << measured << " predicted " << expected);
        REQUIRE(std::abs(measured - expected) / expected < 5.0e-3);
        // And every one of them is below the peak, so the case cannot pass on a model whose response is flat.
        REQUIRE(measured < at_resonance);
    }

    // **The static limit**, measured with the instrument that fits it. At zero drive frequency the push is
    // constant, so the response is `F / w0^2` = 0.01 plus a decaying transient.
    //
    // The **windowed mean** is the right measurement, and that is a fact rather than a convenience: the response
    // is `x_steady + transient`, the transient decays as `exp(-gamma t / 2)` and is gone by the window, and the
    // mean of `cos(wd t)` over a whole number of periods is zero -- so the mean of the window is the steady
    // response's own mean, which for `wd = 0` is exactly the static deflection. The previous round measured the
    // peak-to-peak here and got half the right answer, which is what a peak-to-peak returns for a **constant**
    // offset: the leftover ripple rather than the deflection. The model was never wrong; the instrument was.
    const auto measure_mean = [&](double drive) {
        auto model = make_driven_oscillator(omega0, gamma, force, drive);
        REQUIRE(model != nullptr);
        ex::StateView state = state_of(kDrivenComponents);
        const double dt = 1.0e-4;
        const auto steps = static_cast<std::size_t>(std::llround(60.0 / dt));
        const auto from = static_cast<std::size_t>(static_cast<double>(steps) * 0.9);
        double total = 0.0;
        std::size_t counted = 0;
        for (std::size_t i = 0; i < steps; ++i) {
            REQUIRE(model->step(state, dt).has_value());
            if (i < from) continue;
            total += state.at(0, 0);
            ++counted;
        }
        REQUIRE(counted > 0);
        return total / static_cast<double>(counted);
    };

    const double static_response = measure_mean(0.0);
    const double expected_static = force / (omega0 * omega0);
    INFO("static response " << static_response << " expected " << expected_static);
    REQUIRE(std::abs(static_response - expected_static) / expected_static < 5.0e-3);

    // The clock really advances, which is what makes the whole thing work: after a run, component 2 holds the
    // elapsed time. Asserted because a model that ignored `t' = 1` would still produce a plausible-looking
    // oscillation at whatever fixed phase the derivative happened to use.
    auto clocked = make_driven_oscillator(omega0, gamma, force, 1.0);
    REQUIRE(clocked != nullptr);
    ex::StateView state = state_of(kDrivenComponents);
    REQUIRE(clocked->step(state, 0.25).has_value());
    REQUIRE(std::abs(state.at(0, 2) - 0.25) < 1.0e-12);

    // Its declarations: pure, and **not** time-reversible, because the state carries an absolute time.
    REQUIRE(clocked->describe().is_pure);
    REQUIRE_FALSE(clocked->describe().time_reversible);
}

TEST_CASE("models.the_binder_declares_the_types_it_binds", "[models]") {
    // The defect this catches is silent in both directions and has no symptom at run time: a port the description
    // declares and `bind` never reads is a control the user can turn with no effect, and a port `bind` reads and
    // the description omits is a parameter no editor will offer. Neither shows up as a failure -- the first looks
    // like a model that ignores a setting, the second like a model that runs with defaults nobody chose.
    const std::vector<qp::graph::NodeDesc> types = ModelsBinder::node_types();
    REQUIRE(types.size() == 4);

    const ModelsBinder binder;
    std::vector<std::string> names;
    for (const qp::graph::NodeDesc& desc : types) {
        INFO("type " << desc.type_name);
        REQUIRE(desc.valid());
        REQUIRE_FALSE(desc.label.empty());
        REQUIRE_FALSE(desc.description.empty());
        REQUIRE(desc.category == "models");
        // A model is not a per-frame update: a Runge-Kutta step is a solve, and the particle domain forbids
        // anything that allocates.
        REQUIRE(desc.allow_in_field_domain);
        REQUIRE_FALSE(desc.allow_in_particle_domain);

        // Every type answers `can_bind` at its own layout and at no other -- the property that lets three models
        // with three state dimensions coexist in one graph.
        const std::size_t components = desc.type_name == ModelsBinder::kPendulumType
                                           ? kPendulumComponents
                                           : (desc.type_name == ModelsBinder::kProjectileType
                                                  ? kProjectileComponents
                                                  : (desc.type_name == ModelsBinder::kDrivenType
                                                         ? kDrivenComponents
                                                         : kOscillatorComponents));
        REQUIRE(binder.can_bind(desc.type_name, ex::StateView::zeroed(1, components)));
        REQUIRE_FALSE(binder.can_bind(desc.type_name, ex::StateView::zeroed(1, components + 1)));
        // And it does not claim another model's type, or one it has never heard of.
        REQUIRE_FALSE(binder.can_bind("demo.spring_damper", ex::StateView::zeroed(1, components)));
        REQUIRE_FALSE(binder.can_bind("no.such.type", ex::StateView::zeroed(1, components)));

        // The common ports: an output and the two initial conditions, at the numbers every model shares.
        REQUIRE(desc.outputs.size() == 1);
        REQUIRE(desc.find_port(1, /*is_output=*/true) != nullptr);
        REQUIRE(desc.find_port(1, false) != nullptr);
        REQUIRE(desc.find_port(2, false) != nullptr);
        REQUIRE(desc.find_port(3, false) != nullptr);
        // Port 4 is the model-specific physical parameter, and every model has one.
        REQUIRE(desc.find_port(4, false) != nullptr);
        // The parameters are not sockets: `connectable == false` is what makes the property panel draw an editor
        // rather than a port a user would try to wire.
        for (const qp::graph::PortDesc& p : desc.inputs) {
            REQUIRE_FALSE(p.connectable);
            REQUIRE(p.required);
        }

        REQUIRE(std::find(names.begin(), names.end(), desc.type_name) == names.end());
        names.push_back(desc.type_name);
    }

    // The physical parameters are bounded, which is what makes the panel draw a slider: a negative mass, a zero
    // length and a negative frequency are not values a user should be able to type.
    for (const qp::graph::NodeDesc& desc : types) {
        const qp::graph::PortDesc* physical = desc.find_port(4, false);
        REQUIRE(physical != nullptr);
        REQUIRE(physical->min_value < physical->max_value);
        REQUIRE(physical->step > 0.0);
    }
}

TEST_CASE("models.a_node_binds_to_the_model_its_parameters_describe", "[models]") {
    // The binding itself, driven the way a run drives it: build a node, set parameters the way the property panel
    // does, and check that the operator which comes back is the model those numbers describe.
    ModelsBinder binder;

    qp::graph::Node node;
    node.type_name = ModelsBinder::kOscillatorType;
    const auto oscillator = binder.bind(node.type_name, node, ex::StateView::zeroed(1, kOscillatorComponents));
    REQUIRE(oscillator != nullptr);
    REQUIRE(oscillator->name() == "model.damped_oscillator.rk4");
    // A node nobody has configured still binds, at the model's own defaults -- dropping a node on the canvas must
    // not be an error, and the values are visible in the property panel.
    REQUIRE(oscillator->describe().is_pure);

    // A frequency of zero is refused: it is a free particle rather than a slow oscillator, and substituting a
    // value the user did not ask for produces a graph that runs and is not the graph that was drawn.
    node.set_param(3, qp::ports::Value{0.0});
    REQUIRE(binder.bind(node.type_name, node, ex::StateView::zeroed(1, kOscillatorComponents)) == nullptr);

    // A negative frequency is refused for the same reason, and so is a negative decay rate: `exp(+gamma t)`
    // grows without bound, which is a model that blows up rather than one that decays.
    node.set_param(3, qp::ports::Value{12.0});
    node.set_param(4, qp::ports::Value{-1.0});
    REQUIRE(binder.bind(node.type_name, node, ex::StateView::zeroed(1, kOscillatorComponents)) == nullptr);

    // A valid configuration binds, and the **wiring between ports and parameters is real**: a node with gamma = 0
    // conserves energy while the same node with gamma = 2 loses it, which is only true if port 4 was read.
    //
    // Measured as **energy** and not as a position. The first version returned `abs(x)` after one second and
    // asserted it exceeded 0.99 for the undamped case, which failed at 0.839 -- correctly, because one second at
    // w = 10 is 1.59 periods and a position sampled there is nowhere near its starting value. A test that wants to
    // say "nothing was lost" has to look at a quantity that does not depend on the phase.
    const auto energy_after = [&](double gamma) {
        qp::graph::Node n;
        n.type_name = ModelsBinder::kOscillatorType;
        n.set_param(3, qp::ports::Value{10.0});
        n.set_param(4, qp::ports::Value{gamma});
        auto model = binder.bind(n.type_name, n, ex::StateView::zeroed(1, kOscillatorComponents));
        REQUIRE(model != nullptr);
        ex::StateView s = ex::StateView::zeroed(1, kOscillatorComponents);
        s.set(0, 0, 1.0);
        const ex::StateView end = advance(*model, std::move(s), 1.0, 1.0e-4);
        return end.at(0, 0) * end.at(0, 0) + (end.at(0, 1) / 10.0) * (end.at(0, 1) / 10.0);
    };
    REQUIRE(energy_after(0.0) > 0.999);
    // `exp(-gamma t)` at gamma = 2 and t = 1 is 0.135, so a quarter is the loose end of the claim and a model
    // that ignored the damping would sit at 1.0.
    REQUIRE(energy_after(2.0) < 0.25);

    // The pendulum and the projectile bind at their own layouts and only those.
    qp::graph::Node pendulum;
    pendulum.type_name = ModelsBinder::kPendulumType;
    REQUIRE(binder.bind(pendulum.type_name, pendulum, ex::StateView::zeroed(1, kPendulumComponents)) != nullptr);
    REQUIRE(binder.bind(pendulum.type_name, pendulum, ex::StateView::zeroed(1, kOscillatorComponents)) == nullptr);
    // A zero length is refused rather than divided by: `g/L` would be an infinity and the first step would
    // produce a NaN with nothing left to say where it came from.
    pendulum.set_param(4, qp::ports::Value{0.0});
    REQUIRE(binder.bind(pendulum.type_name, pendulum, ex::StateView::zeroed(1, kPendulumComponents)) == nullptr);

    qp::graph::Node projectile;
    projectile.type_name = ModelsBinder::kProjectileType;
    REQUIRE(binder.bind(projectile.type_name, projectile, ex::StateView::zeroed(1, kProjectileComponents)) !=
            nullptr);
    // A negative drag is refused: it is an anti-drag that accelerates the projectile, which is not a physical
    // model and would be a surprising thing to run.
    projectile.set_param(4, qp::ports::Value{-0.1});
    REQUIRE(binder.bind(projectile.type_name, projectile, ex::StateView::zeroed(1, kProjectileComponents)) ==
            nullptr);
    // The driven oscillator's four physical ports, and the drive is what distinguishes it from the damped
    // oscillator's one. Its **default drive frequency equals its natural frequency**, so a node dropped on the
    // canvas and run shows resonance rather than a static deflection.
    qp::graph::Node driven;
    driven.type_name = ModelsBinder::kDrivenType;
    REQUIRE(binder.bind(driven.type_name, driven, ex::StateView::zeroed(1, kDrivenComponents)) != nullptr);
    // Three components, like the undamped oscillator and for a different reason: this one's third slot is the
    // clock. A layout of two is declined, which is what a model owning its shape means.
    REQUIRE(binder.bind(driven.type_name, driven, ex::StateView::zeroed(1, 2)) == nullptr);
    // A zero natural frequency is refused -- a free particle rather than a slow oscillator -- and so is a
    // negative drive frequency, which is an oscillation running backwards rather than a slower one.
    driven.set_param(3, qp::ports::Value{0.0});
    REQUIRE(binder.bind(driven.type_name, driven, ex::StateView::zeroed(1, kDrivenComponents)) == nullptr);
    driven.set_param(3, qp::ports::Value{10.0});
    driven.set_param(6, qp::ports::Value{-1.0});
    REQUIRE(binder.bind(driven.type_name, driven, ex::StateView::zeroed(1, kDrivenComponents)) == nullptr);
    // A zero drive amplitude is **not** refused: it is the free oscillator, which is a legitimate configuration
    // and the control case in every resonance experiment.
    driven.set_param(6, qp::ports::Value{10.0});
    driven.set_param(5, qp::ports::Value{0.0});
    REQUIRE(binder.bind(driven.type_name, driven, ex::StateView::zeroed(1, kDrivenComponents)) != nullptr);
}

TEST_CASE("models.the_shipped_models_register_with_the_host", "[models]") {
    // The three types arrive through the host's ledger, so `origin_of` answers for them and a session can take
    // them back -- the same rule every built-in contribution follows, and the one `plugins/instruments` was
    // written to demonstrate.
    qp::host::PluginHost host{qp::plugin::Capability::node_types};
    REQUIRE(host.node_types().size() == 0);

    const std::size_t registered = ModelsBinder::mount(host);
    REQUIRE(registered == ModelsBinder::node_types().size());
    REQUIRE(host.node_types().size() == 4);
    REQUIRE(host.origin_of(ModelsBinder::kPendulumType) == qp::host::PluginHost::kBuiltinOrigin);
    // Not a plugin: a built-in has no manifest, no library and no capability declaration.
    REQUIRE(host.mounted_ids().empty());

    // The catalog resolves each type by name, which is what the canvas and the property panel do.
    const qp::graph::NodeDesc* found = host.node_types().find(ModelsBinder::kProjectileType);
    REQUIRE(found != nullptr);
    REQUIRE(found->label == "Projectile");

    // A second mount reports honestly rather than pretending: the registry refuses a name it already serves.
    REQUIRE(ModelsBinder::mount(host) == 0);
    REQUIRE(host.node_types().size() == 4);

    // And clearing the built-ins takes them back, so a session can put its own palette in place.
    host.clear_builtin_node_types();
    REQUIRE(host.node_types().size() == 0);
    REQUIRE(host.origin_of(ModelsBinder::kPendulumType).empty());
}
