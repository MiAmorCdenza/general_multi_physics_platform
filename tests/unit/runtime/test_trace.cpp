/**
 * @file test_trace.cpp
 * @brief Tests for the timeline.
 *
 * Test case ids match the @tests fields in the trace headers byte for byte.
 *
 * Two claims carry the weight here, and each is a place where a plausible
 * implementation is quietly wrong:
 *
 *   - **index and time are separate.** Time in a simulation is computed, so two
 *     samples can share a `t` after round-off; a consumer that identified samples
 *     by time would collapse or reorder them, and the values would end up attached
 *     to the wrong instants.
 *   - **an out-of-order append is refused, not sorted into place.** Inserting it
 *     would renumber every later sample, so a cursor or a saved column index taken
 *     before the insert would point at a different measurement, and nothing in the
 *     record would show that it had happened.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/runtime/trace.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace qp::runtime;

namespace {

/// @brief A dimensionless reading, for fixtures.
UncertainValue reading(double v, double u = 0.0) {
    return UncertainValue::measured(v, u);
}

/// @brief A trace with one channel, ready to append to.
Trace one_channel(RunId run = RunId{1}) {
    Trace trace{run};
    const auto added = trace.add_channel(Channel{"position", qp::units::Dim{1, 0, 0, 0, 0, 0, 0}});
    REQUIRE(added.has_value());
    REQUIRE(added.value() == 0);
    return trace;
}

}  // namespace

// ===========================================================================
// Sample identity
// ===========================================================================

TEST_CASE("trace.sample.index_and_time_are_separate", "[trace]") {
    Trace trace = one_channel();

    // A zero step, appended twice. Both samples carry t == 0.0, which is what
    // accumulated round-off and a multi-channel read at one instant both produce.
    REQUIRE(trace.append(0.0, reading(1.0)).has_value());
    REQUIRE(trace.append(0.0, reading(2.0)).has_value());

    REQUIRE(trace.size() == 2);
    REQUIRE(trace.samples()[0].t == 0.0);
    REQUIRE(trace.samples()[1].t == 0.0);

    // The indices still tell them apart, and that is the point: identifying samples
    // by time would collapse these two and silently lose a measurement.
    REQUIRE(trace.samples()[0].index == 0);
    REQUIRE(trace.samples()[1].index == 1);
    REQUIRE(trace.value_at(0, 0)->value == 1.0);
    REQUIRE(trace.value_at(1, 0)->value == 2.0);
    REQUIRE(trace.is_consistent());

    // The extent reports both zero, and the duration is zero rather than absent:
    // two samples were taken, they just happened at the same time.
    REQUIRE(*trace.t_begin() == 0.0);
    REQUIRE(*trace.t_end() == 0.0);
    REQUIRE(*trace.duration() == 0.0);
}

// ===========================================================================
// Channels
// ===========================================================================

TEST_CASE("trace.trace.channels_are_stable", "[trace]") {
    Trace trace{RunId{1}};
    REQUIRE(trace.channel_count() == 0);
    REQUIRE(trace.empty());

    // An empty trace has no extent at all, rather than 0.0. A duration of zero for
    // a trace nobody sampled is a number a report would print.
    REQUIRE_FALSE(trace.t_begin().has_value());
    REQUIRE_FALSE(trace.t_end().has_value());
    REQUIRE_FALSE(trace.duration().has_value());

    const auto first = trace.add_channel(Channel{"t", qp::units::Dim{0, 0, 1, 0, 0, 0, 0}});
    const auto second = trace.add_channel(Channel{"x", qp::units::Dim{1, 0, 0, 0, 0, 0, 0}});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first.value() == 0);
    REQUIRE(second.value() == 1);
    REQUIRE(trace.channel_count() == 2);

    // Declaration order is the column order, and it is what an export writes.
    // Reordering would silently swap two columns' meaning in a saved file.
    REQUIRE(trace.channels()[0].name == "t");
    REQUIRE(trace.channels()[1].name == "x");

    SECTION("an empty or duplicate name is refused") {
        REQUIRE_FALSE(trace.add_channel(Channel{"", {}}).has_value());
        // A duplicate name would give an export two columns with one header, and a
        // reader could not tell which one a fit used.
        REQUIRE_FALSE(trace.add_channel(Channel{"t", {}}).has_value());
        REQUIRE(trace.channel_count() == 2);
    }

    SECTION("a channel declared after the first sample is refused") {
        // It would leave every earlier sample one value short, and padding with
        // zeroes would invent a measurement nobody took.
        REQUIRE(trace.append(0.0, {reading(0.0), reading(0.0)}).has_value());
        const auto late = trace.add_channel(Channel{"v", {}});
        REQUIRE_FALSE(late.has_value());
        REQUIRE(late.error() == qp::diag::ErrorCode::duplicate_connection);
        REQUIRE(trace.channel_count() == 2);
    }
}

// ===========================================================================
// Appending
// ===========================================================================

TEST_CASE("trace.trace.append_is_ordered", "[trace]") {
    Trace trace = one_channel();

    REQUIRE(trace.append(0.0, reading(1.0)).has_value());
    REQUIRE(trace.append(0.5, reading(2.0)).has_value());
    REQUIRE(trace.append(1.0, reading(3.0)).has_value());
    REQUIRE(trace.size() == 3);

    // Indices are consecutive from zero, so a cursor can index directly and a
    // saved column position refers to one sample.
    for (std::size_t i = 0; i < trace.size(); ++i) {
        REQUIRE(trace.samples()[i].index == i);
    }
    REQUIRE(trace.is_consistent());

    REQUIRE(*trace.t_begin() == 0.0);
    REQUIRE(*trace.t_end() == 1.0);
    REQUIRE(*trace.duration() == 1.0);

    SECTION("a wrong value count is refused") {
        // Two values for a one-channel trace: accepting it would leave a sample
        // whose extra value belongs to no channel, and every later read by index
        // would be off by one.
        const auto refused = trace.append(2.0, {reading(1.0), reading(2.0)});
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == qp::diag::ErrorCode::invalid_argument);
        REQUIRE(trace.size() == 3);
    }

    SECTION("a non-finite time is refused") {
        // NaN is neither less than, equal to, nor greater than the previous time,
        // so a trace that accepted it would have a sample whose position depends on
        // which comparison the reader happens to write first.
        REQUIRE_FALSE(trace.append(std::nan(""), reading(1.0)).has_value());
        REQUIRE_FALSE(
            trace.append(std::numeric_limits<double>::infinity(), reading(1.0)).has_value());
        REQUIRE(trace.size() == 3);
        REQUIRE(trace.is_consistent());
    }

    SECTION("the single-value overload requires exactly one channel") {
        Trace two{RunId{2}};
        REQUIRE(two.add_channel(Channel{"a", {}}).has_value());
        REQUIRE(two.add_channel(Channel{"b", {}}).has_value());
        // Ambiguous: which channel would the value belong to? Refused rather than
        // guessed, and the multi-value overload is the one that works here.
        REQUIRE_FALSE(two.append(0.0, reading(1.0)).has_value());
        REQUIRE(two.append(0.0, {reading(1.0), reading(2.0)}).has_value());
        REQUIRE(two.size() == 1);
    }
}

TEST_CASE("trace.trace.rejects_time_travel", "[trace]") {
    Trace trace = one_channel();
    REQUIRE(trace.append(0.0, reading(1.0)).has_value());
    REQUIRE(trace.append(1.0, reading(2.0)).has_value());
    REQUIRE(trace.append(2.0, reading(3.0)).has_value());

    // An earlier time is refused, not sorted into place. Inserting it would
    // renumber every later sample, so an index taken before the insert -- a cursor,
    // a cache key, a column position in a saved file -- would point at a different
    // measurement, and nothing in the record would show it.
    const auto refused = trace.append(0.5, reading(99.0));
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == qp::diag::ErrorCode::out_of_range);

    // And the trace is untouched: the refused sample left no trace of itself.
    REQUIRE(trace.size() == 3);
    REQUIRE(trace.value_at(1, 0)->value == 2.0);
    REQUIRE(trace.is_consistent());

    // Equal to the last time is accepted: that is several channels read at one
    // instant, and it is what round-off produces.
    REQUIRE(trace.append(2.0, reading(4.0)).has_value());
    REQUIRE(trace.size() == 4);
    REQUIRE(trace.is_consistent());

    // The clock is allowed to be coarse; it is not allowed to go backwards.
    REQUIRE_FALSE(trace.append(1.999999, reading(5.0)).has_value());
    REQUIRE(trace.size() == 4);
}

// ===========================================================================
// Reading and walking
// ===========================================================================

TEST_CASE("trace.trace.cursor_navigation", "[trace]") {
    Trace trace = one_channel();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(trace.append(static_cast<double>(i), reading(static_cast<double>(i) * 10.0))
                    .has_value());
    }

    Cursor cursor{trace};
    REQUIRE(cursor.valid());
    REQUIRE(cursor.index() == 0);
    REQUIRE(cursor.current()->values[0].value == 0.0);
    REQUIRE(cursor.has_next());
    REQUIRE_FALSE(cursor.has_previous());

    REQUIRE(cursor.next());
    REQUIRE(cursor.index() == 1);
    REQUIRE(cursor.has_previous());
    REQUIRE(cursor.previous());
    REQUIRE(cursor.index() == 0);
    // At the first sample there is nowhere earlier to go, and the cursor stays put
    // rather than wrapping to the end -- a scrub that wrapped would look like data.
    REQUIRE_FALSE(cursor.previous());
    REQUIRE(cursor.index() == 0);

    REQUIRE(cursor.seek(3));
    REQUIRE(cursor.index() == 3);
    REQUIRE_FALSE(cursor.has_next());
    REQUIRE_FALSE(cursor.next());
    REQUIRE(cursor.index() == 3);

    // An out-of-range seek is refused and does not move the cursor.
    REQUIRE_FALSE(cursor.seek(4));
    REQUIRE_FALSE(cursor.seek(999));
    REQUIRE(cursor.index() == 3);

    SECTION("an empty trace has no valid cursor") {
        Trace empty{RunId{7}};
        REQUIRE(empty.add_channel(Channel{"a", {}}).has_value());
        Cursor none{empty};
        REQUIRE_FALSE(none.valid());
        REQUIRE(none.current() == nullptr);
        REQUIRE_FALSE(none.next());
        REQUIRE_FALSE(none.has_next());
    }

    SECTION("reading out of range has no answer") {
        // Nothing rather than a default: a caller that guessed a channel or an
        // index has a bug, and returning 0.0 would let it continue with a
        // plausible wrong number.
        REQUIRE_FALSE(trace.value_at(99, 0).has_value());
        REQUIRE_FALSE(trace.value_at(0, 5).has_value());
        REQUIRE(trace.value_at(0, 0).has_value());
    }

    SECTION("the trace remembers which run it belongs to") {
        REQUIRE(trace.run() == RunId{1});
        Trace other{RunId{42}};
        REQUIRE(other.run() == RunId{42});
        REQUIRE(other.run() != trace.run());
    }
}
