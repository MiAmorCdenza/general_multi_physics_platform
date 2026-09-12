/**
 * @file collision.hpp
 * @brief A perfectly inelastic collision: every block in the batch ends at one velocity.
 *
 * ## What this is honest about
 *
 * This operator does **not** detect collisions. It has no pairs, no radii, no contact
 * test, and no way to tell two blocks that are touching from two on opposite sides of the
 * room -- because a `BatchView` is a flat array of state with no neighbourhood
 * information in it. The kernel contract offers `is_neighbourhood` for operators that need
 * to read each other, but nothing in this module does, and inventing a plausible-looking
 * pairwise sweep here would produce a "collision" that depends on array order.
 *
 * What it computes is the thing a collision exercise actually measures: the **common
 * velocity a set of masses reaches when they stick together**, which is total momentum
 * over total mass. A student compares that number against the momentum they recorded
 * before the collision and against the kinetic energy they recorded before it, and finds
 * the first conserved and the second not. That comparison is the experiment.
 *
 * The name says `merge` rather than `collision` for that reason. Discovery of *which*
 * particles collide needs contact geometry, and contact geometry is a plugin of its own --
 * one that declares `is_neighbourhood` and operates on a spatial structure this operator
 * has no access to.
 *
 * ## Mass is component 2, and it must be positive
 *
 * The state is `[x, v, m]`. Velocity is mass-independent so the arithmetic is the familiar
 * `p = m v`, and the momentum sum cannot be taken before the masses are known, which is
 * why the mass has to travel with the particle rather than sit in the parameter block -- a
 * parameter block is the same for every particle, and the entire content of a collision is
 * that the particles differ.
 *
 * A zero or negative mass is refused. Zero would contribute nothing to either sum and
 * silently drop a particle from the collision; negative would produce a common velocity
 * outside the range of the inputs, which is arithmetically what the formula says and
 * physically meaningless.
 *
 * @ownership   observes
 * @thread      main (prepare), eval (advance)
 * @pre         none
 * @post        none
 * @invariant   `advance` allocates nothing, throws nothing, and blocks on nothing
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       plugin.mechanics.merge_conserves_momentum,
 *              plugin.mechanics.merge_loses_kinetic_energy,
 *              plugin.mechanics.merge_refuses_partial_restitution,
 *              plugin.mechanics.merge_refuses_unusable_state,
 *              plugin.mechanics.merge_registers_under_its_name
 */
#pragma once

#include <qp/graph/kernels/kernel.hpp>

#include <qp/diag.hpp>

#include <cstddef>
#include <string_view>

namespace qp::plugins::mechanics {

/**
 * @brief Collapses a batch of masses to their common centre-of-mass velocity.
 *
 * ### State layout
 *
 * | Component | Meaning |
 * |---|---|
 * | 0 | position (metres) |
 * | 1 | velocity (m/s) |
 * | 2 | mass (kg). Finite and strictly positive |
 * | 3 | reserved, written as 0.0 |
 *
 * ### Parameter block layout
 *
 * | Slot | Meaning |
 * |---|---|
 * | `reals[0]` | `restitution`, in [0, 1]. 0 means perfectly inelastic |
 *
 * The parameter exists so that a non-unity value is **refused** rather than ignored. A
 * partially elastic collision changes the *velocities* but not the common velocity of a
 * merged pair, and the velocity each body ends at depends on the collision geometry --
 * which this operator does not have. Accepting `restitution = 0.8` and returning the
 * perfectly inelastic answer would be a wrong number presented as a right one; refusing
 * it says the operator cannot do that, which is information the user can act on.
 *
 * ### What is written
 *
 * Every particle's velocity becomes `sum(m_i v_i) / sum(m_i)`. Positions are left
 * unchanged: a merged body's centre of mass moves at the common velocity, and applying
 * that to every particle's position would teleport them all onto one point, which is not
 * what a coalescence does to the *positions* it started with.
 *
 * @ownership   owns nothing; the host owns the registry entry and this object
 * @thread      main (prepare), eval (advance)
 * @pre         none
 * @post        none
 * @invariant   The momentum of the batch after a step equals its momentum before
 * @errors      See each declaration
 * @complexity  O(count)
 * @nondet      none
 * @frozen      no
 * @tests       plugin.mechanics.merge_conserves_momentum,
 *              plugin.mechanics.merge_loses_kinetic_energy,
 *              plugin.mechanics.merge_refuses_partial_restitution,
 *              plugin.mechanics.merge_refuses_unusable_state,
 *              plugin.mechanics.merge_registers_under_its_name
 */
class MergeCollision final : public graph::kernels::IBatchAdvancer {
public:
    /// @brief A stepper that has not been prepared and cannot advance yet.
    MergeCollision() noexcept = default;

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
     * @tests       plugin.mechanics.merge_registers_under_its_name
     */
    [[nodiscard]] std::string_view name() const noexcept override;

    /**
     * @brief What this operator needs from the host.
     *
     * `none`, and the declaration is load-bearing here. The operator reduces a whole batch
     * to one number, which reads like a neighbourhood operation; it is not one. Every
     * particle contributes to a sum and no particle reads another, so no spatial
     * structure, no scratch buffer and no ordering guarantee is required. Declaring
     * `is_neighbourhood` would make the host build and maintain a spatial index for an
     * operator that never queries it.
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
     * @tests       plugin.mechanics.merge_registers_under_its_name
     */
    [[nodiscard]] graph::kernels::Capability capabilities() const noexcept override;

    /**
     * @brief Reads `restitution` out of the parameter block, or refuses it.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success, `advance` is usable and `is_prepared()` is true
     * @invariant   Calling it twice with equal blocks has the same effect as once
     * @errors      Returns `invalid_argument` for a non-finite value, a value outside
     *              [0, 1], or any value other than 0 -- see the class comment for why a
     *              partially elastic collision is refused rather than approximated
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.merge_refuses_partial_restitution
     */
    [[nodiscard]] diag::Result<void> prepare(const graph::kernels::ParamBlock& params) override;

    /**
     * @brief Replaces every velocity with the batch's centre-of-mass velocity.
     *
     * @ownership   observes `batch`; writes through `batch.out`
     * @thread      eval
     * @pre         `batch.valid()` and `is_prepared()`
     * @post        Every particle has the same velocity, and the total momentum is
     *              unchanged from its value before the call
     * @invariant   Allocates nothing: the two sums are scalars, so the reduction does not
     *              need a per-particle accumulator
     * @errors      Returns `invalid_argument` for an unprepared operator, an unreadable
     *              batch, or a non-positive mass; `type_mismatch` for non-f64 data
     * @complexity  O(count)
     * @nondet      none
     * @frozen      no
     * @tests       plugin.mechanics.merge_conserves_momentum,
     *              plugin.mechanics.merge_loses_kinetic_energy
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
    /// @tests       plugin.mechanics.merge_refuses_partial_restitution
    [[nodiscard]] bool is_prepared() const noexcept { return prepared_; }

private:
    bool prepared_ = false;
};

}  // namespace qp::plugins::mechanics
