/**
 * @file boris.hpp
 * @brief The relativistic Boris push, as a particle-domain kernel.
 *
 * ## What this is, and the one thing it must get right
 *
 * `IBatchAdvancer::advance` is called once per host step and loops over every particle internally. What it must
 * get right is the **relativistic** rotation: the Boris scheme rotates the *kinetic momentum* `u = gamma v`, not
 * the velocity, and the angle it rotates through is `q B dt / (2 gamma m)`. A non-relativistic Boris is
 * indistinguishable from this one for a slow particle and wrong by a factor of `gamma` for a 1 MeV electron,
 * which at `L = 1.5` has a gyroradius of 0.06 earth radii -- so the radiation belt this kit exists to show is
 * exactly the regime where the correction is not optional.
 *
 * The five steps, in the standard form:
 *
 *     u_minus = u + (q/m) E dt/2                       (half the electric impulse)
 *     gamma_minus = sqrt(1 + |u_minus|^2 / c^2)
 *     t = (q/m) B (dt/2) / gamma_minus
 *     u_prime = u_minus + u_minus x t
 *     u = u_minus + u_prime x (2t / (1 + |t|^2)) + (q/m) E dt/2
 *     x += (u / gamma) dt
 *
 * ## Sub-stepping, and why the criterion is the rotation angle
 *
 * A step that rotates a particle through a large angle in one go integrates the wrong orbit, and the error is not
 * small -- it is a different trajectory with a different gyroradius. The reference implementation this kit is
 * ported from sub-steps on `omega_c dt > 0.5`, with `omega_c` the **non-relativistic** cyclotron frequency
 * `qB/m`. That threshold is an over-estimate in two independent ways, and the direction is worth stating because
 * the first draft of this comment had it backwards:
 *
 *   - the angle a relativistic particle turns through is `omega_c dt / gamma`, so the reference divides by a
 *     number it has not divided by;
 *   - the rotation the scheme actually applies turns through `2 atan(|t|)`, which is **less** than `2 |t|` for
 *     every `|t| > 0`.
 *
 * Both push the same way: the reference sub-steps **more** than the angle requires, which is safe and expensive,
 * and expensive on exactly the hot particles where a run is already slowest. This file computes the angle the
 * scheme's accuracy actually depends on:
 *
 *     theta = 2 atan(|t|)      with    t = (q/m) B (dt/2) / gamma
 *
 * which is one expression for a relativistic particle and a slow one, because `gamma` is already inside `t`. The
 * difference is a measurement rather than an argument:
 * `magnetosphere.boris.a_relativistic_particle_needs_fewer_substeps` runs two particles in the same field and
 * counts what each criterion would have taken.
 *
 * ## The speed limit, and what happens when it is reached
 *
 * A Boris step cannot exceed `c` -- the rotation preserves `|u|`, and the half-impulses are what can -- but
 * **numerical** drift can push a particle past it, and a `gamma` computed from `|u|^2 > c^2` is a NaN that
 * spreads. So a limit is applied, and three decisions about it are worth stating because the reference
 * implementation takes none of them:
 *
 *   1. it is a **declared parameter** (`kIndexSpeedLimit`) rather than a hard-coded `0.999999c`, so a caller can
 *      lower it for an experiment or raise it; the default is just below `c`;
 *   2. it **counts** what it did (`AdvanceReport::clamped` counts components; this kernel's own
 *      `speed_clamps()` counts particles), because the project's rule is that the platform never clamps
 *      silently -- a wrong answer wearing a right answer's clothes;
 *   3. it is **not** the only thing that happens, because the real answer to "this particle is gyrating faster
 *      than the step can resolve" is sub-stepping, and the sub-step criterion above is what handles it. The
 *      limit is the last resort, and reaching it means the step was not good enough.
 *
 * ## The status codes, which are three and not two
 *
 * A particle that reaches the body, one that leaves the modelled region, and one whose arithmetic became a
 * number no longer: all three stop, and a report that merged them could not say which happened. `absorbed` is
 * the first, `escaped` the other two, and the split is the same one `particles::Status` documents.
 *
 * @ownership   observes the batch it is handed, owns nothing between calls
 * @thread      eval (`advance`), main (`prepare`)
 * @pre         none
 * @post        none
 * @invariant   No allocation, no throw, and no dependence on anything but the batch, the parameters and `ctx`
 * @errors      Reports through `diag::Result` rather than throwing
 * @frozen      no
 * @tests       magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency,
 *              magnetosphere.boris.a_magnetic_field_does_no_work,
 *              magnetosphere.boris.a_round_trip_is_second_order_not_exact,
 *              magnetosphere.boris.a_relativistic_particle_needs_fewer_substeps,
 *              magnetosphere.boris.the_speed_limit_is_counted,
 *              magnetosphere.boris.a_bad_batch_is_refused
 */
#pragma once

#include <qp/graph/field/field.hpp>
#include <qp/graph/kernels/kernel.hpp>
#include <qp/graph/particles/executor.hpp>

#include <qp/plugins/magnetosphere/pusher.hpp>

#include <cstddef>
#include <cstdint>

namespace qp::plugins::magnetosphere {

/**
 * @brief The relativistic Boris pusher, for a batch of charged particles in a magnetic field.
 *
 * @ownership   observes the batch
 * @thread      eval
 * @pre         none
 * @post        none
 * @invariant   The same batch and parameters give bit-identical results
 * @errors      See `advance`
 * @frozen      no
 * @tests       magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency
 */
class BorisAdvancer final : public PusherAdvancer {
public:
    /// @brief The kernel's stable name, as it appears in the registry and in a run's record.
    static constexpr const char* kName = "boris";

    /// One line for the editor.
    static constexpr const char* kSummary =
        "Relativistic Boris push: q(u/c x B), adaptive sub-stepping on the rotation angle";

    // -- The parameter block, declared once for the family in `pusher.hpp` and aliased here so that a reader of
    // -- this header still sees the names this scheme is written in. Two kernels agreeing by convention about a
    // -- slot index would be two chances to disagree; there is one declaration and these are its other spelling.
    /// @brief The radius at which a particle is retired as having left the region, in earth radii.
    static constexpr std::size_t kIndexMaxRange = PusherParams::kIndexMaxRange;
    /// @brief The gravity multiplier. `0` disables gravity; `1` is the Earth's own field.
    static constexpr std::size_t kIndexGravity = PusherParams::kIndexGravity;
    /// @brief The largest number of sub-steps one `advance` may take. A cap, so a pathological field cannot hang.
    static constexpr std::size_t kIndexSubstepCap = PusherParams::kIndexSubstepCap;
    /// @brief The speed limit as a fraction of `c`. Defaults to just below `1`.
    static constexpr std::size_t kIndexSpeedLimit = PusherParams::kIndexSpeedLimit;
    /// @brief Where the field grid's first node is, in metres: `x`, then `y`, then `z`.
    static constexpr std::size_t kIndexGridOrigin0 = PusherParams::kIndexGridOrigin0;
    /// @brief The grid origin's `y`, in metres.
    static constexpr std::size_t kIndexGridOrigin1 = PusherParams::kIndexGridOrigin1;
    /// @brief The grid origin's `z`, in metres.
    static constexpr std::size_t kIndexGridOrigin2 = PusherParams::kIndexGridOrigin2;
    /// @brief The grid's node spacing along `x`, in metres.
    static constexpr std::size_t kIndexGridSpacing0 = PusherParams::kIndexGridSpacing0;
    /// @brief The grid's node spacing along `y`, in metres.
    static constexpr std::size_t kIndexGridSpacing1 = PusherParams::kIndexGridSpacing1;
    /// @brief The grid's node spacing along `z`, in metres.
    static constexpr std::size_t kIndexGridSpacing2 = PusherParams::kIndexGridSpacing2;
    /// @brief How many double slots this kernel reads, checked against the block's own width by `static_assert`.
    ///
    /// A kernel that documented slot 11 and read it would get the block's zero for an out-of-range index rather
    /// than a compile error, and a silent zero is a parameter the user set and the run ignored. The assertion is
    /// what makes the block's width and this list one fact instead of two.
    static constexpr std::size_t kDoublesUsed = PusherParams::kDoublesUsed;
    /// @brief Whether the drag slot is read. `0` or `1`; the reference implementation has the same switch.
    static constexpr std::size_t kIndexUseDrag = PusherParams::kIndexUseDrag;

    /// @brief The fields this kernel cannot run without, as a mask for `StepPlan::required_slots`.
    ///
    /// Declared once for the family in `pusher.hpp`: "a push needs a magnetic field" is knowledge that lives in
    /// exactly one place. A plan that carries this mask and binds no magnetic field is refused by
    /// `ParticleExecutor::prepare` with `slot_unbound` instead of being run: a particle in an absent field travels
    /// in a straight line, and a straight line is indistinguishable from a field model that is broken.
    static constexpr std::uint32_t kRequiredFields = PusherParams::kRequiredFields;

    static_assert(kDoublesUsed <= qp::graph::kernels::ParamBlock::kDoubles,
                  "the parameter block is narrower than this kernel's documented slots");

    /// @brief The default speed limit: `1 - 1e-6`, the reference implementation's own margin.
    ///
    /// Just below `c` rather than at it, because a `gamma` computed from `|u|^2` exactly equal to `c^2` divides by
    /// zero. The margin is a floating-point necessity and not a physical claim, which is why it is a named
    /// constant with that sentence beside it.
    static constexpr double kDefaultSpeedLimit = PusherParams::kDefaultSpeedLimit;

    /// @brief The default rotation angle a sub-step is allowed to turn through, in radians.
    ///
    /// `0.5`, the reference implementation's threshold, kept because it is a reasonable working point and changed
    /// only in **how** it is measured -- as an angle rather than as `omega dt`.
    static constexpr double kDefaultMaxRotation = PusherParams::kDefaultMaxRotation;

    BorisAdvancer() = default;

    [[nodiscard]] std::string_view name() const noexcept override { return kName; }

    /**
     * @brief What this kernel needs from the host.
     *
     * In place, not stochastic, no scratch, and **not** a neighbourhood: every particle's step depends only on
     * its own position and velocity, which is what makes the batch loop a plain `for` and what a future
     * vectorised version would rely on.
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
     * @tests       magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency
     */
    [[nodiscard]] qp::graph::kernels::Capability capabilities() const noexcept override {
        return qp::graph::kernels::Capability::is_in_place;
    }

    /// @brief Whether a `+dt` step followed by a `-dt` step returns to the start.
    ///
    /// **False, and the reason is worth the space because the first draft of this file claimed true.** The
    /// velocity half of Boris is exactly reversible: the rotation `R(theta)` about `B` is inverted by the rotation
    /// through `-theta` about the same axis, which is what a negative `dt` produces, and in exact arithmetic
    /// `R(-theta) R(theta) = I` to the last bit. The **position** half is not. Boris updates `x` with the
    /// post-rotation velocity, `x' = x + (u+/gamma) dt`, so the backward leg moves by `(u/gamma) dt` where the
    /// forward leg moved by `(u+/gamma) dt`, and the round trip returns to
    ///
    ///     x'' - x = (R(theta) - I) (u/gamma) dt        |x'' - x| ~ |v| theta dt
    ///
    /// -- **the step length**, not the rounding. `magnetosphere.boris.a_round_trip_is_second_order_not_exact`
    /// measures it and checks the order, because a claim like this is only worth making with a number behind it.
    ///
    /// The scheme is still symplectic and still volume-preserving, and a genuinely time-reversed trajectory
    /// (`v -> -v`, `t -> -t`) does retrace. What is false is the specific claim this method exists to answer, and
    /// answering it honestly is the point: `grep` for a `true` here and the run ledger reports a reversible
    /// integrator, which is a statement about the physics that nothing would have checked. A caller that needs
    /// the property can have it -- average the pre- and post-rotation velocities in the position update and the
    /// scheme becomes exactly reversible -- and that is a different scheme, with a different name, so it is a
    /// different kernel rather than a quiet change to this one.
    [[nodiscard]] bool is_time_reversible() const noexcept override { return false; }

    /**
     * @brief Advances every particle by `ctx.dt`.
     *
     * The prologue and the epilogue are the family's -- `PusherAdvancer::load` and `finish` -- and what is here is
     * the sub-step loop: half impulse, rotation, half impulse, drag, drift.
     *
     * @param batch The state, in **SI**, which is what `particle_state.hpp` declares its slots to hold and what
     *              every report quotes. `batch.count` is `kBatchSlotCount`: the four state buffers in `BatchSlot`
     *              order -- positions in metres, velocities in metres per second, charge-to-mass ratios in
     *              coulombs per kilogram, status codes -- then the bound fields in `SlotName` order, of which this
     *              kernel reads the magnetic one (a volume lattice in tesla, on the grid the parameters describe
     *              in metres) and, when the drag switch is on, the drag coefficient (a scalar lattice in per
     *              second on the same grid). A slot it does not read is not required to be readable; the one
     *              field it cannot run without is `kRequiredFields`, which is what a plan builder puts in
     *              `StepPlan::required_slots`. The conversion into the loop's normalized units happens inside and
     *              once per particle per step, not per sub-step.
     * @param ctx   The step context. `dt` is in normalized time units and may be negative: the sub-step count is
     *              computed from `|dt|`, so a backward step takes the same sub-steps in the other direction.
     *
     * @ownership   observes
     * @thread      eval
     * @pre         `batch.valid()`, the magnetic slot readable, and a **volume** lattice there
     * @post        Every live particle holds its state after one step, in SI, and the counters have grown
     * @invariant   No allocation, no throw, no host callback per particle, and a retired particle is not moved
     * @errors      `invalid_argument` for a batch that is not `valid()`, for a batch whose count is below
     *              `kBatchSlotCount`, for a magnetic slot that is unreadable or is not a volume of vectors, for a
     *              particle slot that is not f64, and for a zero or non-finite `dt`
     * @complexity  O(particles x substeps)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency,
     *              magnetosphere.boris.a_magnetic_field_does_no_work,
     *              magnetosphere.boris.a_round_trip_is_second_order_not_exact
     */
    [[nodiscard]] qp::diag::Result<void> advance(const qp::graph::kernels::BatchView& batch,
                                                 qp::graph::kernels::AdvanceContext& ctx) override;

private:
    /**
     * @brief One particle's sub-step loop, which is the whole of what this scheme adds to the family.
     *
     * @ownership   mutates `loaded`
     * @thread      eval
     * @pre         `loaded.usable`, and the fields the batch carries are on the parameters' grid
     * @post        `loaded.position` and `loaded.velocity` hold the state after `loaded.substeps` sub-steps
     * @invariant   The rotation's two shears are constructed so that the scale factors cancel, so `|u|` survives
     *              the magnetic part to the rounding
     * @errors      noexcept
     * @complexity  O(substeps)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_magnetic_field_does_no_work
     */
    void push(PusherAdvancer::Loaded& loaded, const qp::graph::kernels::BatchView& batch,
              const qp::graph::kernels::AdvanceContext& ctx) noexcept;
};

}  // namespace qp::plugins::magnetosphere
