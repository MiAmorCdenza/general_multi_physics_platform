/**
 * @file particle_state.hpp
 * @brief Where a particle batch lives, and why it is four arrays rather than one.
 *
 * ## The problem this solves
 *
 * A `kernel::BatchView` describes the state as an array of `field::FieldValue`, and a `FieldValue` names a
 * **uniform** lattice: one element type, one component count, one shape, one dimension for the whole buffer. A
 * particle batch is not uniform. It holds positions and velocities, which are three components of two different
 * physical quantities, alongside a charge-to-mass ratio and a status code, which are scalars of two more.
 *
 * So the state is **four slots**, one per quantity, and this file is where they live. That is not a workaround
 * for the field vocabulary; it is what the vocabulary is for. `abi::LatticeDesc` carries `dimension` precisely
 * so a buffer says what it holds, and a single merged array could not say it -- a seven-component field of
 * mixed dimensions is not a field, it is a struct, and `field::is_valid_field` would be answering a question
 * nobody asked.
 *
 * ## The four slots, and the units of each
 *
 * | slot | components | element | dimension | unit |
 * |---|---|---|---|---|
 * | `position` | 3 | f64 | length | m |
 * | `velocity` | 3 | f64 | velocity | m/s |
 * | `charge_mass` | 1 | f64 | charge/mass | C/kg |
 * | `status` | 1 | f64 | dimensionless | -- |
 *
 * **Every slot is `f64`, and that decision was taken by the platform's owner rather than by this file.** The
 * field module defaults to `f32` (ADR-0005: field data is float32 and lives on a hot path), and a particle
 * batch is the case that default does not cover: a Boris push accumulates hundreds of thousands of steps, and
 * the conservation checks that say whether it is still integrating the right equation are differences between
 * numbers of order one. In `f32` those differences are of the same order as the rounding, which would leave
 * "energy is conserved" and "energy drifts by the rounding" indistinguishable -- the one property the whole
 * integrator is chosen for. `abi::ElementType::f64` exists, `field::get_component` widens from either, and
 * `abi::is_consistent` accepts both, so this costs nothing but bytes.
 *
 * ## Why the units are SI, and why that is not a step backwards
 *
 * The reference implementation this kit is ported from works in normalized units -- lengths in Earth radii,
 * `c = 47.055`, `GM = 1.5398e-6` -- and that is a real and defensible engineering choice: it keeps every number
 * in a run near unity, which is where a double has the most room. Copying it would mean copying five magic
 * constants whose definitions exist only in a comment, and a port whose units cannot be checked by reading it.
 *
 * So the state is SI (metres, seconds, coulombs per kilogram) and the normalized constants are **derived** in
 * `plugins/magnetosphere/units.hpp`, each from the SI constant it stands for, each asserted against the value the
 * reference used. A reader can then check any line of the pusher against the equations rather than against a
 * table of unexplained numbers.
 *
 * ## Status is a number, not an enum, and the reason is the buffer
 *
 * `field::FieldValue` reads `f64` and nothing else, so a status written as an enum would need a second buffer
 * and a conversion. The four states are therefore small integers stored in a double, declared by `Status` so
 * they have names, and compared through `status_of` rather than by spelling the numbers at call sites.
 *
 * @ownership   owns (the arrays it allocates)
 * @thread      main (allocation, layout) / eval (component access)
 * @pre         none
 * @post        none
 * @invariant   Every slot describes `count()` points of the element type and dimension its table row names
 * @errors      See each declaration
 * @frozen      no
 * @tests       particles.state.layout_is_declared_not_assumed,
 *              particles.state.slots_are_double_precision,
 *              particles.state.a_write_is_visible_through_the_view,
 *              particles.state.status_round_trips_through_the_buffer
 */
#pragma once

#include <qp/abi/lattice.hpp>
#include <qp/graph/field/field.hpp>
#include <qp/units/dimensions.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qp::graph::particles {

/// @brief What happened to one particle, as the step loop records it.
///
/// The values are the ones the reference implementation's `p.status[i]` uses, kept deliberately: a port that
/// renumbered them would make every saved diagnostic and every comparison against the reference read the wrong
/// state, and the numbers are small enough that nobody would notice.
enum class Status : std::uint8_t {
    /// Alive and being integrated.
    live = 0,
    /// Reached the body: below the surface radius, so the run is over for this particle.
    ///
    /// Distinct from `escaped` because the two are different findings about the experiment. A particle that hit
    /// the planet is a precipitation event; one that left the box is a boundary condition. A single "not running"
    /// state would make the two indistinguishable in a report, which is the same defect as reporting an unknown
    /// uncertainty as zero.
    absorbed = 1,
    /// Left the modelled region, or produced a non-finite value and was retired.
    escaped = 2,
};

/// @brief Stable short name of a status, for a message or a log line.
///
/// @param status The status to name.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        One of the three names, never null
/// @invariant   Total: every enumerator has a name
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       particles.state.status_round_trips_through_the_buffer
[[nodiscard]] const char* to_string(Status status) noexcept;

/// @brief A particle batch: four typed arrays, and the four views that describe them.
///
/// ## Why the views are produced on demand rather than stored
///
/// A stored `FieldValue` holds a raw pointer into an array that can be reallocated by `resize`, so it would go
/// stale on the first resize and the failure would be a read of freed memory rather than a compile error. The
/// arrays are the storage and the views are a function of them, so this class exposes `slot()` and lets the
/// caller take a view for the duration of a call.
///
/// @ownership   owns
/// @thread      main
/// @pre         none
/// @post        none
/// @invariant   Every array has exactly `count()` entries
/// @errors      See each declaration
/// @frozen      no
/// @tests       particles.state.layout_is_declared_not_assumed
class ParticleState final {
public:
    /// @brief The slots a batch has, in the order `slot()` indexes them.
    ///
    /// A named enumerator per slot rather than bare numbers, because the executor's field bindings and the
    /// kernels' reads both address slots and a literal `2` in two files is two chances to disagree about which
    /// array holds the charge.
    enum class Slot : std::uint8_t {
        /// Positions, three components, metres.
        position = 0,
        /// Velocities, three components, metres per second.
        velocity = 1,
        /// Charge-to-mass ratio, one component, coulombs per kilogram.
        charge_mass = 2,
        /// `Status` as a number, one component, dimensionless.
        status = 3,
    };

    /// @brief How many slots a batch has.
    static constexpr std::size_t kSlotCount = 4;

    /// @brief An empty batch. `count()` is zero and every slot view is invalid but harmless.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `count() == 0` and `is_consistent()` is true
    /// @invariant   Every array is empty
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.layout_is_declared_not_assumed
    ParticleState() = default;

    /**
     * @brief Allocates `n` particles, all live, at rest at the origin with zero charge.
     *
     * A zero charge-to-mass ratio is the honest initial value: a particle whose species nobody has set does not
     * accelerate, which is visible immediately, where a defaulted proton would silently integrate as though the
     * user had chosen one. The kit's emitter is what fills it in.
     *
     * @param n How many particles.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        `count() == n`, every position and velocity is zero, every status is `live`
     * @invariant   Every array has `n` entries
     * @errors      May allocate; allocation failure terminates, as elsewhere in this project
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       particles.state.layout_is_declared_not_assumed
     */
    explicit ParticleState(std::size_t n);

    /// @brief How many particles the batch holds.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        The same number every call until the next `resize`
    /// @invariant   Equal to the length of every array
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.layout_is_declared_not_assumed
    [[nodiscard]] std::size_t count() const noexcept { return count_; }

    /**
     * @brief Reallocates to `n` particles, discarding what was there.
     *
     * Discarding rather than preserving, because every view into the old arrays dies here and a caller that
     * expected its particles to survive would be reading a buffer that moved. A run that changes its particle
     * count has changed its experiment.
     *
     * @param n How many particles.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        `count() == n` and every component is its initial value
     * @invariant   No view taken before this call is valid after it
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       particles.state.a_write_is_visible_through_the_view
     */
    void resize(std::size_t n);

    /**
     * @brief A view of one slot, for the duration of the call.
     *
     * The returned `FieldValue` borrows this object's array: it must not outlive it, and it must not be held
     * across a `resize`. Both are the caller's obligation and both are stated, because a `FieldValue` cannot
     * enforce either -- it is a description and a pointer, which is what makes the hot path allocation-free.
     *
     * @param slot Which array.
     *
     * @ownership   borrows from this object
     * @thread      eval
     * @pre         `slot` is one of the four enumerators
     * @post        A readable view covering `count()` points, or an invalid one for an empty batch
     * @invariant   The description matches the table in the file comment for that slot
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.state.layout_is_declared_not_assumed,
     *              particles.state.slots_are_double_precision,
     *              particles.state.a_write_is_visible_through_the_view
     */
    [[nodiscard]] field::FieldValue slot(Slot slot) noexcept;

    /// @brief The same, read-only.
    ///
    /// @param slot Which array.
    ///
    /// @ownership   borrows from this object
    /// @thread      any
    /// @pre         `slot` is one of the four enumerators
    /// @post        A readable view covering `count()` points, or an invalid one for an empty batch
    /// @invariant   The description matches the table in the file comment for that slot
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.slots_are_double_precision
    [[nodiscard]] field::FieldValue slot(Slot slot) const noexcept;

    /**
     * @brief One component of one particle, as a double.
     *
     * The direct read path, for tests and for code that is not on the per-particle inner loop. A kernel reads
     * through the `FieldValue`s instead, because that is the interface it is given.
     *
     * @param i         Which particle. Out of range returns 0.0 rather than terminating, matching
     *                  `StateView::at`'s choice one module over: a caller with a layout mismatch wants a zero
     *                  that flows through, not a termination inside a step loop.
     * @param component Which component within the slot.
     * @param slot      Which array.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        The stored value, or 0.0 when either index is out of range
     * @invariant   Never reads outside the array
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.state.a_write_is_visible_through_the_view
     */
    [[nodiscard]] double at(std::size_t i, std::size_t component, Slot slot) const noexcept;

    /// @brief Writes one component of one particle. An out-of-range request changes nothing.
    ///
    /// @param i         Which particle.
    /// @param component Which component within the slot.
    /// @param slot      Which array.
    /// @param value     The value to store.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        The addressed component holds `value`, or nothing changed
    /// @invariant   Never writes outside the array
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.a_write_is_visible_through_the_view
    void set(std::size_t i, std::size_t component, Slot slot, double value) noexcept;

    /// @brief The status of one particle, or `escaped` for an index out of range.
    ///
    /// `escaped` rather than `live` for an out-of-range read, and the direction matters: a particle that does not
    /// exist is not one that is being integrated, and answering `live` would let a loop that ran past the end
    /// keep stepping a phantom that always looks healthy.
    ///
    /// @param i Which particle.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        The stored status, or `Status::escaped` when `i` is out of range
    /// @invariant   Never reads outside the array
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.status_round_trips_through_the_buffer
    [[nodiscard]] Status status_of(std::size_t i) const noexcept;

    /// @brief Sets one particle's status.
    ///
    /// @param i      Which particle.
    /// @param status The new status.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `status_of(i) == status` for an in-range index
    /// @invariant   An out-of-range index changes nothing
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.status_round_trips_through_the_buffer
    void set_status(std::size_t i, Status status) noexcept;

    /// @brief How many particles are still `live`.
    ///
    /// The number a report quotes, and the reason `Status` exists: a run that ends with 12 000 of 20 000
    /// particles left has two different explanations depending on whether the rest were absorbed or escaped, and
    /// a count that merged them could not say which.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        At most `count()`
    /// @invariant   Equals the number of entries whose status is `live`
    /// @errors      noexcept
    /// @complexity  O(n)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.status_round_trips_through_the_buffer
    [[nodiscard]] std::size_t live_count() const noexcept;

    /// @brief How many particles have one particular status.
    ///
    /// @param status The status to count.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        At most `count()`
    /// @invariant   The three counts sum to `count()`
    /// @errors      noexcept
    /// @complexity  O(n)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.status_round_trips_through_the_buffer
    [[nodiscard]] std::size_t count_with(Status status) const noexcept;

    /// @brief Whether every array is the length the count says.
    ///
    /// Checked before a step rather than assumed, for the reason `StateView::is_consistent` exists: a batch whose
    /// arrays disagree is one a kernel would read past the end of, and the read would be a plausible number
    /// rather than a crash.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        True exactly when all four arrays hold `count()` entries
    /// @invariant   True for an empty batch
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       particles.state.layout_is_declared_not_assumed
    [[nodiscard]] bool is_consistent() const noexcept;

private:
    /// @brief The address of one slot's first element, or null for an unknown slot.
    [[nodiscard]] const double* data_of(Slot slot) const noexcept;

    std::size_t count_ = 0;
    std::vector<double> position_{};
    std::vector<double> velocity_{};
    std::vector<double> charge_mass_{};
    std::vector<double> status_{};
};

}  // namespace qp::graph::particles
