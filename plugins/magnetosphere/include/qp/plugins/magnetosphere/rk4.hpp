/**
 * @file rk4.hpp
 * @brief The classical fourth-order Runge-Kutta push: four field samples a step, and no exact rotation anywhere.
 *
 * ## Why a second scheme exists at all
 *
 * The reference implementation declares four integrators with **identical sockets and identical parameters** --
 * Boris, leapfrog, Runge-Kutta and Verlet -- because to a user they are four answers to one question. That is the
 * family `pusher.hpp` sets up, and this is its second member: the honest opposite of Boris, which is the pair a
 * course needs to see.
 *
 * **Boris is second order and exact where it matters.** It splits the Lorentz force into a half impulse, an exact
 * rotation about `B`, and the other half impulse, and the rotation is constructed so that its scale factors cancel
 * -- so `|u|` survives the magnetic part to the rounding and a purely magnetic run's kinetic energy does not drift
 * at all. Its error is in the *trajectory*, and it is `O(dt^2)`.
 *
 * **RK4 is fourth order and drifts.** It integrates `du/dt = q'(E + v x B)` and `dx/dt = v` with the classical four
 * stages, which is far more accurate per step and makes no promise about any invariant. Its amplitude error is
 * exactly its stability function: for a rotation at `y = omega dt` the scheme multiplies the perpendicular velocity
 * by `|R(i y)| = sqrt(1 - y^6/72 + y^8/576) < 1`, so a particle in a magnetic field **slowly spirals in**, and the
 * rate is a number the case measures against that closed form. The reference implementation's own one-line note
 * says the same thing from the other side: Boris is its "default graph-bit-identical baseline".
 *
 * Neither is better. Which one a run should use depends on whether the question is "where does this particle go
 * over a long time" (Boris) or "how accurate is one short arc" (RK4), and a measurement platform should be able to
 * put both numbers in front of a student rather than assert one of them.
 *
 * ## The same equation as Boris, deliberately
 *
 * The state is `(x, u)` with `u = gamma v`, so the two schemes solve **the same** relativistic equation of motion
 * and a comparison between them is a comparison of schemes rather than of physics. `E`, gravity and drag enter as
 * they do in Boris -- the same tables, the same normalized units, the same conversions -- with one difference that
 * is the scheme's own: **drag is part of the right-hand side here** (`du/dt += -nu u`) rather than a factor applied
 * after the rotation. That is what a Runge-Kutta step means by a force, and it makes the decay second-order
 * accurate where Boris's factor is first-order; the case measures both rather than claiming the better one.
 *
 * ## The sub-step control is the family's
 *
 * This scheme keeps the same adaptive sub-stepping Boris uses -- the rotation angle at the start of the step -- so
 * that the two can be compared **at one cadence**. A scheme that picked its own step size would make every
 * comparison a statement about two changes.
 *
 * @ownership   owns (its counters, through `PusherAdvancer`)
 * @thread      main (prepare) / eval (advance)
 * @pre         none
 * @post        none
 * @invariant   The same batch and parameters give bit-identical results
 * @errors      See `advance`
 * @frozen      no
 * @tests       magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses
 */
#pragma once

#include <qp/plugins/magnetosphere/pusher.hpp>

#include <string_view>

namespace qp::plugins::magnetosphere {

/**
 * @brief The classical fourth-order Runge-Kutta push, on the same equation and the same grid as Boris.
 *
 * @ownership   owns
 * @thread      main (prepare) / eval (advance)
 * @pre         `prepare` succeeded before any `advance`
 * @post        none
 * @invariant   Four field samples per sub-step, no allocation, no throw
 * @errors      See `advance`
 * @frozen      no
 * @tests       magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses
 */
class Rk4Advancer final : public PusherAdvancer {
public:
    /// @brief The kernel's stable name, as it appears in a run's record.
    static constexpr const char* kName = "rk4";

    /// One line for the editor.
    static constexpr const char* kSummary =
        "Classical RK4: four field samples a sub-step, fourth order, no exact rotation";

    /// @brief How many field samples one sub-step costs. Four stages, one sample each.
    ///
    /// A number rather than a comment because a report should be able to say what a scheme costs: this one samples
    /// the magnetic and electric tables **four times** where Boris samples them once, which is the price of the
    /// order and the reason a run's wall time changes when the integrator does.
    static constexpr std::size_t kSamplesPerSubstep = 4;

    Rk4Advancer() = default;

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
     * @tests       magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses
     */
    [[nodiscard]] qp::graph::kernels::Capability capabilities() const noexcept override {
        return qp::graph::kernels::Capability::is_in_place;
    }

    /**
     * @brief Whether a `+dt` step followed by a `-dt` step returns to the start. **False, and for a different
     * reason than Boris's.**
     *
     * Boris fails to be reversible because of where the position update sits; this scheme fails because a
     * Runge-Kutta step is not a symplectic map at all -- the four stages are a polynomial approximation of the
     * flow, and its inverse is not the same polynomial with `-dt`. Nothing here is worth a `true`.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        False
     * @invariant   Constant for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.rk4.a_run_reports_which_scheme_it_used
     */
    [[nodiscard]] bool is_time_reversible() const noexcept override { return false; }

    /**
     * @brief Advances every particle by `ctx.dt` with four Runge-Kutta stages per sub-step.
     *
     * @param batch The state, in SI, exactly as `BorisAdvancer::advance` documents it: the same slots, the same
     *              field tables, the same required magnetic socket. `prepare` is inherited and validates the same
     *              parameters, so a graph that runs one scheme runs the other.
     * @param ctx   The step context. `dt` is in normalized time units and may be negative.
     *
     * @ownership   observes
     * @thread      eval
     * @pre         `batch.valid()`, the magnetic slot readable and a volume of vectors
     * @post        Every live particle holds its state after one step, in SI, and the counters have grown
     * @invariant   The counters are the family's: a clamp or a retirement is counted here as it is in Boris
     * @errors      `invalid_argument` for a batch that is not `valid()`, for a count below `kBatchSlotCount`, for
     *              an unreadable magnetic slot, for a particle slot that is not f64, and for a zero or non-finite
     *              `dt`
     * @complexity  O(particles x substeps x stages)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses,
     *              magnetosphere.rk4.a_run_reports_which_scheme_it_used
     */
    [[nodiscard]] qp::diag::Result<void> advance(const qp::graph::kernels::BatchView& batch,
                                                 qp::graph::kernels::AdvanceContext& ctx) override;

private:
    /**
     * @brief The right-hand side of the equation of motion: `(v, a)` for a state `(x, u)`.
     *
     * @ownership   pure
     * @thread      eval
     * @pre         The batch's magnetic slot is readable on the parameters' grid
     * @post        `out_velocity` is `u / gamma` and `out_acceleration` is `q'(E + v x B) + g - nu u`
     * @invariant   Reads no state outside the arguments, so a stage cannot see the previous stage's result
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses
     */
    struct Derivative final {
        /// The velocity, which is the position's derivative.
        Vec3 velocity{};
        /// The momentum's derivative: the Lorentz force, gravity, and drag when it is switched on.
        Vec3 acceleration{};
    };

    /**
     * @brief One stage: sample the fields at `state`, and return the derivative there.
     *
     * @param batch  The bound fields. Borrowed.
     * @param state  Where and how fast, in the loop's units.
     * @param charge_mass The particle's `q/m`, already an angular frequency in these units.
     * @param drag   The drag rate at `state`, in per normalized time, or zero when drag is off.
     *
     * @ownership   pure
     * @thread      eval
     * @pre         The tables are readable and the grid is the parameters'
     * @post        The derivative at `state`
     * @invariant   Four calls per sub-step, one per stage, and no state is written
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses
     */
    [[nodiscard]] Derivative stage(const qp::graph::kernels::BatchView& batch, const Vec3& position,
                                   const Vec3& momentum, double charge_mass, bool has_electric,
                                   bool has_drag) const noexcept;
};

}  // namespace qp::plugins::magnetosphere
