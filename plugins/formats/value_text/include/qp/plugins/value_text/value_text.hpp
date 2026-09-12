/**
 * @file value_text.hpp
 * @brief A port value as **text plus its kind**, so a document format has one thing to write.
 *
 * ## The problem this solves, and the one it does not
 *
 * A node parameter is a `ports::Value`: a tagged union of a double, a float, an integer, a boolean, a string, a
 * dimension and a field handle. Every document format has to write one, and until now exactly one format existed
 * and the conversion lived inside it. The moment a second format appears, that conversion would be written twice
 * -- and the two would drift, with the symptom that one format reads a parameter the other wrote as a different
 * thing. That is the same failure the platform's whole module split exists to prevent, one layer down.
 *
 * So the conversion moves here, and it is deliberately **not** a JSON or YAML writer: it produces a kind tag and
 * a text payload, and the format decides which quoting, indentation or escaping carries those two strings. The
 * split is the point -- `value_text` knows what a parameter *is*, and a format knows how a file spells it.
 *
 * ## What "text" means here
 *
 * The payload is the value in its **shortest exact form**, and for each kind that is a different thing:
 *
 * | kind | payload |
 * |---|---|
 * | `f64` | shortest decimal that reads back to the same bits |
 * | `f32` | shortest decimal that reads back to the same **float** |
 * | `i64` | decimal, signed |
 * | `bool` | `true` or `false` |
 * | `dim` | the seven axes, comma-separated: `L,M,T,I,Th,N,J` |
 * | `text` | the string itself, raw and unescaped |
 * | `field` | refused |
 *
 * `f32` is its own row rather than being widened, and that is a defect this file exists to prevent: a float
 * written through `double` and read back is the same *number* and not the same *value*, so
 * `static_cast<float>(round_trip(x)) != x` for some `x` -- which is exactly the invariant
 * `ports::Value`'s own contract states and a widening round trip quietly breaks.
 *
 * ## The three refusals, unchanged and for the same reasons
 *
 * A `field_handle` names a buffer in the running process; it is not in the document, so it cannot be in a file,
 * and a handle to nothing would load as a node whose input silently changed. A string that is not UTF-8 would be
 * silently rewritten by any escaping scheme. An infinity or a NaN has no literal in either text format, and
 * writing one as a null would load as a value nobody computed. All three are `DocumentRefusal` codes rather than
 * booleans, so a caller can say which of them happened.
 *
 * @ownership   pure (every function is a conversion)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A value that `to_text` accepts survives `from_text` with its kind and its payload unchanged
 * @errors      Reports through `DocumentRefusal` rather than throwing
 * @frozen      no
 * @tests       value_text.round_trips_every_kind
 */
#pragma once

#include <qp/authoring/persist/persist.hpp>
#include <qp/ports/value.hpp>

#include <string>
#include <string_view>

namespace qp::plugins::value_text {

/**
 * @brief A port value as a kind tag and a text payload: what a format writes as two scalars.
 *
 * The kind is a string rather than the `ValueKind` enumerator, because it goes into a file and a file has to be
 * readable years from now: an enumerator's number is a compile-time accident, while `"f64"` is a name the format
 * and the reader agree on. The same argument `qpjson` makes for writing `"f64"` as its own text.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `kind` is one of the names `to_text` produces
 * @errors      noexcept
 * @frozen      no
 * @tests       value_text.round_trips_every_kind
 */
struct Tagged final {
    /// The kind's file name: `f64`, `f32`, `i64`, `bool`, `dim` or `text`.
    std::string kind{};
    /// The value in its shortest exact form. Never escaped: escaping belongs to the format.
    std::string payload{};
};

/**
 * @brief Converts a port value to its kind and text.
 *
 * @param value The value to convert.
 * @param out   Filled on success, untouched on failure.
 *
 * @ownership   owns the converted pair
 * @thread      any
 * @pre         none
 * @post        On `ok`, `from_text(out)` returns a value equal to `value`
 * @invariant   The payload contains no quoting or escaping: a format adds those
 * @errors      `value_kind_not_supported` for a `field_handle` or an `invalid` value, `text_not_utf8` for a
 *              string that is not legal UTF-8, `non_finite_number` for an infinity or a NaN
 * @complexity  O(text)
 * @nondet      none
 * @frozen      no
 * @tests       value_text.round_trips_every_kind,
 *              value_text.refuses_what_a_file_cannot_hold
 */
[[nodiscard]] qp::authoring::DocumentRefusal to_text(const qp::ports::Value& value, Tagged& out);

/**
 * @brief Converts a kind and its text back to a port value.
 *
 * @param tagged The pair, as read from a file.
 * @param out    Filled on success, untouched on failure.
 *
 * @ownership   owns the converted value
 * @thread      any
 * @pre         none
 * @post        On `ok`, `to_text(out)` produces the same kind and payload
 * @invariant   An unknown kind name is refused rather than guessed at
 * @errors      `value_kind_not_supported` for a kind this build does not know, `malformed` for a payload that
 *              does not parse as its own kind, `text_not_utf8` for a `text` payload that is not legal UTF-8,
 *              `non_finite_number` for `inf` or `nan` spelled as a payload
 * @complexity  O(text)
 * @nondet      none
 * @frozen      no
 * @tests       value_text.round_trips_every_kind,
 *              value_text.refuses_a_payload_that_is_not_its_kind
 */
[[nodiscard]] qp::authoring::DocumentRefusal from_text(const Tagged& tagged, qp::ports::Value& out);

}  // namespace qp::plugins::value_text
