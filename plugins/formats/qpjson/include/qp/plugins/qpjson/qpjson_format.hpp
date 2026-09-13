/**
 * @file qpjson_format.hpp
 * @brief The first document format: JSON, written to be read by a person.
 *
 * ## Why JSON first
 *
 * A saved document is the one artifact a student keeps, a teacher collects, and a supervisor asks to
 * see. That makes two properties matter more than size: a human can open it, and a version control
 * system can diff it. Both point at text, and among text formats JSON is the one this project already
 * writes -- `qp::diag` emits JSON Lines logs through `qp::diag::json_escape` -- so a second escaping
 * rule, a second set of quoting edge cases, and a second parser are things this format does not have to
 * invent.
 *
 * ## What it refuses, and why refusing is the feature
 *
 * A format that silently drops what it cannot represent turns a save into data loss that nobody
 * notices until the file is reopened. So this one refuses, with a code that says which of three things
 * went wrong:
 *
 *   - `value_kind_not_supported` -- a node parameter holding a **field handle**. That names a buffer
 *     that lives in the running process; it is not in the document, so it cannot be in the file, and a
 *     handle to nothing would load as a node whose input silently changed.
 *   - `text_not_utf8` -- a string (a title, a name, a text parameter, a view's layout payload) that is
 *     not legal UTF-8. Escaping replaces an invalid byte with `?`; for a log line that is the right
 *     answer, and for a user's own data it is a silent edit.
 *   - `non_finite_number` -- an infinity or a NaN in a numeric parameter. JSON has no literal for
 *     either, and writing `null` would load as a value nobody computed.
 *
 * ## Why the file has a marker and a version
 *
 * The first member is `"qp_document": 1`. Without it, "this is not a document" and "this is a document
 * with a syntax error" are the same answer, and the user gets one message for two problems -- one of
 * which has an obvious fix ("you opened the wrong file") and one of which does not.
 *
 * ## Why unknown members are refused rather than skipped
 *
 * Skipping is the usual forgiving choice and it is wrong here: the version number *is* the
 * compatibility mechanism. A file at version 1 holding a member version 1 does not define means the
 * file and this build disagree about what a document is, and loading the part they agree on would
 * produce a graph that is not the one that was saved. That is worse than refusing, because it looks
 * like it worked.
 *
 * ## Why the round trip is byte-stable
 *
 * Nodes are written in slot order, edges in insertion order, parameters in the order the node holds
 * them, and layouts in the order the document holds them. A save of the same document therefore
 * produces the same bytes, which is what lets a test compare bytes rather than parse them, and what
 * makes "this file changed" a meaningful statement in a diff.
 *
 * @ownership   observes (the format owns no document)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `format()` describes the bytes this class writes
 * @errors      Reports through `DocumentRefusal` rather than throwing
 * @frozen      no
 * @tests       persist.qpjson.describes_itself
 */
#pragma once

#include <qp/authoring/persist.hpp>

#include <cstdint>
#include <string>

namespace qp::plugins::qpjson {

/**
 * @brief Writes and reads a document as JSON text.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `to_bytes` and `from_bytes` are inverse on every document this format can write
 * @errors      noexcept; every failure is a `DocumentRefusal`
 * @frozen      no
 * @tests       persist.qpjson.round_trip_preserves_everything_that_matters
 */
class QpJsonFormat final : public qp::authoring::IDocumentFormat {
public:
    /// @brief The version this build writes and the highest it reads.
    // **2 since a document carries the session's measurements.** The marker's value is the version, and the reader
    // accepts anything up to and including this one -- so a file written by the previous build still loads (it simply
    // has no readings member) and a file written by *this* build is refused by that one with `unsupported_version`
    // rather than having its measurements silently dropped. That is the compatibility mechanism this format
    // documents: a member a version does not define is a disagreement about what a document is, and refusing is the
    // answer.
    static constexpr int kVersion = 2;

    /// @brief The member that marks a file as a document. Its value is the version.
    static constexpr const char* kMarkerKey = "qp_document";

    /// @brief Deepest nesting accepted while parsing.
    ///
    /// The writer's deepest is five (document, graph, node, parameter). A limit exists because a parser
    /// that recurses on input it did not produce is a parser that a hostile file can turn into a stack
    /// overflow, and `from_bytes` is handed whatever the user picked.
    static constexpr int kMaxDepth = 16;

    /**
     * @brief The format this class implements.
     *
     * @ownership   borrows (returns a reference to a function-local static, valid for the process)
     * @thread      main
     * @pre         none
     * @post        `name` is non-empty and `extensions` holds at least one entry
     * @invariant   The same object every call, so a registry can hold the reference
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       persist.qpjson.describes_itself
     */
    [[nodiscard]] const qp::authoring::DocumentFormatDesc& format() const noexcept override;

    /**
     * @brief Writes `source` as JSON text into `out`.
     *
     * @param source The document to write, borrowed: this format never copies a graph.
     * @param out    Cleared at entry; holds the whole document on success.
     *
     * @ownership   observes `source`, owns `out`'s content
     * @thread      main
     * @pre         `source.is_complete()`
     * @post        On `ok`, `out` is a complete document and re-reading it yields an equal graph;
     *              otherwise `out` is empty
     * @invariant   The same source produces the same bytes
     * @errors      noexcept; `value_kind_not_supported`, `text_not_utf8`, `non_finite_number`
     * @complexity  O(nodes + edges + layouts)
     * @nondet      none
     * @frozen      no
     * @tests       persist.qpjson.round_trip_preserves_everything_that_matters,
     *              persist.qpjson.refuses_what_json_cannot_carry,
     *              persist.qpjson.a_save_is_byte_stable
     */
    [[nodiscard]] qp::authoring::DocumentRefusal to_bytes(
        const qp::authoring::DocumentSource& source, std::string& out) const noexcept override;

    /**
     * @brief Parses JSON text into `out`.
     *
     * @param bytes Anything at all, including a file that is not a document and a file that was cut
     *              short mid-download.
     * @param out   Unchanged on failure.
     *
     * @ownership   observes `bytes`, owns `out`'s content
     * @thread      main
     * @pre         none
     * @post        On `ok`, `out` holds the parsed document; otherwise `out` is unchanged
     * @invariant   Returns a code for **any** input; never crashes, never loops forever, never recurses
     *              past `kMaxDepth`
     * @errors      noexcept; every `DocumentRefusal` except `value_kind_not_supported`,
     *              `text_not_utf8` and `non_finite_number`, which are write-side answers
     * @complexity  O(bytes)
     * @nondet      none
     * @frozen      no
     * @tests       persist.qpjson.round_trip_preserves_everything_that_matters,
     *              persist.qpjson.refuses_a_foreign_file_and_a_broken_one,
     *              persist.qpjson.rejects_a_graph_with_a_cycle,
     *              persist.qpjson.arbitrary_bytes_never_crash
     */
    [[nodiscard]] qp::authoring::DocumentRefusal from_bytes(
        std::string_view bytes, qp::authoring::DocumentSnapshot& out) const noexcept override;
};

}  // namespace qp::plugins::qpjson
