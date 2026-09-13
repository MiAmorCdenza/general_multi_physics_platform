/**
 * @file pusher.hpp
 * @brief What every particle pusher in this kit shares: the parameter block, the units boundary, the sampling, and
 *        the sub-step control.
 *
 * ## Why there is a base class at all
 *
 * This kit ships a **family** of integrators -- Boris and the classical fourth-order Runge-Kutta today, the
 * reference implementation's leapfrog and Verlet named for later -- and the reference declares them as four node
 * types with **identical sockets and identical parameters**, because to a user they are four answers to one
 * question: which scheme advances my particles. Two of them differ in the five lines inside the sub-step loop; all
 * of them agree on everything around it:
 *
 *   - **the parameter block's layout**. Six grid numbers and four scalars, read by index. Two kernels agreeing by
 *     convention is two chances to disagree; the indices are declared once, here, and both schemes alias them;
 *   - **the units boundary**. The batch is SI and the loop is normalized, so every scheme converts in and out the
 *     same way. `units.hpp` derives the constants; this file is where they meet the buffers;
 *   - **the sub-step control**. `2 atan(|t|)` with `t` from the field at the start of the step, capped by the
 *     node's own parameter. It is a property of the *family* rather than of Boris: keeping it identical is what
 *     makes two schemes comparable at one cadence, which is the only way a case can measure "this one is more
 *     accurate than that one" without also changing the step;
 *   - **the prologue and the epilogue**. Finiteness, the light-speed clamp, the retirement rules at the body and
 *     at the boundary, and the conversion back. A scheme that got the *boundary* wrong would look like a scheme
 *     that is bad at physics.
 *
 * What is left to a scheme is the sub-step loop: the vector algebra that turns a state into the next state. That
 * is the part that deserves its own file, and it is the only part that does.
 *
 * ## The counters
 *
 * `speed_clamps()`, `retirements()` and `last_substeps()` live here rather than in one scheme, because a run
 * reports them whoever advanced the particles. `run.cpp` reads them through this base -- the downcast it used to
 * make to `BorisAdvancer` by name -- so a second scheme is reported by the same code rather than by a second
 * branch that somebody has to remember to add.
 *
 * @ownership   owns (its counters)
 * @thread      main (prepare) / eval (advance)
 * @pre         none
 * @post        none
 * @invariant   A scheme's `advance` leaves every live particle in SI, as it found it
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency,
 *              magnetosphere.rk4.the_stability_function_is_the_amplitude_it_loses
 */
#pragma once

#include <qp/graph/kernels/kernel.hpp>
#include <qp/graph/particles/executor.hpp>
#include <qp/graph/particles/particle_state.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/geometry.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qp::plugins::magnetosphere {

namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;
namespace gfield = qp::graph::field;

/**
 * @brief The parameter block every pusher in this kit reads: the same numbers, in the same slots.
 *
 * @ownership   pure (a table of constants)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The indices are the block's layout, and the layout is this kit's promise to its own kernels
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads
 */
struct PusherParams final {
    /// @brief The radius at which a particle is retired as having left the region, in earth radii.
    static constexpr std::size_t kIndexMaxRange = 0;
    /// @brief The gravity multiplier. `0` disables gravity; `1` is the Earth's own field.
    static constexpr std::size_t kIndexGravity = 1;
    /// @brief The largest number of sub-steps one `advance` may take. A cap, so a pathological field cannot hang.
    static constexpr std::size_t kIndexSubstepCap = 2;
    /// @brief The speed limit as a fraction of `c`. Defaults to just below `1`.
    static constexpr std::size_t kIndexSpeedLimit = 3;
    /// @brief Where the field grid's first node is, in metres: `x`, then `y`, then `z`.
    ///
    /// A `field::FieldValue` describes a lattice's **counts** and says nothing about where it sits, so the six
    /// numbers that place it travel in the parameter block -- the one place a kernel's configuration already
    /// lives, and the alternative would be a new type in `core/graph/field`, a module that exists precisely
    /// because it has no opinion about physics.
    static constexpr std::size_t kIndexGridOrigin0 = 4;
    /// @brief The grid origin's `y`, in metres.
    static constexpr std::size_t kIndexGridOrigin1 = 5;
    /// @brief The grid origin's `z`, in metres.
    static constexpr std::size_t kIndexGridOrigin2 = 6;
    /// @brief The grid's node spacing along `x`, in metres.
    static constexpr std::size_t kIndexGridSpacing0 = 7;
    /// @brief The grid's node spacing along `y`, in metres.
    static constexpr std::size_t kIndexGridSpacing1 = 8;
    /// @brief The grid's node spacing along `z`, in metres.
    static constexpr std::size_t kIndexGridSpacing2 = 9;
    /// @brief How many double slots a pusher reads, checked against the block's own width by `static_assert`.
    static constexpr std::size_t kDoublesUsed = 10;
    /// @brief Whether the drag slot is read. `0` or `1`; the reference implementation has the same switch.
    static constexpr std::size_t kIndexUseDrag = 0;

    /// @brief The default speed limit: `1 - 1e-6`, the reference implementation's own margin.
    ///
    /// Just below `c` rather than at it, because a `gamma` computed from `|u|^2` exactly equal to `c^2` divides by
    /// zero. The margin is a floating-point necessity and not a physical claim.
    static constexpr double kDefaultSpeedLimit = 0.999999;

    /// @brief The default rotation angle a sub-step is allowed to turn through, in radians.
    ///
    /// `0.5`, the reference implementation's threshold, kept because it is a reasonable working point and changed
    /// only in **how** it is measured -- as an angle rather than as `omega dt`.
    static constexpr double kDefaultMaxRotation = 0.5;

    /// @brief The fields a pusher cannot run without, as a mask for `StepPlan::required_slots`.
    ///
    /// "A push needs a magnetic field" is knowledge that lives in exactly one place, and this is it: a plan that
    /// carries this mask and binds no magnetic field is refused by `ParticleExecutor::prepare` with `slot_unbound`
    /// instead of being run, because a particle in an absent field travels in a straight line and a straight line
    /// is indistinguishable from a field model that is broken.
    static constexpr std::uint32_t kRequiredFields =
        qp::graph::particles::slot_bit(qp::graph::particles::SlotName::magnetic);
};

static_assert(PusherParams::kDoublesUsed <= qp::graph::kernels::ParamBlock::kDoubles,
              "the parameter block is narrower than the pushers' documented slots");

/// @brief The status codes, taken from the module that owns them rather than re-spelled here.
///
/// `particles::Status` is the definition; a plugin that wrote its own `0.0`, `1.0`, `2.0` would be a second
/// definition, and the day one of them changed the failure would be a particle that is retired in one file and
/// live in the other. The conversion is explicit because the slot is a double: `field::FieldValue` reads f64 and
/// nothing else.
inline constexpr double kStatusLive = static_cast<double>(pp::Status::live);
/// @brief The status a particle gets when it reaches the body.
inline constexpr double kStatusAbsorbed = static_cast<double>(pp::Status::absorbed);
/// @brief The status a particle gets when it leaves the modelled region or its state stops being a number.
inline constexpr double kStatusEscaped = static_cast<double>(pp::Status::escaped);

/**
 * @brief Where the field grid's first node is, and how far apart they are -- in SI, because the table is SI.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Both vectors are finite for any block a pusher's `prepare` accepted
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads
 */
struct GridMetadata final {
    /// Where node `(0, 0, 0)` is, in metres.
    Vec3 origin{};
    /// The distance between neighbouring nodes, in metres.
    Vec3 spacing{};
};

/**
 * @brief The grid a plan's field slots were baked onto, read from the parameter block.
 *
 * @param params The block. Borrowed.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The six numbers the block carries, in the order `PusherParams` names them
 * @invariant   Reads no slot outside the six grid slots
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads
 */
[[nodiscard]] inline GridMetadata grid_of(const pk::ParamBlock& params) noexcept {
    GridMetadata grid;
    grid.origin = Vec3{params.real(PusherParams::kIndexGridOrigin0),
                       params.real(PusherParams::kIndexGridOrigin1),
                       params.real(PusherParams::kIndexGridOrigin2)};
    grid.spacing = Vec3{params.real(PusherParams::kIndexGridSpacing0),
                        params.real(PusherParams::kIndexGridSpacing1),
                        params.real(PusherParams::kIndexGridSpacing2)};
    return grid;
}

/**
 * @brief Whether a grid's spacing can be divided by.
 *
 * @param grid The grid. Borrowed.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        True exactly when every component of the origin and spacing is finite and every spacing is positive
 * @invariant   A true answer means the sampler can compute an index for any point
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.boris.a_bad_batch_is_refused
 */
[[nodiscard]] inline bool grid_is_usable(const GridMetadata& grid) noexcept {
    return std::isfinite(grid.origin.x) && std::isfinite(grid.origin.y) && std::isfinite(grid.origin.z) &&
           std::isfinite(grid.spacing.x) && std::isfinite(grid.spacing.y) && std::isfinite(grid.spacing.z) &&
           grid.spacing.x > 0.0 && grid.spacing.y > 0.0 && grid.spacing.z > 0.0;
}

/**
 * @brief Whether a slot is a volume of vectors on three axes, which is the only shape a pusher samples.
 *
 * A `point` lattice describes one value and carries zero counts, so reading one as a volume would silently produce
 * a zero field: the particle travels in a straight line and nothing says why. Refusing it names the problem.
 *
 * @param field The slot. Borrowed.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        True exactly for a readable f64 vector volume with at least two nodes an axis
 * @invariant   Never true for a scalar table
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.boris.a_bad_batch_is_refused
 */
[[nodiscard]] inline bool is_sampleable_volume(const gfield::FieldValue& field) noexcept {
    return gfield::is_readable(field) && field.kind() == gfield::Kind::Volume && field.is_vector() &&
           field.desc.count[0] >= 2 && field.desc.count[1] >= 2 && field.desc.count[2] >= 2;
}

/**
 * @brief Whether a slot is a volume of scalars on three axes, which is the drag table's shape.
 *
 * @param field The slot. Borrowed.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        True exactly for a readable f64 scalar volume with at least two nodes an axis
 * @invariant   Never true for a vector table
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.boris.the_speed_limit_is_counted
 */
[[nodiscard]] inline bool is_sampleable_scalar_volume(const gfield::FieldValue& field) noexcept {
    return gfield::is_readable(field) && field.kind() == gfield::Kind::Volume && field.is_scalar() &&
           field.desc.count[0] >= 2 && field.desc.count[1] >= 2 && field.desc.count[2] >= 2;
}

/**
 * @brief The vector field at `point` (SI), read through the sampler this kit shares with its emitter.
 *
 * @param field The table. Borrowed; must be a volume of vectors.
 * @param grid  Where its samples are.
 * @param point Where to evaluate, in metres.
 *
 * @ownership   pure
 * @thread      eval
 * @pre         `is_sampleable_volume(field)`
 * @post        The trilinear blend, clamped to the boundary as `sample_baked` documents
 * @invariant   One sampler for the whole kit: the emitter and every pusher read the same way
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.baked_field.a_kernel_and_an_emitter_read_the_same_table
 */
[[nodiscard]] inline Vec3 sample_volume(const gfield::FieldValue& field, const GridMetadata& grid,
                                        const Vec3& point) noexcept {
    return sample_baked(field, grid.origin, grid.spacing, point);
}

/**
 * @brief The scalar field at `point` (SI), read through the same sampler.
 *
 * @param field The table. Borrowed; must be a volume of scalars.
 * @param grid  Where its samples are.
 * @param point Where to evaluate, in metres.
 *
 * @ownership   pure
 * @thread      eval
 * @pre         `is_sampleable_scalar_volume(field)`
 * @post        The trilinear blend, clamped to the boundary
 * @invariant   One sampler for the whole kit
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.baked_field.a_kernel_and_an_emitter_read_the_same_table
 */
[[nodiscard]] inline double sample_scalar(const gfield::FieldValue& field, const GridMetadata& grid,
                                          const Vec3& point) noexcept {
    return sample_baked_scalar(field, grid.origin, grid.spacing, point);
}

/**
 * @brief The part of a pusher that is the same for every scheme: the units boundary, the sub-step control, the
 *        retirement rules and the counters.
 *
 * A scheme derives from this, implements `advance`, and is free to be five lines or fifty. What it may not do is
 * disagree with its siblings about where the boundary is, how a state crosses it, or when a particle is retired --
 * those are the family's promises, and they are the difference between "this scheme is less accurate" and "this
 * scheme is wrong about the box".
 *
 * @ownership   owns (its counters and its copy of the parameters)
 * @thread      main (prepare) / eval (advance)
 * @pre         none
 * @post        none
 * @invariant   `prepare` succeeds before `advance` is called, as `ParticleExecutor` sequences them
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency
 */
class PusherAdvancer : public qp::graph::kernels::IBatchAdvancer {
public:
    /**
     * @brief Validates the parameters before any step runs.
     *
     * @param params The block. The step size is **not** here -- it arrives with `AdvanceContext`, and a slot for it
     *               would be a parameter the user could set and the run ignore. The rest must be usable: a
     *               non-positive range, a negative gravity multiplier, a speed limit outside `(0, 1)`, a sub-step
     *               cap below one and a grid spacing that cannot be divided by are all refused here, while the
     *               user is still editing the graph rather than in the middle of a run.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success `advance` can run with this block
     * @invariant   Calling twice with equal arguments has the same effect as once
     * @errors      `invalid_argument` for a range that is not positive, a negative gravity multiplier, a speed
     *              limit outside `(0, 1)`, a sub-step cap below one, or a grid origin or spacing that is not
     *              finite or not positive
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_bad_batch_is_refused
     */
    [[nodiscard]] qp::diag::Result<void> prepare(const qp::graph::kernels::ParamBlock& params) override;

    /**
     * @brief How many particles have had their speed limited since construction.
     *
     * The evidence for a statement a report has to make. Zero is a real answer meaning "no step was ever
     * throttled"; a growing count on a run that is also being sub-stepped says the field is stronger than the
     * step can resolve, which is a configuration finding rather than a numerical one.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        The number of clamps since the last `reset_counts`
     * @invariant   Never decreases except through `reset_counts`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.the_speed_limit_is_counted
     */
    [[nodiscard]] std::uint64_t speed_clamps() const noexcept { return speed_clamps_; }

    /**
     * @brief How many particles have been retired, by the body or by the boundary.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        The number of retirements since the last `reset_counts`
     * @invariant   Never decreases except through `reset_counts`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_bad_batch_is_refused
     */
    [[nodiscard]] std::uint64_t retirements() const noexcept { return retirements_; }

    /**
     * @brief How many sub-steps the last `advance` took, summed over every particle.
     *
     * The number that says whether the sub-stepping is doing anything. A run whose count equals its particle
     * count is taking one sub-step each, which is the common case and the cheap one.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        The count the last `advance` accumulated
     * @invariant   Reset at the start of every `advance`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_relativistic_particle_needs_fewer_substeps
     */
    [[nodiscard]] std::uint64_t last_substeps() const noexcept { return last_substeps_; }

    /**
     * @brief Forgets the counters, so a second run is reported as its own.
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        All three counters are zero
     * @invariant   Does not touch the parameters
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.rk4.a_run_reports_which_scheme_it_used
     */
    void reset_counts() noexcept {
        speed_clamps_ = 0;
        retirements_ = 0;
        last_substeps_ = 0;
    }

protected:
    /**
     * @brief Zeroes the sub-step counter at the start of an `advance`, so the count reported is **that** step's.
     *
     * @ownership   observes
     * @thread      eval
     * @pre         none
     * @post        `last_substeps()` is zero
     * @invariant   Leaves the speed-clamp and retirement counts alone, which accumulate across a run
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_relativistic_particle_needs_fewer_substeps
     */
    void begin_advance() noexcept { last_substeps_ = 0; }

    /**
     * @brief One particle, loaded across the units boundary and ready for a scheme's loop.
     *
     * @ownership   owns
     * @thread      eval
     * @pre         none
     * @post        none
     * @invariant   `substeps` is at least one when `usable` is true
     * @errors      noexcept
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency
     */
    struct Loaded final {
        /// The position in earth radii.
        Vec3 position{};
        /// The velocity in units of `c`.
        Vec3 velocity{};
        /// The magnetic field at the starting position, in units of the equatorial surface value.
        Vec3 magnetic_start{};
        /// The charge-to-mass ratio, already an angular frequency in the loop's units.
        double charge_mass = 0.0;
        /// How many sub-steps this particle's step is split into.
        std::size_t substeps = 1;
        /// False when the state was not a number and the particle has already been retired.
        bool usable = false;
    };

    /**
     * @brief Converts one particle's state into the loop's units, clamps its speed, and counts its sub-steps.
     *
     * The three quantities a step reads are converted **once per particle per host step**, not per sub-step: it is
     * nine multiplies against a loop that already does hundreds. A state that is not a number is retired here,
     * because a NaN that entered the loop would leave as a NaN and take the report with it.
     *
     * @param batch   The state, in SI.
     * @param particle Which particle. Must be below `batch.count`.
     * @param status  The status buffer, written when the state is not finite.
     * @param dt      The step, in normalized time units. `|dt|` is what enters the sub-step count.
     *
     * @ownership   observes
     * @thread      eval
     * @pre         The magnetic slot is readable and on a usable grid
     * @post        On `usable`, the state is in the loop's units and `substeps >= 1`
     * @invariant   A state that is not finite is retired and counted exactly once
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.the_speed_limit_is_counted
     */
    [[nodiscard]] Loaded load(const qp::graph::kernels::BatchView& batch, std::size_t particle, double* status,
                              double dt) noexcept;

    /**
     * @brief Applies the body and boundary rules, and writes the state back in SI.
     *
     * The boundary is the family's, not the scheme's: a particle that reached the body is absorbed, one that left
     * the region is escaped, and one whose state stopped being a number is escaped -- in every scheme, at the same
     * radius, with the same margin past the range so that a particle whose orbit reaches the boundary is not
     * clipped by it.
     *
     * @param loaded       What `load` returned, for the charge and the sub-step count.
     * @param position     The state after the step, in earth radii.
     * @param velocity     The state after the step, in units of `c`.
     * @param position_out The SI position buffer, written.
     * @param velocity_out The SI velocity buffer, written.
     * @param status       The status buffer, written.
     *
     * @ownership   borrows the three buffers, which it writes
     * @thread      eval
     * @pre         `loaded.usable`
     * @post        The three buffers hold this particle's state after the step, or its retirement
     * @invariant   Writes exactly one particle's worth of each buffer
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.boris.a_bad_batch_is_refused
     */
    void finish(const Loaded& loaded, const Vec3& position, const Vec3& velocity, double* position_out,
                double* velocity_out, double* status) noexcept;

    /**
     * @brief The parameters `prepare` accepted.
     * @ownership   observes
     * @thread      eval
     * @pre         `prepare` succeeded
     * @post        The block the last `prepare` was given
     * @invariant   Never written by `advance`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads
     */
    [[nodiscard]] const qp::graph::kernels::ParamBlock& params() const noexcept { return params_; }

    /**
     * @brief The grid the bound fields were baked on, from the parameters.
     *
     * @ownership   pure
     * @thread      eval
     * @pre         `prepare` succeeded
     * @post        The six grid numbers of the accepted block
     * @invariant   One grid per step: the plan refused two lattices before this ran
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.plan.two_sockets_on_two_lattices_are_refused
     */
    [[nodiscard]] GridMetadata grid() const noexcept { return grid_of(params_); }

    /**
     * @brief Whether the drag slot is switched on **and** holds a table this kernel can read.
     *
     * @ownership   pure
     * @thread      eval
     * @pre         none
     * @post        True exactly when the node's switch is set and the slot is a scalar volume
     * @invariant   A switch that is off is off whatever the slot holds
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.an_atmosphere_thins_the_way_an_exponential_does
     *
     * @param batch The batch whose drag slot is in question.
     */
    [[nodiscard]] bool drag_enabled(const qp::graph::kernels::BatchView& batch) const noexcept;

private:
    qp::graph::kernels::ParamBlock params_{};
    std::uint64_t speed_clamps_ = 0;
    std::uint64_t retirements_ = 0;
    std::uint64_t last_substeps_ = 0;
};

}  // namespace qp::plugins::magnetosphere
