/**
 * @file expected.cpp
 * @brief The closed forms, with no numerical method anywhere in the file.
 *
 * That is worth stating as a property rather than a style: every function here is a formula, and a formula that
 * called an integrator would be a check that agrees with whatever it is checking. If a future prediction needs a
 * numerical result -- the period of a pendulum at 170 degrees, say -- it belongs in a differently named function
 * that says where its number came from, not in one of these.
 */
#include <qp/plugins/examples/expected.hpp>

#include <cmath>

namespace qp::plugins::examples {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// @brief Whether `value` is a number a formula can use.
[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

}  // namespace

std::optional<double> pendulum_period_small_angle(double length, double gravity) noexcept {
    if (!finite(length) || !finite(gravity)) return std::nullopt;
    if (!(length > 0.0) || !(gravity > 0.0)) return std::nullopt;
    return 2.0 * kPi * std::sqrt(length / gravity);
}

std::optional<double> pendulum_period_series(double length, double theta0, double gravity) noexcept {
    const std::optional<double> base = pendulum_period_small_angle(length, gravity);
    if (!base.has_value()) return std::nullopt;
    if (!finite(theta0)) return std::nullopt;

    // The series in `theta0`, not in `sin(theta0/2)`. The two are the same expansion written differently: the
    // manual's `sin^2(theta0/2)` form collects the terms so that the first correction never overshoots, and its
    // leading coefficient is `theta0^2/16` once expanded. Writing it the manual's way would need the
    // `sin^2(theta0/2)` form to be part of the contract, and a caller comparing against a hand calculation uses
    // the polynomial.
    const double t2 = theta0 * theta0;
    return base.value() * (1.0 + t2 / 16.0 + 11.0 * t2 * t2 / 3072.0);
}

std::optional<double> oscillator_envelope(double amplitude, double gamma, double t) noexcept {
    if (!finite(amplitude) || !finite(gamma) || !finite(t)) return std::nullopt;
    if (gamma < 0.0) return std::nullopt;
    return std::abs(amplitude) * std::exp(-gamma * t / 2.0);
}

std::optional<double> oscillator_frequency(double omega, double gamma) noexcept {
    if (!finite(omega) || !finite(gamma)) return std::nullopt;
    if (!(omega > 0.0) || gamma < 0.0) return std::nullopt;

    const double squared = omega * omega - gamma * gamma / 4.0;
    // At or beyond critical damping there is no oscillation, and returning zero would be a frequency a caller
    // could divide by. Nothing says "this system does not oscillate" without pretending to be a number.
    if (!(squared > 0.0)) return std::nullopt;
    return std::sqrt(squared);
}

std::optional<double> projectile_range(double speed, double angle, double gravity) noexcept {
    if (!finite(speed) || !finite(angle) || !finite(gravity)) return std::nullopt;
    if (speed < 0.0 || !(gravity > 0.0)) return std::nullopt;
    return speed * speed * std::sin(2.0 * angle) / gravity;
}

std::optional<double> projectile_apex(double speed, double angle, double gravity) noexcept {
    if (!finite(speed) || !finite(angle) || !finite(gravity)) return std::nullopt;
    if (speed < 0.0 || !(gravity > 0.0)) return std::nullopt;
    const double vertical = speed * std::sin(angle);
    return vertical * vertical / (2.0 * gravity);
}

}  // namespace qp::plugins::examples
