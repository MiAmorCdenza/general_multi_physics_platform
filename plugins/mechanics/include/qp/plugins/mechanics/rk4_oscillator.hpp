/**
 * @file rk4_oscillator.hpp
 * @brief A fourth-order Runge-Kutta stepper for the harmonic oscillator.
 *
 * ## Why this is a plugin and not part of the foundation
 *
 * `core/graph/kernels` owns the *contract* -- what an operator is, what it may do
 * per step, how it is registered -- and ships no integrator at all. That split is
 * deliberate: RK4, leapfrog, Verlet and Boris are all defensible answers to "how do
 * I advance this", and which one is right depends on the physics, the accuracy the
 * course demands, and what the student is supposed to learn from watching it fail.
 * Building one into the core would make the others second-class, and the first
 * teacher who needs symplectic behaviour for an orbit would be editing the
 * foundation.
 *
 * ## Why the state is two components per particle
 *
 * Position in component 0, velocity in component 1. A harmonic oscillator is
 * second order, so its state is a pair, and the pair is what makes RK4 applicable:
 * the method advances a first-order system `y' = f(t, y)` and gets second-order
 * equations only by carrying the derivative as part of the state.
 *
 * ## What this operator is honest about
 *
 * **RK4 is not symplectic, and an oscillator is exactly where that shows.** The
 * method loses energy at a slow, steady rate, so a student who runs a pendulum for
 * ten thousand periods sees the amplitude decay. That is a property of the method,
 * not a bug, and it is the reason `Leapfrog` and `Verlet` exist beside this one --
 * a comparison between the two is one of the more useful afternoons a mechanics lab
 * can have. The alternative, quietly swapping in a symplectic integrator so the
 * graph "looks right", would remove the lesson and make the platform lie about what
 * it computed.
 *
 * @ownership   observes
 * @thread      main (prepare), eval (advance)
 * @pre         none
 * @post        none
 * @invariant   `advance` allocates nothing, throws nothing, and blocks on nothing
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none -- no RNG is consumed, so a run is bit-reproducible
 * @frozen      no
 * @tests       plugin.mechanics.oscillator_stays_on_its_orbit,
 *              plugin.mechanics.oscillator_converges_at_fourth_order,
 *              plugin.mechanics.oscillator_loses_energy_slowly,
 *              plugin.mechanics.oscillator_is_deterministic,
 *              plugin.mechanics.oscillator_advances_every_particle,
 *              plugin.mechanics.oscillator_rejects_bad_input,
 *              plugin.mechanics.oscillator_rejects_bad_batch
 */
#pragma once

#include <qp/graph/kernels/kernel.hpp>

#include <qp/diag.hpp>

#include <cstddef>
#include <string_view>

namespace qp::plugins::mechanics {

/**
 * @brief RK4 for `x'' = -omega^2 * x`, one step per call.
 *
 * ### Parameter block layout
 *
 * | Slot | Meaning |
 * |---|---|
 * | `reals[0]` | `omega`, the angular frequency in rad/s. Must be finite and > 0 |
 *
 * Slot 0 is required. A zero or negative `omega` is refused in `prepare` rather
 * than clamped: `omega = 0` is a free particle rather than a slow oscillator, and
 * silently substituting a value the caller did not ask for would produce a graph
 * that runs and is not the graph that was drawn.
 *
 * ### Why the frequency is a parameter and not derived from `k` and `m`
 *
 * Because the caller already did that. A spring constant and a mass are two
 * separate parameters of the *model*; `omega` is the one number the integrator
 * needs. Passing `k` and `m` down would put a square root on the step path for
 * every particle, every step, to recompute a constant that never changes during a
 * run -- and `prepare` is precisely the place the platform provides for computing
 * it once.
 *
 * @ownership   owns nothing; the host owns the registry entry and this object
 * @thread      main (prepare), eval (advance)
 * @pre         none
 * @post        none
 * @invariant   After a successful `prepare`, `omega_` is finite and positive
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       plugin.mechanics.oscillator_stays_on_its_orbit,
 *              plugin.mechanics.oscillator_converges_at_fourth_order,
 *              plugin.mechanics.oscillator_loses_energy_slowly,
 *              plugin.mechanics.oscillator_is_deterministic,
 *              plugin.mechanics.oscillator_advances_every_particle,
 *              plugin.mechanics.oscillator_rejects_bad_input,
 *              plugin.mechanics.oscillator_rejects_bad_batch,
 *              plugin.mechanics.oscillator_registers_under_its_name
 */
class Rk4Oscillator final : public graph::kernels::IBatchAdvancer {
public:
    /// @brief A stepper that has not been prepared and cannot advance yet.
    Rk4Oscillator() noexcept = default;

    /**
     * @brief The name this operator is registered and logged under.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns a string literal with static storage
     * @invariant   Constant for the lifetime of the object
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.oscillator_registers_under_its_name
     */
    [[nodiscard]] std::string_view name() const noexcept override;

    /**
     * @brief What this operator needs from the host.
     *
     * `none`: the state is `[x, v]` per particle, four intermediate stages fit in
     * registers, and no RNG is consumed. Declaring `is_stochastic` here would be the
     * expensive kind of lie -- the host would thread a seeded RNG through every step
     * to feed an operator that ignores it, and a run's reproducibility ledger would
     * claim a dependency that does not exist.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns Capability::none
     * @invariant   Constant for the lifetime of the object
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.oscillator_registers_under_its_name
     */
    [[nodiscard]] graph::kernels::Capability capabilities() const noexcept override;

    /**
     * @brief Reads `omega` out of the parameter block, or refuses the block.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success, `advance` is usable and `is_prepared()` is true
     * @invariant   Calling it twice with equal blocks has the same effect as once
     * @errors      Returns `invalid_argument` when `omega` is not finite or not
     *              positive; the previous preparation is left untouched by a refusal
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.oscillator_rejects_bad_input
     */
    [[nodiscard]] diag::Result<void> prepare(const graph::kernels::ParamBlock& params) override;

    /**
     * @brief Advances every particle by `ctx.dt` with one RK4 step.
     *
     * @ownership   observes `batch`; writes through `batch.out`
     * @thread      eval
     * @pre         `batch.valid()` and `is_prepared()`
     * @post        `out[i]` holds the state of particle `i` after one step
     * @invariant   Allocates nothing, throws nothing, and reads only the state it
     *              was given: no `count`-dependent allocation and no host callback
     * @errors      Returns `invalid_argument` for an unprepared operator or an
     *              unreadable batch instead of advancing
     * @complexity  O(count)
     * @nondet      none -- `ctx.rng` is never read, which is what its capability
     *              declaration promises
     * @frozen      no
     * @tests       plugin.mechanics.oscillator_stays_on_its_orbit,
     *              plugin.mechanics.oscillator_rejects_bad_input
     */
    [[nodiscard]] diag::Result<void> advance(const graph::kernels::BatchView& batch,
                                             graph::kernels::AdvanceContext& ctx) override;

    /// @brief Whether `prepare` has succeeded at least once.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        none
    /// @invariant   True for every object whose last `prepare` returned ok
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.mechanics.oscillator_rejects_bad_input
    [[nodiscard]] bool is_prepared() const noexcept { return prepared_; }

    /// @brief The prepared angular frequency, or 0 when unprepared.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        none
    /// @invariant   Positive exactly when `is_prepared()`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.mechanics.oscillator_rejects_bad_input
    [[nodiscard]] double omega() const noexcept { return omega_; }

private:
    double omega_ = 0.0;
    bool prepared_ = false;
};

}  // namespace qp::plugins::mechanics
