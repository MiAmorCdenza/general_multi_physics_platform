/**
 * @file test_value_text.cpp
 * @brief Every kind out and back, and the three refusals.
 *
 * Test case ids match the @tests fields in the plugin header byte for byte.
 *
 * ## Why the round trip is asserted bit-for-bit
 *
 * `to_text` and `from_text` are a conversion between a value and a file, and a conversion that is off by one ulp
 * is a save that changed the user's data. So the cases compare **bits**, not approximate equality: the float case
 * in particular is the one a widening implementation passes and this one must not, because
 * `static_cast<float>(double_round_trip(x)) != x` for some `x`. The assertion is
 * `ports::Value`'s own contract -- `static_cast<float>(to_double()) == as_f32()` -- applied across a save.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/value_text/value_text.hpp>

#include <qp/units/dim.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace qp::plugins::value_text;

namespace {

namespace ports = qp::ports;
using qp::authoring::DocumentRefusal;

/// @brief Converts `value` out and back, and returns whether the result is the same kind and payload.
[[nodiscard]] bool round_trips(const ports::Value& value) {
    Tagged tagged;
    if (to_text(value, tagged) != DocumentRefusal::ok) return false;
    ports::Value back;
    if (from_text(tagged, back) != DocumentRefusal::ok) return false;
    return back == value;
}

}  // namespace

TEST_CASE("value_text.round_trips_every_kind", "[value_text]") {
    // One case per kind, and each one is a **different** claim. A single "a value survives" case over a double
    // would pass for an implementation that mishandled the other five kinds entirely.
    const std::vector<ports::Value> values{
        ports::Value{12.5},
        ports::Value{-0.0},
        ports::Value{1.0e-300},
        ports::Value{1.7976931348623157e308},   // the largest finite double
        ports::Value{2.2250738585072014e-308},  // the smallest normal one
        ports::Value{1.25f},
        ports::Value{-3.5e-8f},
        ports::Value{std::int64_t{0}},
        ports::Value{std::int64_t{-9007199254740993LL}},  // beyond a double's exact range
        ports::Value{std::numeric_limits<std::int64_t>::min()},
        ports::Value{true},
        ports::Value{false},
        ports::Value{qp::units::dims::length},
        ports::Value{qp::units::dims::velocity},
        ports::Value{qp::units::Dim{}},
        ports::Value{qp::units::Dim{-1, 2, -3, 4, -5, 6, -7}},
        ports::Value{std::string{}},
        ports::Value{std::string{"length"}},
        ports::Value{std::string{"a value with spaces, colons: and \"quotes\""}},
        ports::Value{std::string{"\xE5\x8D\x95\xE6\x91\x86"}},  // CJK, as a lab's node name might be
    };

    for (const ports::Value& value : values) {
        Tagged tagged;
        const DocumentRefusal code = to_text(value, tagged);
        INFO("kind " << value.kind_name() << " payload " << tagged.payload);
        REQUIRE(code == DocumentRefusal::ok);
        REQUIRE_FALSE(tagged.kind.empty());

        ports::Value back;
        REQUIRE(from_text(tagged, back) == DocumentRefusal::ok);
        REQUIRE(back.kind() == value.kind());
        // Bit equality, not approximate equality: see the file comment.
        REQUIRE(back == value);
    }

    // The float case is called out on its own because it is the one a widening implementation passes and this one
    // must not: a value whose double is not the float it came from.
    {
        const float exact = 0.1f;  // not representable in binary; its double is a different number
        const ports::Value value{exact};
        Tagged tagged;
        REQUIRE(to_text(value, tagged) == DocumentRefusal::ok);
        REQUIRE(tagged.kind == "f32");
        ports::Value back;
        REQUIRE(from_text(tagged, back) == DocumentRefusal::ok);
        REQUIRE(back.kind() == ports::ValueKind::f32);
        // The invariant `ports::Value` states about itself, held across a save.
        REQUIRE(static_cast<float>(back.to_double()) == exact);
        REQUIRE(back.as_f32() == exact);
    }

    // And the helper agrees, so the loop above is checking what it says it is.
    for (const ports::Value& value : values) REQUIRE(round_trips(value));
}

TEST_CASE("value_text.refuses_what_a_file_cannot_hold", "[value_text]") {
    // The three refusals, each with its own code, because they are three different things to tell a user. This is
    // the same set `qpjson` documents, and the reason it lives here rather than there is that a second format
    // must inherit it rather than re-derive it.
    Tagged tagged;

    // (1) A field handle names a buffer in the running process. It is not in the document, so it cannot be in a
    // file, and a handle to nothing would load as a node whose input silently changed.
    qp::abi::LatticeDesc lattice{};
    const ports::Value handle{lattice};
    REQUIRE(handle.kind() == ports::ValueKind::field_handle);
    REQUIRE(to_text(handle, tagged) == DocumentRefusal::value_kind_not_supported);

    // (2) A string that is not UTF-8 would be silently rewritten by any escaping scheme -- for a log line that is
    // the right answer and for a user's own data it is an edit they did not make.
    const ports::Value bad_text{std::string{"\xFF\xFE not utf8"}};
    REQUIRE(to_text(bad_text, tagged) == DocumentRefusal::text_not_utf8);
    // The same refusal on the way in: a file whose bytes are not valid UTF-8 is refused rather than repaired.
    ports::Value scratch;
    REQUIRE(from_text(Tagged{"text", "\xFF\xFE"}, scratch) == DocumentRefusal::text_not_utf8);

    // (3) An infinity or a NaN has no literal in a text format, and writing one as a null would load as a value
    // nobody computed.
    REQUIRE(to_text(ports::Value{std::numeric_limits<double>::infinity()}, tagged) ==
            DocumentRefusal::non_finite_number);
    REQUIRE(to_text(ports::Value{std::nan("")}, tagged) == DocumentRefusal::non_finite_number);
    REQUIRE(to_text(ports::Value{std::numeric_limits<float>::infinity()}, tagged) ==
            DocumentRefusal::non_finite_number);

    // An unset parameter is refused too: writing it would produce a file claiming a value, and the caller should
    // omit the parameter instead.
    REQUIRE(to_text(ports::Value{}, tagged) == DocumentRefusal::value_kind_not_supported);
}

TEST_CASE("value_text.refuses_a_payload_that_is_not_its_kind", "[value_text]") {
    // A file is text a person can edit, so a payload can be anything at all. Each of these is `malformed` rather
    // than a coerced value, and the distinction matters: `"12abc"` read as `12` would be a silent edit, and
    // `"yes"` read as `true` would be one more spelling rule to keep in step with the writer.
    ports::Value out;
    REQUIRE(from_text(Tagged{"f64", "12abc"}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"f64", ""}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"f32", "1.0.0"}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"i64", "12.5"}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"i64", "9999999999999999999999"}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"bool", "yes"}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"bool", "1"}, out) == DocumentRefusal::malformed);
    // Six axes is a file that lost one and eight is a file from a build with a different dimension model; reading
    // either as if it were seven would be a guess.
    REQUIRE(from_text(Tagged{"dim", "1,0,0,0,0,0"}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"dim", "1,0,0,0,0,0,0,0"}, out) == DocumentRefusal::malformed);
    REQUIRE(from_text(Tagged{"dim", "1,0,0,0,0,0,x"}, out) == DocumentRefusal::malformed);
    // An axis outside the `int8_t` range would wrap silently, turning a typo into a plausible dimension.
    REQUIRE(from_text(Tagged{"dim", "1000,0,0,0,0,0,0"}, out) == DocumentRefusal::malformed);
    // An unknown kind is a file written by a build that disagrees about what a value is.
    REQUIRE(from_text(Tagged{"complex", "1+2i"}, out) == DocumentRefusal::value_kind_not_supported);
    REQUIRE(from_text(Tagged{"", "12"}, out) == DocumentRefusal::value_kind_not_supported);

    // A non-finite number spelled out is refused on the way in as well as out, and the code is
    // `non_finite_number` rather than `malformed` because `from_chars` **does** accept `inf` and `nan` -- so the
    // payload really does parse, as an infinity, and the refusal is about what it parsed to. That is worth being
    // precise about: the two codes mean different things to a reader ("the text is not a number" against "the
    // number is not one a file may hold") and this case is the second.
    REQUIRE(from_text(Tagged{"f64", "inf"}, out) == DocumentRefusal::non_finite_number);
    REQUIRE(from_text(Tagged{"f64", "nan"}, out) == DocumentRefusal::non_finite_number);
    REQUIRE(from_text(Tagged{"f32", "inf"}, out) == DocumentRefusal::non_finite_number);

    // And a kind with a valid payload still works, so the refusals above are about the payloads.
    REQUIRE(from_text(Tagged{"i64", "-7"}, out) == DocumentRefusal::ok);
    REQUIRE(out.as_i64() == -7);
}
