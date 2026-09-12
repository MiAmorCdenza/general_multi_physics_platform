/**
 * @file models.hpp
 * @brief The concrete physics this build ships: three models, and one RK4 that integrates all of them.
 *
 * ## What is here, and what is deliberately not
 *
 * | model | equation | state per particle |
 * |---|---|---|
 * | `model.damped_oscillator` | `x'' = -w^2 x - g x'` | position, velocity, `w` |
 * | `model.pendulum` | `th'' = -(g/L) sin(th)` | `th`, `th'`, `L` |
 * | `model.projectile` | `x'' = -k x'`, `y'' = -g - k y'` | `x`, `y`, `vx`, `vy` |
 *
 * Each is a **model** in the sense the plan tree means: a differential equation with parameters a user sets, not
 * an integrator. The integrator is shared, and that is the point of this file's shape rather than four copies of
 * the same arithmetic with different right-hand sides.
 *
 * ## Why one RK4 and not a class per model
 *
 * `plugins/mechanics`' `Rk4Oscillator` is an `IBatchAdvancer`, which is the right interface for a kernel that
 * steps a lattice. These three are `IStateOperator`s, which is the right interface for a run loop, and both exist
 * because they serve different callers. What they do **not** need to differ in is the Runge-Kutta arithmetic
 * itself: a fourth-order step is one algorithm applied to `y' = f(y)`, and writing it three times would be three
 * places for a coefficient to be mistyped and three places to fix when a fifth-order method is wanted.
 *
 * So `Rk4Model` takes the derivative as a function and owns the stepping, and each model below is a derivative
 * and a validation. The cost is one indirect call per stage -- four per step -- and the benefit is that the
 * arithmetic has one implementation, tested once, at fourth order, by the same convergence case for all three
 * models.
 *
 * ## Why the state layouts differ, and why that is not an inconsistency
 *
 * The oscillator uses three components per particle (position, velocity, frequency) and the pendulum uses two
 * (angle, angular velocity), while `StateView::components_per_particle` defaults to three. A layout is the
 * **model's** business: the pendulum has no frequency parameter, because its frequency depends on its amplitude
 * and is not a constant of the model -- that is the entire difference between a pendulum and a harmonic
 * oscillator, and padding the state with a number nothing reads would be inventing a parameter so the shapes
 * would match.
 *
 * Each binder therefore states the layout it needs and declines any other, which is what makes several models
 * coexist in one graph.
 *
 * ## Honesty about reversibility, per model
 *
 * `SimModelDesc::time_reversible` is a claim the platform can check, so each model answers for itself and the
 * answers differ:
 *
 *   - the **oscillator** is time-reversible -- both terms are linear in the state -- and says so;
 *   - the **pendulum** is too: `sin` has no preferred direction in time, and `theta'' = -(g/L) sin(theta)` run
 *     backwards retraces the swing;
 *   - the **projectile is not**, and not because of the drag term -- `-k v` is linear as well. It is because the
 *     model is *stated* to stop at the ground, and "stop" is not a reversible operation: running the flight
 *     backwards from a resting projectile does not produce a launch. A model whose definition contains an
 *     irreversible step is not time-reversible, however linear its equations are, and saying so is the point of
 *     having the field.
 *
 * That last one is worth reading twice, because it is the sort of thing a plugin author will get wrong: the
 * declaration is about **the model as stated**, not about the differential equation in isolation.
 *
 * @ownership   owns
 * @thread      main (a step runs on the caller's thread; no model keeps mutable state)
 * @pre         none
 * @post        none
 * @invariant   A step allocates nothing, throws nothing, and reads nothing but the state and `dt`
 * @errors      See each declaration
 * @frozen      no
 * @tests       models.a_step_converges_at_fourth_order
 */
#pragma once

#include <qp/graph/execution/execution.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <string_view>

namespace qp::plugins::models {

/// @brief Components per particle for each model's state.
///
/// Named rather than written as literals at each use, because the numbers are a contract between a model and its
/// binder and a literal in two places is two chances to disagree.
inline constexpr std::size_t kOscillatorComponents = 3;  // x, x', w
inline constexpr std::size_t kPendulumComponents = 2;    // th, th'
inline constexpr std::size_t kProjectileComponents = 4;  // x, y, vx, vy
/// Position, velocity and **time** -- the driven oscillator carries its own clock; see its declaration.
inline constexpr std::size_t kDrivenComponents = 3;

/**
 * @brief The largest state dimension `Rk4Model` accepts, and why there is a limit at all.
 *
 * The step works in fixed-size arrays rather than in heap vectors, because `step`'s contract says it allocates
 * nothing and a `std::vector` per stage -- four of them, once per step -- is exactly the allocation that contract
 * forbids. That makes the dimension a compile-time constant, and this is it: the widest model in this directory is
 * the four-component projectile, so four is the bound and a model needing more would grow this number rather than
 * quietly allocate.
 */
inline constexpr std::size_t kMaximumDimension = 4;

/**
 * @brief A right-hand side: `dydt = f(t, y)`, with `t` the state *before* the step.
 *
 * The time argument is carried because a projectile with a time-dependent wind, or a driven oscillator, needs it;
 * all three models here ignore it, and the signature keeps a future model from having to change the integrator.
 */
using Derivative = std::function<void(double t, const double* y, double* dydt)>;

/**
 * @brief What a model does to a particle **after** the step, for constraints a differential equation cannot state.
 *
 * A floor is the case this exists for. "The projectile stops when it reaches the ground" is not a term in
 * `y' = f(y)` -- no smooth right-hand side can express it -- and approximating it with a very stiff spring would
 * make the trajectory depend on a spring constant nobody chose. So a model may install a post-step constraint,
 * and it is applied to every particle immediately after the arithmetic.
 *
 * The hook is given the state and must leave it consistent; it may not allocate, throw, or depend on anything but
 * the state. It runs **inside** the step rather than after a run, which is the difference between a projectile
 * that lands and one that is pulled out of the floor at the end.
 */
using PostStep = std::function<void(qp::graph::execution::StateView& state)>;

/**
 * @brief Classical RK4 for a first-order system, and the base every model in this directory derives from.
 *
 * @ownership   owns
 * @thread      main
 * @pre         `dimension > 0`
 * @post        none
 * @invariant   `describe()` is stable; `step` reads only the state and `dt`
 * @errors      `step` returns `invalid_argument` for a zero or non-finite `dt`
 * @frozen      no
 * @tests       models.a_step_converges_at_fourth_order,
 *              models.a_zero_step_is_refused
 */
class Rk4Model : public qp::graph::execution::IStateOperator {
public:
    /**
     * @brief Builds a stepper for a `dimension`-dimensional first-order system.
     *
     * @param name       Stable name for the trace's provenance.
     * @param dimension  Doubles per particle in this model's state, at most `kMaximumDimension`.
     * @param derivative The right-hand side. Captured by value.
     * @param desc       What the model claims about itself. Taken by value because the claims differ per model
     *                   and a base class cannot derive them.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `0 < dimension <= kMaximumDimension` and `derivative` is set
     * @post        none
     * @invariant   The derivative is called exactly four times per particle per step
     * @errors      noexcept; a dimension outside the range is clamped rather than refused, because the caller is
     *              this directory's own factory and a silently-wrong model would be worse than a narrow one
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       models.a_step_converges_at_fourth_order
     */
    Rk4Model(std::string_view name, std::size_t dimension, Derivative derivative,
             qp::graph::execution::SimModelDesc desc) noexcept;

    /**
     * @brief Installs a post-step constraint, or clears it.
     *
     * Separate from the constructor because only one of the three models needs one, and a parameter that three of
     * four callers pass as `{}` is a parameter that should not be in the signature.
     *
     * @param post The constraint, applied to the whole state after each step. Empty means none.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `post`, if set, leaves the state consistent and does not allocate
     * @post        The next step applies `post` once, after the arithmetic, before returning
     * @invariant   Without a constraint the step is exactly classical RK4
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       models.the_projectile_stops_at_the_ground,
 *              examples.the_projectile_stops_at_the_ground_and_the_check_says_so
     */
    void set_post_step(PostStep post) noexcept { post_ = std::move(post); }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] qp::graph::execution::SimModelDesc describe() const noexcept override {
        return desc_;
    }

    [[nodiscard]] diag::Result<void> step(qp::graph::execution::StateView& state, double dt) override;

    /// @brief Doubles per particle this model expects. Its binder declines any other layout.
    [[nodiscard]] std::size_t dimension() const noexcept { return dimension_; }

private:
    std::string name_;
    std::size_t dimension_ = 0;
    Derivative derivative_;
    PostStep post_{};
    qp::graph::execution::SimModelDesc desc_{};
};

/**
 * @brief `x'' = -w^2 x - gamma x'`: a harmonic oscillator with damping.
 *
 * Fills a real gap rather than duplicating `plugins/mechanics`: its operator integrates **undamped**
 * `x'' = -w^2 x` and its binder refuses a node with a non-zero `c`, because the kernel has no damping term. A
 * damped oscillator is the first thing a mechanics lab measures -- the decay rate is the measurement -- so the
 * refusal is correct and the missing model is real.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Time-reversible: both terms are linear in the state
 * @errors      See `Rk4Model::step`
 * @frozen      no
 * @tests       models.the_damped_oscillator_decays_at_the_rate_it_declares
 */
[[nodiscard]] std::unique_ptr<qp::graph::execution::IStateOperator> make_damped_oscillator(
    double omega, double gamma);

/**
 * @brief `th'' = -(g/L) sin(th)`: the pendulum, without the small-angle approximation.
 *
 * **Not** `th'' = -(g/L) th`, and the difference is the lesson. The linearised equation has a period that does not
 * depend on amplitude; the real one does, growing by about 0.5% at 10 degrees and 18% at 90. A platform that
 * linearised would agree with a first-year textbook and disagree with the pendulum on the bench, and the
 * disagreement is exactly what an experiment is supposed to find.
 *
 * @param gravity   `g` in m/s^2. Must be finite and positive.
 * @param length    `L` in metres. Must be finite and positive.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Time-reversible: `sin` has no preferred direction in time
 * @errors      See `Rk4Model::step`
 * @frozen      no
 * @tests       models.the_pendulum_period_grows_with_amplitude
 */
[[nodiscard]] std::unique_ptr<qp::graph::execution::IStateOperator> make_pendulum(double gravity,
                                                                                 double length);

/**
 * @brief A **driven** oscillator: `x'' = -w0^2 x - gamma x' + F cos(wd t)`.
 *
 * The resonance experiment, and the one model here whose equation depends explicitly on time. A student sweeps
 * the driving frequency and plots the response amplitude, which peaks near `w0` and whose width is set by the
 * damping -- the measurement that explains why a swing is pushed at its natural rate and why a bridge can be
 * destroyed by marching soldiers.
 *
 * ## Why the time is a state component
 *
 * `x'' = -w0^2 x - gamma x' + F cos(wd t)` is **not autonomous**: its right-hand side depends on `t` as well as
 * on the state. The obvious implementation is to read a clock, and it is wrong for two reasons that both matter:
 * a model that read the wall clock would not be reproducible from its seed, and a model that read the run loop's
 * own step counter would be reaching for state it does not own -- which is exactly what `IStateOperator`'s
 * contract forbids, because a step must depend on nothing but the state and `dt`.
 *
 * So the phase is carried **in the state** as its third component, with `t' = 1`. That makes the equation
 * autonomous again: the derivative reads `t` from the state it was handed, and a step depends on nothing else.
 * It is the standard trick for turning a non-autonomous system into an autonomous one, and it is worth the
 * component because it keeps the model honest under time reversal as well -- stepping backwards with `dt < 0`
 * runs `t` backwards, so the forcing runs backwards too.
 *
 * The component count is therefore **three**, not two. That is a different shape from the undamped and damped
 * oscillators, and it is why `ModelsBinder` asks each of them for its own layout rather than assuming one.
 *
 * ## What it declares
 *
 * Pure, and **not** time-reversible, and the second is worth reading. The equation is as linear as the damped
 * oscillator's, and `cos` has no preferred direction in time -- so the differential equation is reversible. What
 * is not reversible is the **phase**: `t` is a monotonically increasing quantity with an origin, and stepping
 * backwards past `t = 0` would run the drive through its own start. A model whose state carries an absolute time
 * cannot be run backwards through that origin, so it says so rather than letting a round-trip test discover it.
 *
 * @param omega0  The natural angular frequency in rad/s. Must be finite and positive.
 * @param gamma   The damping coefficient in 1/s. Must be finite and non-negative.
 * @param force   The driving amplitude per unit mass, in m/s^2. Must be finite; zero is the free case.
 * @param omega_d The driving angular frequency in rad/s. Must be finite and non-negative; zero gives a constant
 *                push rather than an oscillation, which is a legitimate way to measure the static response.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The third state component advances as `t' = 1`, whatever the other two do
 * @errors      See `Rk4Model::step`
 * @frozen      no
 * @tests       models.the_driven_oscillator_finds_its_resonance
 */
[[nodiscard]] std::unique_ptr<qp::graph::execution::IStateOperator> make_driven_oscillator(
    double omega0, double gamma, double force, double omega_d);

/**
 * @brief A projectile under gravity with **linear** drag, stopped at the ground.
 *
 * `x'' = -k x'` and `y'' = -g - k y'`, with `k` the drag coefficient per unit mass in 1/s. Linear rather than
 * quadratic drag, and that is a stated limitation rather than an oversight: quadratic drag has no closed-form
 * solution, so a test could only check it against itself, while the linear case has an exact solution that makes
 * the numerical error measurable. A course that wants `v^2` drag gets a different model under a different name,
 * and the reopening condition is written where the model is defined.
 *
 * **The ground is part of the model, not a post-processing step.** Below `y = 0` the velocity is set to zero and
 * the position is clamped, because a projectile that keeps falling through the floor produces a trajectory that
 * looks plausible and is not what was asked for. That is also why this model declares itself
 * **not** time-reversible -- see the file comment.
 *
 * @param gravity `g` in m/s^2. Must be finite and positive.
 * @param drag    `k` in 1/s. Must be finite and non-negative; zero is the drag-free case.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A particle at rest on the ground stays there
 * @errors      See `Rk4Model::step`
 * @frozen      no
 * @tests       models.a_projectile_matches_its_closed_form
 */
[[nodiscard]] std::unique_ptr<qp::graph::execution::IStateOperator> make_projectile(double gravity,
                                                                                   double drag);

}  // namespace qp::plugins::models
