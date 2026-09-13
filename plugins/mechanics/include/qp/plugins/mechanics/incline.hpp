/**
 * @file incline.hpp
 * @brief A block on an inclined plane, with static and kinetic friction.
 *
 * ## Why semi-implicit Euler rather than RK4
 *
 * Coulomb friction is **discontinuous in velocity**: the force is not a function of
 * position and velocity but a set-valued relation, because at zero velocity the
 * friction force can be anything up to `mu_s * N`. RK4 assumes a smooth right-hand
 * side and evaluates it at intermediate points, so it will happily sample the friction
 * force at a velocity that is on the wrong side of the discontinuity -- which produces a
 * block that jitters around the static threshold instead of resting on it. That jitter
 * is not a small numerical artefact; it is the phenomenon the exercise is about, and a
 * student who sets `mu_s` just above `tan(theta)` is entitled to a block that stays put
 * rather than one that creeps downhill at the fourth root of the step size.
 *
 * Semi-implicit Euler is the standard choice for this because it evaluates friction once
 * per step, as a decision, and the decision is expressible: if the along-slope speed is
 * zero and gravity cannot overcome the static threshold, the block does not move.
 *
 * ## Why the transition is a comparison, not a smoothing
 *
 * A common trick is to ramp friction continuously from `mu_s` to `mu_k` over a small
 * velocity band, which removes the discontinuity and lets RK4 run. It also removes the
 * lesson: the point of `mu_s > mu_k` is that the force required to *start* motion exceeds
 * the force that *opposes* it, and a smoothed law has no start. The block either holds or
 * it slides, and this operator says which.
 *
 * ## The state layout
 *
 * | Component | Meaning |
 * |---|---|
 * | 0 | position along the slope, measured from the starting point (metres) |
 * | 1 | velocity along the slope, positive downhill (m/s) |
 * | 2 | the incline angle in radians, per particle |
 * | 3 | reserved, written as 0.0 |
 *
 * The angle is per particle rather than one global value so that a single graph can
 * compare two inclines side by side, which is a thing a lab write-up asks for and a
 * global parameter cannot express.
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
 * @tests       plugin.mechanics.incline_block_rests_below_the_static_threshold,
 *              plugin.mechanics.incline_block_slides_above_it,
 *              plugin.mechanics.incline_stops_instead_of_reversing,
 *              plugin.mechanics.incline_handles_every_block_in_the_batch,
 *              plugin.mechanics.incline_rejects_impossible_surfaces,
 *              plugin.mechanics.incline_rejects_unusable_input,
 *              plugin.mechanics.incline_registers_under_its_name,
 *              plugin.mechanics.incline_clamps_and_reports_it
 */
#pragma once

#include <qp/graph/kernels/kernel.hpp>

#include <qp/diag.hpp>

#include <cstddef>
#include <string_view>

namespace qp::plugins::mechanics {

/**
 * @brief Advances a block on an incline by one step, with Coulomb friction.
 *
 * ### Parameter block layout
 *
 * | Slot | Meaning |
 * |---|---|
 * | `reals[0]` | `g`, gravitational acceleration magnitude in m/s^2. Finite and > 0 |
 * | `reals[1]` | `mu_s`, coefficient of static friction. Finite and >= 0 |
 * | `reals[2]` | `mu_k`, coefficient of kinetic friction. Finite and >= 0 |
 *
 * All three are required. `mu_k > mu_s` is refused rather than accepted and ignored: it
 * describes a surface on which starting to slide makes sliding harder, which is not a
 * physical surface, and a graph that says so is a graph whose author has confused the two
 * coefficients. Silently swapping them would hide the mistake and produce numbers that
 * look reasonable.
 *
 * ### The decision taken each step
 *
 * With `a_g = g * sin(theta)` the along-slope acceleration gravity alone would give:
 *
 *   - **at rest** (`v == 0`): move only if `|a_g| > mu_s * g * cos(theta)`. The
 *     comparison is on accelerations rather than forces because the mass cancels in both
 *     -- which is the result the exercise is meant to produce, so the operator does not
 *     carry a mass at all.
 *   - **moving** (`v != 0`): apply `a_g - sign(v) * mu_k * g * cos(theta)`, and if that
 *     would carry the velocity through zero within the step, take the velocity to zero
 *     and stop there rather than accelerating back up the slope.
 *
 * @ownership   owns nothing; the host owns the registry entry and this object
 * @thread      main (prepare), eval (advance)
 * @pre         none
 * @post        none
 * @invariant   After a successful `prepare`, every coefficient is finite and the pair
 *              satisfies `mu_k <= mu_s`
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       plugin.mechanics.incline_block_rests_below_the_static_threshold,
 *              plugin.mechanics.incline_block_slides_above_it,
 *              plugin.mechanics.incline_stops_instead_of_reversing,
 *              plugin.mechanics.incline_handles_every_block_in_the_batch,
 *              plugin.mechanics.incline_rejects_impossible_surfaces,
 *              plugin.mechanics.incline_rejects_unusable_input,
 *              plugin.mechanics.incline_registers_under_its_name,
 *              plugin.mechanics.incline_clamps_and_reports_it
 */
class InclineFriction final : public graph::kernels::IBatchAdvancer {
public:
    /// @brief A stepper that has not been prepared and cannot advance yet.
    InclineFriction() noexcept = default;

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
     * @tests       plugin.mechanics.incline_registers_under_its_name
     */
    [[nodiscard]] std::string_view name() const noexcept override;

    /**
     * @brief What this operator needs from the host.
     *
     * `none`. The friction decision is taken from each particle's own velocity, so no
     * cross-particle state is read and no scratch buffer is required.
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
     * @tests       plugin.mechanics.incline_registers_under_its_name
     */
    [[nodiscard]] graph::kernels::Capability capabilities() const noexcept override;

    /**
     * @brief Reads `g`, `mu_s` and `mu_k` out of the parameter block, or refuses it.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success, `advance` is usable and `is_prepared()` is true
     * @invariant   Calling it twice with equal blocks has the same effect as once
     * @errors      Returns `invalid_argument` for a non-finite or non-positive `g`, a
     *              negative or non-finite coefficient, or `mu_k > mu_s`; a refusal
     *              leaves any previous preparation untouched
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_rejects_impossible_surfaces
     */
    [[nodiscard]] diag::Result<void> prepare(const graph::kernels::ParamBlock& params) override;

    /**
     * @brief Advances every block by `ctx.dt` with one semi-implicit Euler step.
     *
     * @ownership   observes `batch`; writes through `batch.out`
     * @thread      eval
     * @pre         `batch.valid()` and `is_prepared()`
     * @post        Each block's along-slope position is updated by its new velocity
     * @invariant   A block whose static threshold holds keeps the exact velocity it had,
     *              which for a resting block is exactly zero -- no drift, no jitter
     * @errors      Returns `invalid_argument` for an unprepared operator, an unreadable
     *              batch, or a non-positive step; `type_mismatch` for non-f64 data
     * @complexity  O(count)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_block_rests_below_the_static_threshold,
     *              plugin.mechanics.incline_block_slides_above_it
     */
    [[nodiscard]] diag::Result<void> advance(const graph::kernels::BatchView& batch,
                                             graph::kernels::AdvanceContext& ctx) override;


    /**
     * @brief The clamping policy this operator applies to every value it writes.
     *
     * Charter C2 requires a kernel to **declare** its policy rather than choose quietly,
     * because the two available answers are not equivalent and the difference is visible in
     * the physics: a clamped run stays finite and is wrong past the clamp, while an
     * unclamped run produces a non-finite value the host can refuse to record.
     *
     * `bounded` rather than `finite`: a block's position along a slope does grow
     * without limit under gravity -- that is the exercise -- so a magnitude bound is
     * meaningful. `1e12` metres is absurd on purpose. The bound is there to catch a
     * divergence, not to model a slope that ends, and a value that large means the step size
     * or the angle is unusable rather than that the block has travelled a long way.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        `finite_only` is true for the returned policy
     * @invariant   Constant for the lifetime of the object
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_clamps_and_reports_it
     */
    [[nodiscard]] graph::kernels::ClampPolicy clamp_policy() const noexcept;

    /**
     * @brief How many values this operator has clamped since it was prepared.
     *
     * C8 requires that numerical error not be mistaken for physics, and a run that has been
     * clamped looks exactly like one that has not unless somebody says so. Reset by
     * `prepare`, so the count describes the current configuration.
     *
     * @ownership   pure
     * @thread      eval (read after a run)
     * @pre         none
     * @post        none
     * @invariant   Zero immediately after a successful `prepare`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_clamps_and_reports_it
     */
    [[nodiscard]] std::uint64_t clamps_fired() const noexcept { return clamps_fired_; }

    /**
     * @brief Whether `prepare` has succeeded at least once.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   True for every object whose last `prepare` returned ok
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_rejects_impossible_surfaces
     */
    [[nodiscard]] bool is_prepared() const noexcept { return prepared_; }

    /**
     * @brief The prepared gravitational acceleration, or 0 when unprepared.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   Positive exactly when `is_prepared()`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_rejects_impossible_surfaces
     */
    [[nodiscard]] double gravity() const noexcept { return gravity_; }

    /**
     * @brief The prepared static friction coefficient.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   Unchanged by a refused `prepare`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_rejects_impossible_surfaces
     */
    [[nodiscard]] double static_friction() const noexcept { return mu_static_; }

    /**
     * @brief The prepared kinetic friction coefficient.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   Never exceeds `static_friction()`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.incline_rejects_impossible_surfaces
     */
    [[nodiscard]] double kinetic_friction() const noexcept { return mu_kinetic_; }

private:
    double gravity_ = 0.0;
    double mu_static_ = 0.0;
    double mu_kinetic_ = 0.0;
    bool prepared_ = false;
    std::uint64_t clamps_fired_ = 0;
};

}  // namespace qp::plugins::mechanics
