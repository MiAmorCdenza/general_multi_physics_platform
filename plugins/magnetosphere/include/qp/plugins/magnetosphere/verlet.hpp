/**
 * @file verlet.hpp
 * @brief The velocity-Verlet push: position first, the force at the half step, and no second read of the field.
 *
 * ## Why a third scheme, and why this one
 *
 * The reference implementation declares **four** integrators -- Boris, leapfrog, Runge-Kutta and Verlet -- with
 * identical sockets, because to a user they are four answers to one question. Two of them are here, and the
 * question this file answers is what happened to the other two.
 *
 * **Boris already is the reference's leapfrog, and that is a measurement rather than a reading.** The reference
 * describes its leapfrog as "Boris rotation (magnetic) + kick-drift-kick (E/gravity/drag), `|v|` preserved", and
 * that is exactly what `BorisAdvancer` does: a half impulse, the exact rotation, the other half impulse, then the
 * drift. A second node with the same arithmetic would be **two names for one scheme** -- the same objection that
 * kept the reference's `kan` out of the tail -- and the case records it instead: in a static gravity field both
 * show the symplectic signature (a bounded, oscillatory energy error rather than a secular one) and both are
 * second order, which is what the reference's name claims for it.
 *
 * **Verlet is genuinely a different scheme**, and the difference is *where the force is evaluated*. Boris kicks
 * both halves at the **start** position; velocity Verlet drifts half a step first and evaluates the force at the
 * **midpoint**:
 *
 *     x_half = x + (dt/2) v                    drift half
 *     a      = F(x_half)                       one evaluation, at the midpoint
 *     v_new  = R( v + dt a )                   kick, with the magnetic rotation
 *     x_new  = x_half + (dt/2) v_new           drift half
 *
 * That is the reference's "position first", and it is the leapfrog family's characteristic form: the drift is
 * split around the kick, so the force is sampled where the particle actually is during the step rather than where
 * it started. The case measures the consequence -- a smaller energy error than Boris at the same step and the same
 * cost, both second order -- rather than asserting which is better, because which is better depends on the
 * question: Boris's rotation is exact for the magnetic part and Verlet's midpoint force is more accurate for the
 * translational one, and a particle in a strong field and a particle in a strong potential do not want the same
 * answer.
 *
 * ## What it costs, and the number the header used to claim
 *
 * `Rk4Advancer::kSamplesPerSubstep` said "four field samples a sub-step" and nothing checked it. It has four
 * **stages**, each of which reads the magnetic table *and* the electric one -- so the real cost is eight, and the
 * family now counts its reads instead of documenting them (`PusherAdvancer::last_field_samples`). Verlet costs
 * one read of each table per sub-step: no more than Boris, and a quarter of RK4's.
 *
 * ## Drag, which the reference's own description leaves out
 *
 * The reference lists "E/gravity" for its Verlet and not drag, and that is right about the *scheme*: drag is a
 * velocity-dependent force, and the symmetry that makes velocity Verlet symplectic is a statement about the
 * conservative part. This kit does not silently drop a wired input, so a bound drag table is applied the way the
 * family's other second-order scheme applies it -- the decay factor after the kick -- and the contract says that
 * the symplectic property being measured is about the conservative part. A run that wants drag to be second-order
 * accurate uses RK4, where it is part of the right-hand side, and the difference is `O((nu dt)^2)` per step.
 *
 * @ownership   owns (its counters, through `PusherAdvancer`)
 * @thread      main (prepare) / eval (advance)
 * @pre         none
 * @post        none
 * @invariant   The same batch and parameters give bit-identical results
 * @errors      See `advance`
 * @frozen      no
 * @tests       magnetosphere.verlet.the_second_order_schemes_cost_the_same_and_rk4_does_not
 */
#pragma once

#include <qp/plugins/magnetosphere/pusher.hpp>

#include <string_view>

namespace qp::plugins::magnetosphere {

/**
 * @brief The velocity-Verlet push: half a drift, the force at the midpoint, the rotation, half a drift.
 *
 * @ownership   owns
 * @thread      main (prepare) / eval (advance)
 * @pre         none
 * @post        none
 * @invariant   `|v|` is preserved by the rotation to the rounding, as the family's other schemes preserve it
 * @errors      See `advance`
 * @frozen      no
 * @tests       magnetosphere.verlet.the_second_order_schemes_cost_the_same_and_rk4_does_not
 */
class VerletAdvancer final : public PusherAdvancer {
public:
    /// @brief The kernel's stable name, as it appears in a run's record.
    static constexpr const char* kName = "verlet";

    /// One line for the editor.
    static constexpr const char* kSummary =
        "Velocity Verlet: the force at the half step, one field sample a sub-step, exact rotation";

    /// @brief How many of each table one sub-step reads. One, and the counter proves it.
    static constexpr std::size_t kSamplesPerSubstep = 1;

    VerletAdvancer() = default;

    [[nodiscard]] std::string_view name() const noexcept override { return kName; }

    /**
     * @brief What this kernel needs from the host: in place, not stochastic, no scratch.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        `is_in_place` is set and nothing else is
     * @invariant   Constant for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.verlet.the_second_order_schemes_cost_the_same_and_rk4_does_not
     */
    [[nodiscard]] qp::graph::kernels::Capability capabilities() const noexcept override {
        return qp::graph::kernels::Capability::is_in_place;
    }

    /**
     * @brief Whether a `+dt` step followed by a `-dt` step returns to the start. **True, and it is the scheme's
     * definition rather than an approximation.**
     *
     * The step is symmetric about its own midpoint: half a drift, a kick, half a drift, with the kick taken where
     * the two halves meet. Reversing `dt` reverses each half and the kick with it, so the composition is its own
     * inverse up to the rounding of the arithmetic -- the property this family reports as `is_reversible` and the
     * one the reference's "position first" name is pointing at.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Always true
     * @invariant   Constant for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.verlet.a_round_trip_returns_to_the_start
     */
    [[nodiscard]] bool is_time_reversible() const noexcept override { return true; }

    /**
     * @brief One step of every live particle in the batch.
     *
     * @param batch The state, in SI. Must carry the seven slots the family declares.
     * @param ctx   The step: its size, and nothing else this scheme reads.
     *
     * @ownership   observes
     * @thread      eval
     * @pre         `batch.valid()` and the magnetic slot is a sampleable volume
     * @post        Every live particle has advanced by `ctx.dt`
     * @invariant   Retired particles are left exactly as they were
     * @errors      `invalid_argument` for an invalid batch, a step that is not finite or is zero, or a batch whose
     *              slots are not f64
     * @complexity  O(particles * substeps)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.verlet.the_second_order_schemes_cost_the_same_and_rk4_does_not
     */
    [[nodiscard]] qp::diag::Result<void> advance(const qp::graph::kernels::BatchView& batch,
                                                 qp::graph::kernels::AdvanceContext& ctx) override;

private:
    /// @brief One particle's step: the two half drifts and the kick between them.
    void push(PusherAdvancer::Loaded& loaded, const qp::graph::kernels::BatchView& batch,
              const qp::graph::kernels::AdvanceContext& ctx) noexcept;
};

}  // namespace qp::plugins::magnetosphere
