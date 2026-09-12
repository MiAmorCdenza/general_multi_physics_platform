/**
 * @file trace.hpp
 * @brief The timeline: the platform's record of what happened, and when.
 *
 * ## Why the timeline is a platform mechanism and not a view's private state
 *
 * The obvious implementation is for the plot widget to keep the samples it drew.
 * It works, and it fails in a way that is invisible until it matters: a second view
 * -- a table, an export, a fit -- then needs its own copy, and the copies disagree
 * because they were sampled at different moments. Worse, a **saved run** has no
 * timeline at all, so the numbers in a lab report cannot be traced back to the run
 * that produced them.
 *
 * Charter C3 makes the timeline a platform concern for that reason. One `Trace`
 * belongs to one `RunId`; every consumer reads it; nothing else stores samples.
 *
 * ## Why a sample carries an index as well as a time
 *
 * Time in a simulation is a computed quantity: `t += dt` accumulates
 * floating-point error, and a fixed step is only approximately fixed. Two samples
 * can therefore carry the same `t` after rounding, and a consumer that identified
 * samples by time would then silently collapse them or reorder them.
 *
 * So a sample has both. `index` is exact and total: it identifies the sample
 * unambiguously and gives a cheap, allocation-free position for a cursor. `t` is
 * what a human reads, what a fit uses as the independent variable, and what an
 * export writes. Where they disagree, `index` is the identity and `t` is the data.
 *
 * ## Why append-only, and why monotonicity is checked
 *
 * A trace is evidence. Allowing an edit -- or a delete -- would make "the run
 * produced this" unverifiable, and the cheapest way to get a wrong answer out of a
 * lab session is to remove the point that disagreed with the hypothesis.
 *
 * Time is therefore required to be **non-decreasing on append**. That check is
 * cheap and it catches the mistake that actually happens: a consumer appending an
 * out-of-order result because it evaluated something late. Accepting it would make
 * every downstream operation -- interpolation, differencing, a plot's line --
 * produce a wrong answer with no indication of why.
 *
 * Equal times are permitted and expected, because that is what a multi-variable
 * sample at one instant looks like, and what accumulated round-off produces.
 *
 * ## What is deliberately absent
 *
 * No resampling, no smoothing, no interpolation, no statistics beyond the extent.
 * Those are analyses with several defensible answers, they belong to plugins, and
 * `runtime/store` already owns the one statistic that is definitional (the Type-A
 * uncertainty of a mean). A trace module that shipped an interpolation would be
 * deciding, for every course, what happens between two measurements.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Sample times are non-decreasing and indices are consecutive from 0
 * @errors      noexcept
 * @frozen      no
 * @tests       trace.sample.index_and_time_are_separate,
 *              trace.trace.append_is_ordered, trace.trace.rejects_time_travel,
 *              trace.trace.cursor_navigation, trace.trace.a_copy_is_a_snapshot
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/runtime/store/store.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qp::runtime {

/**
 * @brief One instant in a run: when it was, and what was read there.
 *
 * @ownership   owns (the readings)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `index` equals this sample's position in its trace
 * @errors      noexcept
 * @frozen      no
 * @tests       trace.sample.index_and_time_are_separate
 */
struct Sample final {
    /// Exact position in the trace. The identity; see the file comment.
    std::uint64_t index = 0;
    /// Simulation time of this sample, in seconds. The data, not the identity.
    double t = 0.0;
    /// One value per traced quantity, in the trace's channel order.
    std::vector<UncertainValue> values{};
};

/**
 * @brief A named quantity being traced.
 *
 * The name is what an export writes as a column header and what a plot labels an
 * axis. The dimension is carried so that a unit is never lost between the model
 * and the report -- a column of numbers without its unit is the commonest way a
 * lab report becomes wrong.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `name` is non-empty for a channel that was added
 * @errors      noexcept
 * @frozen      no
 * @tests       trace.trace.channels_are_stable
 */
struct Channel final {
    std::string name{};
    /// The dimension of this quantity, one per value.
    units::Dim dim{};

    [[nodiscard]] friend bool operator==(const Channel& a, const Channel& b) {
        return a.name == b.name && a.dim == b.dim;
    }
};

/**
 * @brief The record of one run: its channels, and the samples taken from them.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every sample's `values` has the same size as `channels`
 * @errors      reports refusal through diag::Result
 * @frozen      no
 * @tests       trace.trace.append_is_ordered
 */
class Trace final {
public:
    /// @brief An empty trace for `run`. A trace always belongs to a run.
    explicit Trace(RunId run) noexcept : run_(run) {}

    /// @brief The run this trace records.
    [[nodiscard]] RunId run() const noexcept { return run_; }

    /**
     * @brief Declares a channel. Order is the column order.
     *
     * Refused after the first sample, because a channel added later would leave
     * every earlier sample one value short, and padding them with zeroes would
     * invent measurements nobody took.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success the channel exists at the returned position
     * @invariant   Channel names are unique within a trace
     * @errors      noexcept; returns invalid_argument for an empty or duplicate
     *              name, and duplicate_connection when samples already exist
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       trace.trace.channels_are_stable
     */
    [[nodiscard]] diag::Result<std::size_t> add_channel(Channel channel) noexcept;

    /// @brief The declared channels, in declaration order.
    [[nodiscard]] const std::vector<Channel>& channels() const noexcept { return channels_; }

    /// @brief How many values each sample must carry.
    [[nodiscard]] std::size_t channel_count() const noexcept { return channels_.size(); }

    /**
     * @brief Appends one sample.
     *
     * @ownership   owns (copies the values)
     * @thread      main
     * @pre         `sample.values.size() == channel_count()`
     * @post        On success `samples().back().index == size() - 1`
     * @invariant   Times stay non-decreasing; indices stay consecutive from 0
     * @errors      noexcept; returns invalid_argument when the value count is
     *              wrong or `t` is not finite, and out_of_range when `t` is less
     *              than the previous sample's (time travel is refused, not sorted
     *              into place). Allocation failure terminates
     * @complexity  O(values)
     * @nondet      none
     * @frozen      no
     * @tests       trace.trace.append_is_ordered, trace.trace.rejects_time_travel
     */
    [[nodiscard]] diag::Result<void> append(double t, std::vector<UncertainValue> values) noexcept;

    /// @brief Convenience for a single-channel trace.
    [[nodiscard]] diag::Result<void> append(double t, UncertainValue value) noexcept;

    /// @brief Every sample, in append order.
    [[nodiscard]] const std::vector<Sample>& samples() const noexcept { return samples_; }

    /// @brief Number of samples.
    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

    /// @brief The earliest sample time, or nothing when empty.
    [[nodiscard]] std::optional<double> t_begin() const noexcept;

    /// @brief The latest sample time, or nothing when empty.
    [[nodiscard]] std::optional<double> t_end() const noexcept;

    /// @brief The time span covered, or nothing when fewer than two samples.
    [[nodiscard]] std::optional<double> duration() const noexcept;

    /**
     * @brief One value of one sample, or nothing when out of range.
     *
     * Nothing rather than a default: a caller that asked for a channel the trace
     * does not have has a bug, and returning 0.0 would let it continue with a
     * plausible wrong number -- the failure this whole module is arranged against.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Nothing when `index` or `channel` is out of range
     * @invariant   Never reads outside the sample
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       trace.trace.cursor_navigation
     */
    [[nodiscard]] std::optional<UncertainValue> value_at(std::uint64_t index,
                                                         std::size_t channel) const noexcept;

    /// @brief Whether every sample carries every channel's value.
    [[nodiscard]] bool is_consistent() const noexcept;

private:
    RunId run_{};
    std::vector<Channel> channels_{};
    std::vector<Sample> samples_{};
};

/**
 * @brief A position in a trace, for stepping through it without losing the place.
 *
 * Held by a view that scrubs a timeline. It stores an **index**, not a time: a
 * cursor that stored a float time would drift across a scrub and could land
 * between two samples, which has no defined meaning for a record of discrete
 * measurements.
 *
 * @ownership   observes
 * @thread      main
 * @pre         the trace outlives the cursor
 * @post        none
 * @invariant   An exhausted cursor does not move further
 * @errors      noexcept
 * @frozen      no
 * @tests       trace.trace.cursor_navigation
 */
class Cursor final {
public:
    /// @brief Starts at the first sample.
    explicit Cursor(const Trace& trace) noexcept : trace_(&trace) {}

    /// @brief The sample the cursor is on, or null when the trace is empty.
    [[nodiscard]] const Sample* current() const noexcept;

    /// @brief Whether a current sample exists.
    [[nodiscard]] bool valid() const noexcept { return current() != nullptr; }

    /// @brief Moves forward one sample. Returns whether it moved.
    bool next() noexcept;

    /// @brief Moves back one sample. Returns whether it moved.
    bool previous() noexcept;

    /// @brief Jumps to `index`. Returns whether that sample exists.
    bool seek(std::uint64_t index) noexcept;

    /// @brief The current index. Meaningless when invalid.
    [[nodiscard]] std::uint64_t index() const noexcept { return index_; }

    /// @brief Whether a later sample exists.
    [[nodiscard]] bool has_next() const noexcept;

    /// @brief Whether an earlier sample exists.
    [[nodiscard]] bool has_previous() const noexcept { return index_ > 0 && valid(); }

private:
    const Trace* trace_;
    std::uint64_t index_ = 0;
};

}  // namespace qp::runtime
