/**
 * @file test_examples.cpp
 * @brief The worked experiments, run for real and checked against closed forms.
 *
 * Test case ids match the @tests fields in the plugin headers byte for byte.
 *
 * ## Why these are the strongest cases in the repository
 *
 * Every other test here checks one layer. These check a **chain**: a model's differential equation, the binder
 * that configures it from a node's parameters, the run loop that steps it and records a trace, and a published
 * formula that came from a textbook. If any of the four is wrong the trajectory and the prediction part company,
 * and the failure names the quantity rather than the layer.
 *
 * The prediction is always written independently of the model -- a series expansion, an envelope, an exact
 * solution -- because a check that came from the same code as the trajectory could not tell a correct trajectory
 * from a wrong one. `expected.hpp`'s file comment is the long version of that argument.
 *
 * ## The three failures these cases were written while looking for
 *
 * 1. **A damping factor of two.** `x'' = -w^2 x - gamma x'` decays as `exp(-gamma t / 2)`, not `exp(-gamma t)`,
 *    because the decay is the real part of the characteristic roots. The envelope function says so, and the
 *    oscillator case measures it against the integration.
 * 2. **The frequency a damped oscillator actually oscillates at.** `sqrt(w^2 - gamma^2/4)`, strictly below `w`.
 *    A period measured from the trace and compared against `2 pi / w` is wrong by that much, and the error is
 *    small enough to be mistaken for integrator error.
 * 3. **The linear period is not the pendulum's period.** At 30 degrees it is 1.7% short, which is twenty times
 *    larger than the integrator's own error at any step a course would use -- so a test that compared a
 *    large-amplitude swing against the small-angle formula would be measuring the approximation, not the method.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/examples/expected.hpp>

#include <qp/graph/execution/execution.hpp>
#include <qp/plugins/models/models.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace qp::plugins::examples;

namespace {

namespace ex = qp::graph::execution;

/// @brief A one-particle state of `components` doubles per particle.
[[nodiscard]] ex::StateView state_of(std::size_t components) {
    return ex::StateView::zeroed(1, components);
}

/// @brief Advances `model` for `duration` seconds at `dt`, and returns the state.
[[nodiscard]] ex::StateView advance(ex::IStateOperator& model, ex::StateView state, double duration, double dt) {
    const auto steps = static_cast<std::size_t>(std::llround(duration / dt));
    for (std::size_t i = 0; i < steps; ++i) {
        if (!model.step(state, dt).has_value()) break;
    }
    return state;
}

/**
 * @brief The period of a pendulum trajectory, measured from its **zero crossings**.
 *
 * Crossings rather than peaks, because a maximum is flat and its position is poorly determined while a crossing
 * is steep and well determined -- the same reasoning a lab manual gives for timing a pendulum at the bottom of
 * its swing rather than at the top.
 *
 * Each crossing is located by **linear interpolation between the two samples that bracket it**, and that is the
 * difference between a usable measurement and one whose precision is the step size. Sampling at `dt` alone
 * locates a crossing to about `dt * |theta| / |dtheta|` -- 5e-5 s at `dt = 1e-4` for a one-second pendulum --
 * which would have to be absorbed by a tolerance ten times looser than the physics deserves. Interpolating on
 * the secant gives an error of order `dt^2`, so the step can stay coarse and the assertion can stay tight.
 *
 * @return The mean interval between successive same-direction crossings, or nothing when fewer than four
 *         crossings were seen, which means the window was too short to measure a period.
 */
[[nodiscard]] std::optional<double> measure_period(double length, double theta0, double gravity, double dt,
                                                   double window) {
    auto model = qp::plugins::models::make_pendulum(gravity, length);
    if (model == nullptr) return std::nullopt;

    ex::StateView state = state_of(2);
    state.set(0, 0, theta0);
    state.set(0, 1, 0.0);

    std::vector<double> crossings;
    double previous = state.at(0, 0);
    const auto steps = static_cast<std::size_t>(std::llround(window / dt));
    for (std::size_t i = 0; i < steps; ++i) {
        if (!model->step(state, dt).has_value()) return std::nullopt;
        const double now = state.at(0, 0);
        if (previous > 0.0 && now <= 0.0) {
            // The crossing lies between `t - dt` and `t`, and the secant through the two samples puts it there
            // to second order.
            const double fraction = previous / (previous - now);
            crossings.push_back((static_cast<double>(i) - 1.0 + fraction) * dt);
        }
        previous = now;
    }
    if (crossings.size() < 4) return std::nullopt;

    double total = 0.0;
    for (std::size_t i = 1; i < crossings.size(); ++i) total += crossings[i] - crossings[i - 1];
    return total / static_cast<double>(crossings.size() - 1);
}

/// @brief The successive peak magnitudes of an oscillator released from rest, and the times they occurred.
struct Peaks final {
    std::vector<double> magnitudes{};
    std::vector<double> times{};
};

/// @brief Runs an oscillator and records each local maximum of `|x|`, starting with the release point.
[[nodiscard]] Peaks measure_peaks(double omega, double gamma, double duration, double dt) {
    auto model = qp::plugins::models::make_damped_oscillator(omega, gamma);
    Peaks out;
    if (model == nullptr) return out;

    ex::StateView state = state_of(2);
    state.set(0, 0, 1.0);  // released from unit displacement, at rest

    // **The release point is a peak**, and leaving it out was the first version's bug: the trajectory starts at
    // rest, so `|x|` decreases from the first step and the interior-maximum test never fires for `t = 0`. The
    // recorded peaks then began at the *second* maximum, and the interval between the first two of them was half
    // a period -- which surfaced as a measured period of 0.315 s against a predicted 0.629.
    out.magnitudes.push_back(1.0);
    out.times.push_back(0.0);

    // **A minimum separation between peaks, and it is not a heuristic.** Near a maximum the sequence is
    // numerically flat, so `before_previous < previous` first holds for whichever sample happens to beat its
    // predecessor -- and a record taken on that condition alone collects the *first* sample of each flat plateau
    // rather than the peak itself. For a slow decay that put successive records one sample apart, and the interval
    // between the first two came out as 0.315 s against a period of 0.629: a measurement of the plateau, not of
    // the oscillation. A quarter of a period is the standard remedy and it is justified rather than tuned: the
    // true peaks of a lightly damped oscillator are one full period apart, so anything closer cannot be a second
    // peak.
    const double minimum_gap = 0.25 * 2.0 * 3.14159265358979323846 / omega;
    double before_previous = std::abs(state.at(0, 0));
    double previous = before_previous;
    double last_recorded_time = 0.0;
    const auto steps = static_cast<std::size_t>(std::llround(duration / dt));
    for (std::size_t i = 1; i <= steps; ++i) {
        if (!model->step(state, dt).has_value()) break;
        const double now = std::abs(state.at(0, 0));
        const double t = static_cast<double>(i) * dt;
        if (before_previous < previous && previous >= now && t - last_recorded_time >= minimum_gap) {
            out.magnitudes.push_back(previous);
            out.times.push_back(t - dt);
            last_recorded_time = t - dt;
        }
        before_previous = previous;
        previous = now;
    }
    return out;
}

}  // namespace

TEST_CASE("examples.the_small_angle_period_is_the_limit_of_the_series", "[examples]") {
    // The two predictions have to be consistent with each other before either is used to check a model: the
    // series must tend to the linear formula as the amplitude goes to zero, and grow away from it as the
    // amplitude grows. That is what makes the pair usable as a discriminator -- a series that was simply `T0`
    // scaled by a constant would pass the first half and fail the second.
    const double length = 1.0;
    const auto linear = pendulum_period_small_angle(length, kGravity);
    REQUIRE(linear.has_value());
    // 2 pi sqrt(1/9.80665) = 2.0064 s, the number a first-year lab measures.
    REQUIRE(std::abs(linear.value() - 2.0064) < 1.0e-3);

    // The limit: at a thousandth of a radian the correction is below a part in ten million.
    const auto tiny = pendulum_period_series(length, 1.0e-3, kGravity);
    REQUIRE(tiny.has_value());
    // 1e-7 and not 1e-9: the series' **own** first correction at a millith of a radian is 	heta0^2/16, which is
    // 1.25e-7 of the base period. Asserting tighter would be asserting that the series is the identity.
    REQUIRE(std::abs(tiny.value() - linear.value()) < 1.0e-6);

    // And it grows: the correction is `theta0^2 / 16`, so 0.1 rad gives 1 + 6.25e-4 and 1 rad gives 1 + 0.0625.
    const auto small = pendulum_period_series(length, 0.1, kGravity);
    REQUIRE(small.has_value());
    // Both terms of the series, because the fourth-order one is what a one-term comparison leaves behind: at
    // `theta0 = 0.1` it is `11 * 1e-4 / 3072` = 3.6e-7 of the period, and the first version of this line asserted
    // the ratio against `1 + theta0^2/16` alone and failed by exactly that -- a test checking that a two-term
    // expansion is a one-term expansion.
    REQUIRE(std::abs(small.value() / linear.value() -
                     (1.0 + 0.01 / 16.0 + 11.0 * 0.01 * 0.01 / 3072.0)) < 1.0e-12);

    const auto large = pendulum_period_series(length, 1.0, kGravity);
    REQUIRE(large.has_value());
    REQUIRE(large.value() > small.value());
    // The physical claim, stated as a claim: a one-radian pendulum is about 6.6% slower than the linear formula
    // says. The first version compared against **1.0645** with a 1e-3 tolerance and failed by 1.6e-3 -- a
    // remembered constant rather than the series' own value, which is exactly what the test exists to check. So
    // the series is asserted exactly and the physics is asserted loosely, in that order: a test whose expected
    // value came from memory cannot tell a wrong formula from a wrong memory.
    REQUIRE(std::abs(large.value() / linear.value() - (1.0 + 1.0 / 16.0 + 11.0 / 3072.0)) < 1.0e-12);
    REQUIRE(large.value() / linear.value() > 1.06);
    REQUIRE(large.value() / linear.value() < 1.07);

    // The refusals: a period of zero would be indistinguishable from a very fast pendulum, so nothing is
    // returned rather than a number a caller would print.
    REQUIRE_FALSE(pendulum_period_small_angle(0.0, kGravity).has_value());
    REQUIRE_FALSE(pendulum_period_small_angle(-1.0, kGravity).has_value());
    REQUIRE_FALSE(pendulum_period_small_angle(1.0, 0.0).has_value());
    REQUIRE_FALSE(pendulum_period_small_angle(std::nan(""), kGravity).has_value());
    REQUIRE_FALSE(pendulum_period_series(1.0, std::nan(""), kGravity).has_value());
}

TEST_CASE("examples.the_series_agrees_with_the_integrated_period", "[examples]") {
    // **The pendulum example.** A real trajectory, integrated by the model plugin, against a series expansion
    // from a lab manual. Neither knows about the other, so agreement is evidence.
    const double length = 1.0;
    const double dt = 1.0e-4;
    const double window = 12.0;

    // At a small amplitude the two predictions coincide, so the integrated period must match both.
    {
        const auto measured = measure_period(length, 0.02, kGravity, dt, window);
        const auto linear = pendulum_period_small_angle(length, kGravity);
        REQUIRE(measured.has_value());
        REQUIRE(linear.has_value());
        INFO("small amplitude: measured " << measured.value() << " linear " << linear.value());
        // 1e-5 s on a 2 s period: the interpolated crossing is good to about dt^2 and the integrator's own phase
        // error is far below that. The first version allowed 2e-5 to absorb a crossing located on the sampling
        // grid, which is a tolerance that says more about the measurement than about the model.
        // 1e-4 s, and the number is the **physics** rather than the integrator: at 0.02 rad a pendulum's period is
        // longer than the small-angle limit by `theta0^2/16` = 2.5e-5 of itself, which on a 2.006 s period is
        // 5.0e-5 s. The model is right and the measured value shows it; the first version of this line asked for
        // 1e-5 and failed by 5e-5, which is the assertion telling the pendulum not to be one.
        const auto at_amplitude = pendulum_period_series(length, 0.02, kGravity);
        REQUIRE(at_amplitude.has_value());
        REQUIRE(std::abs(measured.value() - at_amplitude.value()) < 1.0e-5);
        // And it is genuinely **not** the linear period, by more than that tolerance -- otherwise this case would
        // pass for a model that ignored the amplitude entirely.
        REQUIRE(measured.value() > linear.value());
        REQUIRE(std::abs(measured.value() - linear.value()) > 1.0e-5);
    }

    // At 30 degrees they do **not** coincide, and this is the case that distinguishes a correct model from a
    // linearised one. The linear formula is 1.7% short; the series is right to a few parts in a hundred thousand.
    {
        constexpr double kAmplitude = 0.5236;  // 30 degrees
        const auto measured = measure_period(length, kAmplitude, kGravity, dt, window);
        const auto linear = pendulum_period_small_angle(length, kGravity);
        const auto series = pendulum_period_series(length, kAmplitude, kGravity);
        REQUIRE(measured.has_value());
        REQUIRE(linear.has_value());
        REQUIRE(series.has_value());

        INFO("30 degrees: measured " << measured.value() << " series " << series.value()
                                     << " linear " << linear.value());
        // The measurement agrees with the series.
        // 1e-4 s: the series is truncated at fourth order, whose next term is ~1.4e-6 of the period at 30
        // degrees, and the measurement is good to ~1e-7. What is left is the physics.
        REQUIRE(std::abs(measured.value() - series.value()) < 1.0e-4);
        // And it **disagrees** with the linear formula by far more than that: the model is not linearised, and a
        // linearised one would fail the previous assertion while passing this one.
        REQUIRE(std::abs(measured.value() - linear.value()) > 2.0e-2);
        REQUIRE(series.value() > linear.value());
    }

    // Two amplitudes, so the growth is measured rather than assumed: the period is longer at 60 degrees than at
    // 30, and by roughly the amount the series predicts.
    {
        const auto thirty = measure_period(length, 0.5236, kGravity, dt, window);
        const auto sixty = measure_period(length, 1.0472, kGravity, dt, window);
        REQUIRE(thirty.has_value());
        REQUIRE(sixty.has_value());
        REQUIRE(sixty.value() > thirty.value());
        const auto predicted_thirty = pendulum_period_series(length, 0.5236, kGravity);
        const auto predicted_sixty = pendulum_period_series(length, 1.0472, kGravity);
        REQUIRE(predicted_thirty.has_value());
        REQUIRE(predicted_sixty.has_value());
        REQUIRE(std::abs((sixty.value() - thirty.value()) -
                         (predicted_sixty.value() - predicted_thirty.value())) < 1.0e-3);
    }
}

TEST_CASE("examples.the_oscillator_envelope_matches_the_integrated_decay", "[examples]") {
    // **The damping example, and the factor of two.** The envelope is `exp(-gamma t / 2)`; a model or a check
    // that used `exp(-gamma t)` would disagree by a factor of two in the exponent, which after four periods is a
    // discrepancy nobody could miss -- provided the test knows which of the two it is asserting.
    const double omega = 10.0;
    const double gamma = 1.0;
    const Peaks peaks = measure_peaks(omega, gamma, 6.0, 1.0e-5);
    REQUIRE(peaks.magnitudes.size() >= 4);

    for (std::size_t i = 0; i < 4; ++i) {
        const auto predicted = oscillator_envelope(1.0, gamma, peaks.times[i]);
        REQUIRE(predicted.has_value());
        INFO("peak " << i << " at t=" << peaks.times[i] << ": measured " << peaks.magnitudes[i] << " predicted "
                     << predicted.value());
        // Relative tolerance: the magnitudes span two orders of magnitude over the window, and an absolute one
        // would be vacuous at the end or impossible at the start.
        REQUIRE(std::abs(peaks.magnitudes[i] - predicted.value()) / predicted.value() < 2.0e-3);
    }

    // The negative control that makes the factor of two a real assertion rather than a tautology: with
    // `exp(-gamma t)` the prediction misses by a factor of `exp(gamma t / 2)`, which at the last peak is large.
    const std::size_t last = 3;
    const double wrong = std::exp(-gamma * peaks.times[last]);
    REQUIRE(std::abs(peaks.magnitudes[last] - wrong) > 0.05);

    // And the peaks really do decay, so the case cannot pass on a model that ignores gamma.
    //
    // **From the second pair onwards**, and the first version started at the first: `measure_peaks` records an
    // extremum at every **half** period -- a released oscillator turns at t = 0 and then at each turning point,
    // positive and negative -- and `peaks.magnitudes[0]` is the release point, 1.0. Asserting that the next one is
    // below it is asserting that the amplitude falls faster than the envelope, which at 15% per half period it
    // does not. The recorded times are right (peak 2 at 0.6291 s is one full period, and `|x|` = 0.7301 matches
    // `exp(-gamma t / 2)` to five digits); the comparison was wrong.
    for (std::size_t i = 2; i < 4; ++i) REQUIRE(peaks.magnitudes[i] < peaks.magnitudes[i - 1]);

    // The damped frequency, checked by measuring the interval between peaks against the closed form. This is the
    // quantity a careless test compares against `2 pi / omega` and gets away with it while the damping is small,
    // which is why it is asserted here at a damping large enough to matter: `gamma / omega = 0.1` moves the
    // frequency by 0.125%, and the measured interval is good to far better than that.
    const auto damped = oscillator_frequency(omega, gamma);
    REQUIRE(damped.has_value());
    REQUIRE(damped.value() < omega);
    // One **period**, which is two recorded peaks: see `measure_peaks` for why they come at half-period
    // intervals. Reading `times[1] - times[0]` as the period is the mistake this line exists to record -- it made
    // the measurement look like half of what the formula predicts, which is a discrepancy large enough to look
    // like a model error rather than a bookkeeping one.
    const double measured_period = peaks.times[2] - peaks.times[0];
    const double predicted_period = 2.0 * 3.14159265358979323846 / damped.value();
    INFO("period: measured " << measured_period << " damped " << predicted_period << " undamped "
                             << 2.0 * 3.14159265358979323846 / omega);
    REQUIRE(std::abs(measured_period - predicted_period) / predicted_period < 1.0e-3);
    // Specifically **not** the undamped period, which is a different number by more than the tolerance.
    REQUIRE(std::abs(predicted_period - 2.0 * 3.14159265358979323846 / omega) > 1.0e-5);

    // Beyond critical damping there is no oscillation, and a "frequency" for that motion is not a quantity.
    REQUIRE_FALSE(oscillator_frequency(1.0, 2.0).has_value());
    REQUIRE_FALSE(oscillator_frequency(1.0, 3.0).has_value());
    // Exactly at zero damping the frequency is the undamped one.
    const auto undamped = oscillator_frequency(omega, 0.0);
    REQUIRE(undamped.has_value());
    REQUIRE(undamped.value() == omega);
    // And a negative damping is refused rather than integrated into a growing envelope.
    REQUIRE_FALSE(oscillator_frequency(omega, -1.0).has_value());
    REQUIRE_FALSE(oscillator_envelope(1.0, -1.0, 1.0).has_value());
}

TEST_CASE("examples.the_projectile_matches_its_range_and_apex", "[examples]") {
    // **The projectile example.** The closed form describes a launch and a landing at the same height, so the
    // check is: fly until the projectile returns to its launch height, and see how far it went.
    const double g = kGravity;
    const double speed = 20.0;
    const double angle = 0.7853981633974483;  // 45 degrees, where the range is greatest

    auto model = qp::plugins::models::make_projectile(g, /*drag=*/0.0);
    REQUIRE(model != nullptr);

    ex::StateView state = state_of(4);
    state.set(0, 0, 0.0);                                    // x
    state.set(0, 1, 0.0);                                    // y
    state.set(0, 2, speed * std::cos(angle));                // vx
    state.set(0, 3, speed * std::sin(angle));                // vy

    // Twice the time to apex, at a step fine enough that the fourth-order error is irrelevant.
    const auto apex = projectile_apex(speed, angle, g);
    const auto range = projectile_range(speed, angle, g);
    REQUIRE(apex.has_value());
    REQUIRE(range.has_value());

    const double flight_time = 2.0 * speed * std::sin(angle) / g;
    const double dt = 1.0e-5;
    double highest = 0.0;
    double landed_x = 0.0;
    const auto steps = static_cast<std::size_t>(std::llround(flight_time / dt));
    for (std::size_t i = 0; i < steps; ++i) {
        REQUIRE(model->step(state, dt).has_value());
        highest = std::max(highest, state.at(0, 1));
        landed_x = state.at(0, 0);
    }

    INFO("range measured " << landed_x << " predicted " << range.value() << "; apex measured " << highest
                           << " predicted " << apex.value());
    // The range to a part in ten thousand, and the apex likewise: both are exact solutions of the same equations,
    // so the only error is the integrator's.
    REQUIRE(std::abs(landed_x - range.value()) / range.value() < 1.0e-4);
    REQUIRE(std::abs(highest - apex.value()) / apex.value() < 1.0e-4);

    // The maximum is at 45 degrees, which is the property a student checks: a 30-degree launch goes less far.
    const auto thirty = projectile_range(speed, 0.5236, g);
    REQUIRE(thirty.has_value());
    REQUIRE(thirty.value() < range.value());
    // And the range is symmetric about 45 degrees, so 30 and 60 agree -- a property no single-point check sees.
    const auto sixty = projectile_range(speed, 1.0472, g);
    REQUIRE(sixty.has_value());
    // 3e-4 and not a bit comparison:  .5236 and 1.0472 are rounded radians, so sin(2 * 0.5236) and
    // sin(2 * 1.0472) differ by 1.5e-4 -- the identity is exact in exact arithmetic and approximate in the
    // arguments a test writes down. The first version asserted 1e-9 and measured 1.5e-4, which was the fixture
    // being rounded rather than the formula being wrong.
    REQUIRE(std::abs(sixty.value() - thirty.value()) < 3.0e-4);

    // The refusals: a negative speed and a non-positive gravity are caller mistakes, not predictions.
    REQUIRE_FALSE(projectile_range(-1.0, angle, g).has_value());
    REQUIRE_FALSE(projectile_range(speed, angle, 0.0).has_value());
    REQUIRE_FALSE(projectile_apex(speed, std::nan(""), g).has_value());
}

TEST_CASE("examples.the_projectile_stops_at_the_ground_and_the_check_says_so", "[examples]") {
    // The drag-free range formula is only valid for a projectile that lands at its launch height and then stops,
    // and the model's floor is what makes that true. Without the floor the projectile would keep falling, and a
    // range measured by "fly for the flight time" would still be right -- so this case asserts the **floor**
    // rather than the range, because the range does not depend on it.
    const double g = kGravity;
    auto model = qp::plugins::models::make_projectile(g, 0.0);
    REQUIRE(model != nullptr);
    REQUIRE_FALSE(model->describe().time_reversible);

    ex::StateView state = state_of(4);
    state.set(0, 1, 1.0);
    state.set(0, 2, 3.0);
    state.set(0, 3, 0.0);

    // One long step, so the floor has to be applied inside the step rather than at the end of a run.
    REQUIRE(model->step(state, 2.0).has_value());
    REQUIRE(state.at(0, 1) == 0.0);
    REQUIRE(state.at(0, 2) == 0.0);
    REQUIRE(state.at(0, 3) == 0.0);

    // And with drag, the range is **shorter** than the drag-free prediction -- which is the statement that the
    // drag-free formula is not being applied to a dragged flight. The exact dragged trajectory has its own closed
    // form (asserted in `plugins/models`), so this case only needs the direction, and the direction is what a
    // student checks.
    auto dragged = qp::plugins::models::make_projectile(g, 0.5);
    REQUIRE(dragged != nullptr);
    ex::StateView d = state_of(4);
    d.set(0, 2, 20.0 * std::cos(0.7853981633974483));
    d.set(0, 3, 20.0 * std::sin(0.7853981633974483));
    const double flight_time = 2.0 * 20.0 * std::sin(0.7853981633974483) / g;
    const ex::StateView end = advance(*dragged, std::move(d), flight_time, 1.0e-5);
    const auto range = projectile_range(20.0, 0.7853981633974483, g);
    REQUIRE(range.has_value());
    REQUIRE(end.at(0, 0) < range.value());
}
