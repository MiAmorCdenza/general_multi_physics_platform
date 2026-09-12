/**
 * @file expected.hpp
 * @brief What an experiment should produce when the physics is right -- written down **before** the run.
 *
 * ## Why a prediction is content, and why it is separate from the model
 *
 * "Compare the measurement against the theory" is the step a lab report is graded on, and it is the step a
 * platform can most easily fake: if the model computed both the trajectory and the number it is checked
 * against, the check would compare an implementation with itself and pass for any physics at all. So the
 * predictions here are written as **closed forms and published series**, independently of the integrators, and
 * they are what the models' outputs are measured against.
 *
 * That also makes them the honest place to say how good the agreement should be. A small-angle pendulum's period
 * is `2 pi sqrt(L/g)`; the real one is longer by `theta0^2/16 + 11 theta0^4/3072 + ...`, and a course that
 * compares a 30-degree swing against the linear formula is not measuring the integrator's error -- it is
 * measuring the approximation it chose. Both numbers are here, under names that say which is which.
 *
 * ## Where each formula comes from
 *
 * Every one is a standard result with a plain derivation, and the derivation is named rather than reproduced:
 *
 *   - `pendulum_period_small_angle`: the simple harmonic period, exact only in the limit `theta0 -> 0`;
 *   - `pendulum_period_series`: the large-amplitude expansion in `sin(theta0/2)`, the form a lab manual gives
 *     because it converges fast enough that two terms are all a course needs;
 *   - `oscillator_envelope`: for `x'' = -w^2 x - gamma x'` under light damping, the amplitude falls as
 *     `exp(-gamma t / 2)` -- the factor of two is the classic error, and it comes from the characteristic
 *     equation's roots, whose imaginary part is what oscillates and whose real part is half the damping;
 *   - `oscillator_frequency`: the damped frequency `sqrt(w^2 - gamma^2/4)`, strictly below `w`. A test that
 *     checked a damped oscillator's period against `2 pi / w` would be wrong by this much, which is why it is a
 *     separate function rather than a comment;
 *   - `projectile_range`: the drag-free range `v^2 sin(2 theta) / g`, and
 *   - `projectile_apex`: `v^2 sin^2(theta) / (2 g)`.
 *
 * ## The one thing none of them does
 *
 * None of them integrates anything, and none of them consults a model. That is the whole point: a prediction
 * that came from the same code as the trajectory could not tell a correct trajectory from a wrong one.
 *
 * @ownership   pure (every function is a closed form)
 * @thread      any
 * @pre         Parameters are physically meaningful; see each declaration
 * @post        none
 * @invariant   Depends on nothing but its arguments
 * @errors      See each declaration
 * @frozen      no
 * @tests       examples.the_small_angle_period_is_the_limit_of_the_series
 */
#pragma once

#include <qp/diag/result.hpp>

#include <cstddef>
#include <optional>

namespace qp::plugins::examples {

/// @brief The standard gravity this catalogue's examples use, in m/s^2.
///
/// One named constant rather than a literal in each example, because an example whose `g` disagrees with the
/// prediction it is compared against is an example that fails for a reason nobody will find quickly.
inline constexpr double kGravity = 9.80665;

/**
 * @brief The period of a simple pendulum in the small-angle limit: `2 pi sqrt(L / g)`.
 *
 * Exact only as `theta0 -> 0`. The name says so, and the series below is what to compare a real swing against.
 *
 * @param length  The pendulum's length, in metres. Must be finite and positive.
 * @param gravity `g`, in m/s^2. Must be finite and positive.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A positive, finite period for valid arguments, otherwise nothing
 * @invariant   Independent of amplitude, which is exactly what the next function corrects
 * @errors      Reports through the optional rather than throwing: a non-positive length is a caller mistake,
 *              and a period of zero would be indistinguishable from a very fast pendulum
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       examples.the_small_angle_period_is_the_limit_of_the_series
 */
[[nodiscard]] std::optional<double> pendulum_period_small_angle(double length,
                                                                double gravity = kGravity) noexcept;

/**
 * @brief The period of a simple pendulum to fourth order in the amplitude.
 *
 * `T = T0 * (1 + (1/16) sin^2(theta0/2) * 4 + ...)`, written in the form a lab manual uses:
 *
 *     T = T0 * (1 + theta0^2/16 + 11 theta0^4/3072)
 *
 * The first correction is the one every course meets -- 0.16% at 5 degrees, 0.65% at 10, 1.5% at 15 -- and it is
 * the reason a pendulum is a bad way to measure `g` unless the amplitude is either small or **measured**.
 *
 * The series is asymptotic in the sense that it is truncated, not that it diverges: at `theta0 = pi/2` the two
 * terms are within 0.1% of the exact elliptic-integral period, which is far beyond any swing a lab uses. Above
 * about 100 degrees the truncation starts to matter and a caller should say so rather than trusting this.
 *
 * @param length   The pendulum's length, in metres. Must be finite and positive.
 * @param theta0   The amplitude, in radians. Must be finite; the sign does not matter.
 * @param gravity  `g`, in m/s^2. Must be finite and positive.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A period at least as long as the small-angle one, for valid arguments
 * @invariant   Tends to the small-angle period as `theta0 -> 0`
 * @errors      Nothing for a non-positive length or gravity, or a non-finite amplitude
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       examples.the_small_angle_period_is_the_limit_of_the_series,
 *              examples.the_series_agrees_with_the_integrated_period
 */
[[nodiscard]] std::optional<double> pendulum_period_series(double length, double theta0,
                                                           double gravity = kGravity) noexcept;

/**
 * @brief The decaying amplitude of a lightly damped oscillator at time `t`: `A0 exp(-gamma t / 2)`.
 *
 * The factor of two is the point of the function existing. For `x'' = -w^2 x - gamma x'` the roots of the
 * characteristic equation are `-gamma/2 +/- i sqrt(w^2 - gamma^2/4)`, so the **imaginary** part -- what
 * oscillates -- is the damped frequency below, and the **real** part -- what decays -- is half the damping
 * coefficient. Writing `exp(-gamma t)` is the mistake the whole example is built to catch.
 *
 * @param amplitude The initial amplitude. Any finite value.
 * @param gamma     The damping coefficient in 1/s. Must be finite and non-negative.
 * @param t         The time in seconds. Must be finite.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A non-negative envelope for valid arguments
 * @invariant   Equals `amplitude` at `t = 0`, and `gamma = 0` makes it constant
 * @errors      Nothing for a non-finite argument or a negative damping
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       examples.the_oscillator_envelope_matches_the_integrated_decay
 */
[[nodiscard]] std::optional<double> oscillator_envelope(double amplitude, double gamma,
                                                        double t) noexcept;

/**
 * @brief The **damped** angular frequency `sqrt(w^2 - gamma^2/4)`.
 *
 * Strictly below `w`, and by an amount that grows as the damping does. A test that measured a damped
 * oscillator's period and compared it against `2 pi / w` would be checking the wrong number, and the error is
 * small enough to be mistaken for an integrator's error until the damping is large.
 *
 * Nothing when the damping is at or beyond critical (`gamma >= 2 w`), because then there is no oscillation: the
 * system returns to equilibrium without crossing it, and a "frequency" for that motion is not a quantity.
 *
 * @param omega The undamped angular frequency in rad/s. Must be finite and positive.
 * @param gamma The damping coefficient in 1/s. Must be finite and non-negative.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A positive frequency below `omega`, or nothing when the damping is critical or beyond
 * @invariant   Equals `omega` exactly when `gamma == 0`
 * @errors      noexcept; nothing for a non-positive `omega` or a negative damping
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       examples.the_oscillator_envelope_matches_the_integrated_decay
 */
[[nodiscard]] std::optional<double> oscillator_frequency(double omega, double gamma) noexcept;

/**
 * @brief The horizontal range of a projectile launched and landed at the same height, with no drag.
 *
 * `R = v^2 sin(2 theta) / g`. The drag-free case only, and the name says so: with drag there is no closed form,
 * so a range prediction for a dragged projectile would have to come from an integration and could not serve as
 * an independent check. `examples`' projectile example therefore verifies the **drag-free** run against this and
 * the dragged run against its velocity's closed form, which does exist.
 *
 * @param speed   The launch speed in m/s. Must be finite and non-negative.
 * @param angle   The launch angle in radians. Any finite value.
 * @param gravity `g`, in m/s^2. Must be finite and positive.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A non-negative range for valid arguments
 * @invariant   Symmetric about `theta = pi/4`, and zero at `theta = 0` and `theta = pi/2`
 * @errors      Nothing for a negative speed or a non-positive gravity
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       examples.the_projectile_matches_its_range_and_apex
 */
[[nodiscard]] std::optional<double> projectile_range(double speed, double angle,
                                                     double gravity = kGravity) noexcept;

/**
 * @brief The greatest height a drag-free projectile reaches: `v^2 sin^2(theta) / (2 g)`.
 *
 * @param speed   The launch speed in m/s. Must be finite and non-negative.
 * @param angle   The launch angle in radians. Any finite value.
 * @param gravity `g`, in m/s^2. Must be finite and positive.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        A non-negative height for valid arguments
 * @invariant   Zero at `theta = 0`, and equal to `v^2 / (2 g)` at `theta = pi/2`
 * @errors      Nothing for a negative speed or a non-positive gravity
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       examples.the_projectile_matches_its_range_and_apex
 */
[[nodiscard]] std::optional<double> projectile_apex(double speed, double angle,
                                                    double gravity = kGravity) noexcept;

}  // namespace qp::plugins::examples
