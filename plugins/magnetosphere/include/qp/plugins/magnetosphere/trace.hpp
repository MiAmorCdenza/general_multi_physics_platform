/**
 * @file trace.hpp
 * @brief Field lines: the curves a baked field's direction field integrates to.
 *
 * ## Why this is content and not `core/`
 *
 * `core/graph/field` says what a field *is* -- a described span of samples, is it readable, what shape -- and
 * holds no algorithm. `core/graph/kernels` owns the operator contract and by its own words "contains not a single
 * integrator": Euler, leapfrog, RK4 and Boris are all defensible answers, and which one is right depends on the
 * physics and on what a course is trying to show. A field-line tracer is the same kind of object as an
 * integrator: it is *how* a curve is found, and the answer is a numerical method with a tolerance, not a
 * definition. So it lives in the kit beside the models it traces, and a second tracer (a cheaper Euler one for a
 * preview, a symplectic one for a drift shell) is a second implementation of a free function rather than a
 * change to any interface.
 *
 * ## The method, and why this one
 *
 * The reference implementation this kit is ported from uses Geopack's `TRACE_08`, and its `STEP_08` is
 * **Runge-Kutta-Merson**: five stages, with the embedded estimate
 *
 *     err = |R1 - 4.5 R3 + 4 R4 - 0.5 R5|
 *
 * which costs five field reads and gives both the step and its own error, so the step size adapts without a
 * second-order comparison and without ever forming a Jacobian. That matters here for a reason that is about this
 * platform rather than about elegance: **a field read is a trilinear interpolation of a baked table**, so it is
 * cheap but not free and not exact, and a method that sampled the field fewer times per unit length would be
 * trading accuracy in the interpolant for speed in the integrator. Five reads per step at a step that grows to
 * `step_max()` when the field is smooth is the right balance for a curve a person is going to look at.
 *
 * ## The five ways a trace ends, and why each is a named reason
 *
 * Every one of these was learned from a picture that was wrong:
 *
 *   - **`hit_surface`** -- the line reached the inner boundary (the Earth). The last point is placed on the
 *     boundary by linear interpolation of the foot point rather than left at whichever sample happened to fall
 *     inside: a line that overshoots the surface by a quarter of a step draws *through* the planet, and in a
 *     dipole picture the whole family of closed lines does this.
 *   - **`left_table`** -- the line left the region the table describes. This is the one the reference documents
 *     at length and it is worth repeating: the sampler **clamps** outside the grid (deliberately -- see
 *     `baked_field.hpp`), so a trace that kept going past the boundary would integrate a *constant* field and
 *     draw a long straight line that looks like data. The stop is therefore at the table's own box, not at a
 *     sphere chosen by the caller.
 *   - **`looped`** -- the line closed on itself. Two detectors, because one is not enough: a line that comes back
 *     within a step of its own seed has closed, and a line that reverses its radial motion more than four times
 *     has circled. The first catches a circular line, whose radius never changes and which the second therefore
 *     cannot see at all; the second catches an elongated orbit, which may pass near its seed long before it
 *     closes. The reversal counter needs a deadband -- on a nearly circular line `dr` is a difference of nearly
 *     equal numbers and its sign is rounding noise -- and that is written where it is applied.
 *   - **`max_points`** -- the budget ran out. Also the reason a caller sees when a curve is so long that the
 *     picture would be a scribble.
 *   - **`no_field`** -- the field is zero (or not finite, or not readable) where the line tried to step. There is
 *     no direction to step in, and inventing one would draw a curve the field does not have.
 *
 * @ownership   observes (a tracer borrows the table it was constructed with)
 * @thread      main (a trace runs when a run finishes, not per frame)
 * @pre         none
 * @post        none
 * @invariant   A tracer never modifies the table it samples
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.trace.a_uniform_field_gives_a_straight_line,
 *              magnetosphere.trace.a_dipole_line_returns_to_its_seed,
 *              magnetosphere.trace.a_closed_line_is_reported_as_looped,
 *              magnetosphere.trace.a_zero_field_stops_before_it_starts,
 *              magnetosphere.trace.the_step_cap_does_not_decide_the_shape,
 *              magnetosphere.trace.leaving_the_table_stops_the_line
 */
#pragma once

#include <qp/graph/field/field.hpp>
#include <qp/plugins/magnetosphere/geometry.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qp::plugins::magnetosphere {

/**
 * @brief Why a trace stopped.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every enumerator has a name from `to_string`
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.trace.leaving_the_table_stops_the_line
 */
enum class TraceStop : std::uint8_t {
    /// The line reached the inner boundary. The last point is on it.
    hit_surface = 0,
    /// The line left the region the table describes. The last point is inside it.
    left_table = 1,
    /// The line closed on itself, or circled without closing.
    looped = 2,
    /// The point budget ran out.
    max_points = 3,
    /// The field is zero or not a number where the line tried to step.
    no_field = 4,
};

/// @brief Stable short name of a stop reason, for a message or a log line.
///
/// @param stop The reason to name.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        One of the names in this enumerator's list, never null
/// @invariant   Total: every enumerator has a name
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       magnetosphere.trace.leaving_the_table_stops_the_line
[[nodiscard]] const char* to_string(TraceStop stop) noexcept;

/**
 * @brief How hard to try: the step cap, the tolerance, and the two budgets.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `tolerance` and `step_max_re` are the only things that decide a line's shape; the budgets only
 *              decide when it stops
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.trace.the_step_cap_does_not_decide_the_shape
 */
struct TraceSpec final {
    /// @brief The largest step the integrator will take, in earth radii.
    ///
    /// A cap rather than a step: the method chooses its own step from its error estimate and this bounds it, so
    /// the value is "how much detail is worth drawing" rather than "how accurate is the answer". The accuracy is
    /// `tolerance`, and the case that pins the difference traces one line at two caps and measures them against
    /// each other.
    double step_max_re = 0.2;
    /// @brief The step error the method is allowed, in earth radii per step.
    ///
    /// 1e-4 is what the reference uses in double precision, and the number is not arbitrary in the way a
    /// tolerance usually is: the field being integrated is a **trilinear interpolation of a table whose spacing
    /// is 0.25 R_E by default**, so the interpolation's own error is of order `spacing^2` and a step error four
    /// orders of magnitude below it is asking the integrator to resolve the interpolant rather than the model.
    /// Tightening it further would buy nothing a picture can show; loosening it past the spacing would draw a
    /// curve that is not the table's.
    double tolerance = 1.0e-4;
    /// @brief The inner boundary, in earth radii: the line stops when it reaches it.
    double surface_radius_re = 1.0;
    /// @brief The most points one direction may produce. Bounds the work and the picture.
    std::size_t max_points = 2000;
    /// @brief How many times one step may be halved before the trace gives up as `no_field`.
    ///
    /// Fifty halvings is 2^-50 of the cap -- far past the point where a failing step means the field is not
    /// there rather than that the step is too big -- and it is a bound on the loop rather than a tuning knob.
    std::size_t max_attempts = 50;

    /// @brief Whether the numbers describe a trace that can run.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        True when the cap, tolerance and radius are positive and finite and both budgets are non-zero
    /// @invariant   Never inspects a field
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.the_step_cap_does_not_decide_the_shape
    [[nodiscard]] bool usable() const noexcept;
};

/**
 * @brief One traced curve, in earth radii.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `points_re` holds no non-finite point; consecutive points are at most `step_max_re` apart
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.trace.a_dipole_line_returns_to_its_seed
 */
struct FieldLine final {
    /// The curve, in earth radii, in the order it was walked. The seed is in the middle when both ways ran.
    std::vector<Vec3> points_re{};
    /// Why the trace stopped. When both directions ran, this is the **first** reason of the two, ordered by how
    /// much it tells a reader: reaching the surface first, then a closed loop, then leaving the table, then a
    /// budget.
    TraceStop stop = TraceStop::max_points;
    /// Path length in earth radii, summed over both directions.
    double length_re = 0.0;
    /// Closest approach to the origin, in earth radii. `+infinity` for a line with no points.
    ///
    /// Carried because it is the one number that says whether a curve is a closed shell line (well above one) or
    /// a line that has fallen into the planet, and a picture cannot be asked.
    double min_radius_re = 0.0;

    /// @brief Whether the trace produced a curve at all.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        True exactly when there are at least two points
    /// @invariant   A single point is not a curve
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.a_uniform_field_gives_a_straight_line
    [[nodiscard]] bool usable() const noexcept { return points_re.size() >= 2; }
};

/**
 * @brief Integrates a baked table's direction field into curves.
 *
 * ## What it is given, and what it is not
 *
 * A `FieldValue` and the geometry that describes where its samples are, which is the same pair every other
 * reader in this kit takes -- `abi::LatticeDesc` carries counts and not positions, so the origin and spacing
 * travel beside it (the reason `BorisAdvancer` documents six parameter slots for the same two vectors).
 *
 * The **table's own box** is derived from those three and is the outer boundary: a trace stops where the samples
 * stop, whatever radius the caller passes to the field the tracer was built over. That is the `left_table`
 * reason above, and it is the difference between a magnetosphere picture and a picture with long straight lines
 * in it.
 *
 * ## Units
 *
 * Positions, seeds, step sizes and tolerances are all in **earth radii**; the table's geometry is in **metres**,
 * because that is what `abi` describes and what a bake writes. The conversion happens once per field read, at
 * the call to `sample_baked`, and it is deliberate that the tracer's own arithmetic never sees a metre: a step
 * cap of `0.2` is a number a content author can reason about (a fifth of a planet per step), while `1275627.4`
 * is not. A tracer that took metres would push the unit system into every parameter of the node that drives it.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The borrowed table is never modified; only `samples()` changes as a trace runs
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.trace.a_uniform_field_gives_a_straight_line
 */
class FieldTracer final {
public:
    /**
     * @brief A tracer over one table.
     *
     * @param table     The samples. Borrowed; must outlive this object. Must be a readable volume of vectors.
     * @param origin_m  Where node `(0, 0, 0)` is, in metres.
     * @param spacing_m The distance between neighbouring nodes, in metres. Each must be positive.
     * @param spec      The step cap, the tolerance and the budgets.
     *
     * @ownership   observes `table`
     * @thread      main
     * @pre         `table` outlives this object
     * @post        `usable()` is true exactly when the table is readable, is a volume of vectors, and the spec's
     *              numbers can run
     * @invariant   No allocation beyond the object itself
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.trace.a_uniform_field_gives_a_straight_line
     */
    FieldTracer(qp::graph::field::FieldValue table, Vec3 origin_m, Vec3 spacing_m,
                TraceSpec spec = TraceSpec{}) noexcept;

    /// @brief Whether this tracer can trace anything.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        True exactly when the table is readable, holds vectors, spans at least two nodes an axis,
    ///              and every spacing is positive and finite
    /// @invariant   A tracer that answers false answers an empty line for every seed
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.a_dipole_line_returns_to_its_seed
    [[nodiscard]] bool usable() const noexcept { return usable_; }

    /// @brief The step cap, the tolerance and the budgets this tracer runs with.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        The spec passed at construction
    /// @invariant   Constant
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.the_step_cap_does_not_decide_the_shape
    [[nodiscard]] const TraceSpec& spec() const noexcept { return spec_; }

    /**
     * @brief The region the samples cover, in earth radii: the box a trace may not leave.
     *
     * @ownership   owns
     * @thread      any
     * @pre         none
     * @post        `min` and `max` in earth radii, ordered, one node apart at least per axis
     * @invariant   `min` and `max` are derived from the geometry and never from a seed
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.trace.leaving_the_table_stops_the_line
     */
    [[nodiscard]] Vec3 box_min_re() const noexcept { return min_re_; }

    /// @brief The far corner of the sampled region, in earth radii. See `box_min_re`.
    ///
    /// @ownership   owns
    /// @thread      any
    /// @pre         none
    /// @post        The corner opposite `box_min_re`
    /// @invariant   `max.x >= min.x` on every axis
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.leaving_the_table_stops_the_line
    [[nodiscard]] Vec3 box_max_re() const noexcept { return max_re_; }

    /**
     * @brief Traces one field line through `seed_re`.
     *
     * Follows the field's **direction**: unit `B` forward and unit `-B` backward, assembled into one curve with
     * the seed in the middle. A field line has no orientation -- the curve through a point is the same curve
     * whichever way it is walked -- so a one-directional trace would draw half a shell and a reader would have to
     * guess where the other half went. `both_ways` false is offered for a caller that wants a ray (an open polar
     * line from the surface outward) and for a case that wants to see one direction's own stop reason.
     *
     * @param seed_re   Where to start, in earth radii.
     * @param both_ways Whether to walk both directions and join them.
     *
     * @ownership   owns the returned line
     * @thread      main
     * @pre         none
     * @post        A line whose points are the seed and its neighbours, or an empty line when the field there is
     *              unusable
     * @invariant   Never writes through the borrowed table; the same seed and spec give the same curve
     * @errors      Cannot fail: an unusable field is the `no_field` stop reason rather than an error code
     * @complexity  O(points x stages), five field reads per stage
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.trace.a_uniform_field_gives_a_straight_line,
     *              magnetosphere.trace.a_dipole_line_returns_to_its_seed,
     *              magnetosphere.trace.a_closed_line_is_reported_as_looped,
     *              magnetosphere.trace.a_zero_field_stops_before_it_starts
     */
    [[nodiscard]] FieldLine trace(const Vec3& seed_re, bool both_ways = true) const;

    /// @brief How many field samples this tracer has read, since construction or the last reset.
    ///
    /// The evidence a report needs to say what a picture cost: five per stage, and a stage per step, so a family
    /// of a hundred lines is a number a caller should be able to see rather than estimate. Counted through a
    /// `mutable` member because sampling is a read -- the same device, and the same reason, as
    /// `BakedField::clamped_samples`.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        Monotone until `reset_samples`
    /// @invariant   Equals the number of `sample_baked` calls every trace made
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.the_step_cap_does_not_decide_the_shape
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }

    /// @brief Forgets the sample count, so a second family is reported as its own.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `samples()` is zero
    /// @invariant   The table is untouched
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.the_step_cap_does_not_decide_the_shape
    void reset_samples() const noexcept { samples_ = 0; }

private:
    /// @brief Unit `B` at a point in earth radii, or a zero vector when there is no usable direction there.
    [[nodiscard]] Vec3 direction(const Vec3& point_re) const noexcept;

    /// @brief Walks one direction from `seed_re`. `step_max_re` signed decides which way.
    ///
    /// @ownership   owns `out`
    /// @thread      main
    /// @pre         `sign` is exactly `+1.0` or `-1.0`
    /// @post        `out` holds the points walked and why the walk ended
    /// @invariant   Every point is inside the table's box
    /// @errors      noexcept
    /// @complexity  O(points x stages)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.trace.a_uniform_field_gives_a_straight_line
    [[nodiscard]] TraceStop walk(const Vec3& seed_re, double sign, FieldLine& out) const;

    /// @brief Whether a point in earth radii is inside the sampled box.
    [[nodiscard]] bool inside(const Vec3& point_re) const noexcept;

    qp::graph::field::FieldValue table_{};
    Vec3 origin_m_{};
    Vec3 spacing_m_{};
    Vec3 min_re_{};
    Vec3 max_re_{};
    TraceSpec spec_{};
    bool usable_ = false;
    /// Mutable because sampling is a read: see `samples()`.
    mutable std::uint64_t samples_ = 0;
};

}  // namespace qp::plugins::magnetosphere
