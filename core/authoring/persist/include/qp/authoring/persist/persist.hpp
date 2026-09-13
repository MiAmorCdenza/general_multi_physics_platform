/**
 * @file persist.hpp
 * @brief The contract for saving and loading a document: bytes out, bytes in, and a reason when not.
 *
 * ## Why the contract is here and the format is not
 *
 * A saved document is three things: the graph, the per-view layout slots, and a title.
 *
 * Turning that into bytes is a **format**, and `docs/plan-tree.md` puts formats with content: a format
 * is a choice -- JSON because a teacher can diff it, a compact binary because a large model loads
 * slowly, YAML because the experiment descriptions are already YAML -- and a platform that ships one
 * choice as the only choice has made that decision for every consumer.
 *
 * What cannot be a plugin is the **shape of the question**. That is what this module owns: the
 * interface says "a document becomes bytes and comes back"; a plugin says what the bytes mean. A Save
 * dialog, a recent-files list, and a round-trip test are all written against the interface and do not
 * know which format they are holding.
 *
 * ## Why the two directions do not take the same argument
 *
 * Writing **borrows**. `DocumentSource` holds a pointer to the session's graph and to the document's
 * layout slots. Nothing on the save path may hold a second copy of the graph: a second copy is a second
 * answer to "what is in this document", and the two drift the moment anyone edits either.
 *
 * Reading **owns**. `DocumentSnapshot` holds the graph, because a graph that has just been parsed has
 * to live somewhere and only the caller can decide where. That a snapshot is a value, produced by one
 * load and handed back, is what keeps it from becoming the second handle `authoring/document` exists to
 * avoid -- nothing edits it, so there is nothing to drift.
 *
 * ## Why the snapshot is filled rather than returned
 *
 * `graph::Graph` is movable and deliberately **not copyable**: copying a graph copies its generation
 * counters, and the copies' handles would then both claim to be valid. `diag::Result<T>` hands its
 * payload back by reference and `value_or` copies its fallback, so a `Result<DocumentSnapshot>` could
 * not be unwrapped at all. Hence `from_bytes` fills a caller-provided snapshot and reports through a
 * refusal code -- the same correction `BatchOperator::make` needed one layer down, and the reason to
 * state it here rather than discover it again.
 *
 * ## Why a refusal is a code and not a bool
 *
 * "It did not load" is not actionable. `malformed` and `unsupported_version` are different problems
 * with different fixes: the first says the file is not what it claims to be, the second says this build
 * is older than the file and a newer one would open it. A bool would send the user to a log file to
 * find out which of the two they have.
 *
 * ## What this module does not do
 *
 * It does not touch the filesystem. `to_bytes` and `from_bytes` deal in strings, so that every property
 * worth asserting -- a document survives a round trip, a truncated file is refused, arbitrary bytes do
 * not crash the parser -- is testable without a temporary directory, and so that a caller who wants to
 * keep a document in memory, in a database, or in a test can do so.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No function here reads or writes a file
 * @errors      See each declaration
 * @frozen      no
 * @tests       persist.format.writes_and_reads_through_the_interface
 */
#pragma once

#include <qp/authoring/document/document.hpp>

#include <qp/graph/structure/graph.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qp::authoring {

/**
 * @brief Why a save or a load did not happen.
 *
 * The set is closed and the numeric tags are stable, so a saved log line keeps its meaning.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One code per distinct reason; `ok` is zero so a zero-initialised value means success
 * @errors      noexcept
 * @frozen      yes
 * @tests       persist.refusal.codes_are_named
 */
enum class DocumentRefusal : std::uint8_t {
    ok = 0,
    /// The bytes do not begin with this format's marker: a different file, or not a document at all.
    not_a_document = 1,
    /// The marker is present and names a version this build does not read.
    unsupported_version = 2,
    /// The bytes are not what the format can parse: a syntax error, a member of the wrong type, or a
    /// member this version does not know.
    ///
    /// Unknown members are refused rather than ignored, and the version marker is the reason: this
    /// format's compatibility mechanism is a version number, so a member outside the version's shape
    /// means the file and this build disagree about what the file is. Skipping it would load a document
    /// that is not the one that was saved.
    malformed = 3,
    /// The bytes end in the middle of a record. Separate from `malformed` because the user's problem is
    /// a file that was cut short -- a copy that ran out of disk, a download that stopped -- and the fix
    /// is to find the original rather than to wonder what they typed.
    truncated = 4,
    /// The document describes a graph with a cycle. Refused rather than loaded, because a graph the
    /// evaluator cannot order is a graph whose nodes would evaluate in an arbitrary order.
    cyclic_graph = 5,
    /// Two nodes claim the same slot, or the graph already holds a node under one of the handles.
    duplicate_node = 6,
    /// Two edges feed the same input port, which the graph's own invariant allows only one of.
    duplicate_edge = 7,
    /// An edge names a node the document does not hold.
    dangling_edge = 8,
    /// A string the format must carry as text is not valid UTF-8: a title, a node's type or user name,
    /// a text parameter, or a view's layout payload.
    ///
    /// Not mangled into something that is: escaping maps an invalid byte to a replacement character,
    /// which is a silent edit to the user's own data. The refusal names the string instead.
    ///
    /// Reported in **both** directions, and that is deliberate. A format that refuses to write bytes it
    /// cannot represent but reads them anyway can produce a file it cannot save: the invalid bytes reach
    /// a view's payload through a load, and the next save fails. Refusing them at the door is what keeps
    /// "this format is text" a property rather than a hope.
    text_not_utf8 = 9,
    /// A node parameter whose value kind this format cannot carry -- a live field handle, for instance,
    /// which names data that is not in the document and therefore cannot be in the file.
    value_kind_not_supported = 10,
    /// A number that is not finite. JSON has no literal for an infinity or a NaN, so a text format can
    /// report this and nothing more; writing `null` instead would load as a value nobody computed.
    non_finite_number = 11,
};

/**
 * @brief Stable short name of a refusal, for a message or a log line.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Non-null for every enumerator
 * @invariant   Distinct codes have distinct names
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       persist.refusal.codes_are_named
 */
[[nodiscard]] constexpr const char* to_string(DocumentRefusal r) noexcept {
    switch (r) {
        case DocumentRefusal::ok: return "ok";
        case DocumentRefusal::not_a_document: return "not_a_document";
        case DocumentRefusal::unsupported_version: return "unsupported_version";
        case DocumentRefusal::malformed: return "malformed";
        case DocumentRefusal::truncated: return "truncated";
        case DocumentRefusal::cyclic_graph: return "cyclic_graph";
        case DocumentRefusal::duplicate_node: return "duplicate_node";
        case DocumentRefusal::duplicate_edge: return "duplicate_edge";
        case DocumentRefusal::dangling_edge: return "dangling_edge";
        case DocumentRefusal::text_not_utf8: return "text_not_utf8";
        case DocumentRefusal::value_kind_not_supported: return "value_kind_not_supported";
        case DocumentRefusal::non_finite_number: return "non_finite_number";
    }
    return "unknown";
}

/**
 * @brief Stable description of a document format.
 *
 * @ownership   owns (the strings and the extension list)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `name` and `extensions` are non-empty for a usable format
 * @errors      noexcept
 * @frozen      no
 * @tests       persist.format.describes_itself
 */
struct DocumentFormatDesc final {
    /// Stable identifier, e.g. "qp.document.json". Logged, and stored in a record of what was opened.
    std::string name{};
    /// What a user sees in a "save as" list.
    std::string label{};
    /// Lower-case extensions without the dot, e.g. {"qpd"}. The first is the default.
    std::vector<std::string> extensions{};
    /// Whether the bytes are text a human can read and a version control system can diff.
    ///
    /// Declared rather than guessed: a caller that offers "open in an editor" needs to know, and the
    /// platform's answer to "can I diff this file" should not be a lookup table of format names.
    bool is_text = false;
};

/**
 * @brief A document as a writer sees it: the graph and the layout slots, **borrowed**.
 *
 * The two parts are references rather than pointers, so "a source with no graph" is not a state that
 * has to be checked for and refused -- it cannot be constructed. A pointer pair with a validity rule
 * would have put the same question in front of every format, and one of them would eventually answer it
 * by writing an empty graph.
 *
 * @ownership   observes (the referenced graph and layouts outlive the source)
 * @thread      main
 * @pre         The referenced graph and layouts outlive this object
 * @post        none
 * @invariant   Both parts are always present
 * @errors      noexcept
 * @frozen      no
 * @tests       persist.source.borrows_both_parts
 */
struct DocumentSource final {
    /// The graph to write. Not copied: see the file comment on why the save path borrows.
    const qp::graph::Graph& graph;
    /// The per-view layout slots to write.
    const ViewLayouts& layouts;    /// The document's title. May be empty: a document the user has not named yet.
    std::string_view title{};
};

/**
 * @brief A document as a reader produces it: the graph and the layout slots, **owned**.
 *
 * Move-only, because `graph::Graph` is move-only. A default-constructed snapshot is a valid empty
 * document -- no nodes, no layouts, no title -- which is what "new document" means.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A snapshot produced by a load is internally consistent: every edge names a node the
 *              snapshot holds
 * @errors      noexcept
 * @frozen      no
 * @tests       persist.snapshot.is_move_only_and_survives_a_move
 */
struct DocumentSnapshot final {
    DocumentSnapshot() = default;
    DocumentSnapshot(DocumentSnapshot&&) noexcept = default;
    DocumentSnapshot& operator=(DocumentSnapshot&&) noexcept = default;
    DocumentSnapshot(const DocumentSnapshot&) = delete;
    DocumentSnapshot& operator=(const DocumentSnapshot&) = delete;
    ~DocumentSnapshot() = default;

    /// The loaded graph.
    qp::graph::Graph graph{};
    /// The loaded per-view layout slots, in the order the file held them.
    ViewLayouts layouts{};
    /// The loaded title, or empty when the file held none.
    std::string title{};
};

/**
 * @brief A document format. Implementations are plugins.
 *
 * @ownership   observes (the plugin owns itself)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `format()` is stable for the lifetime of the object
 * @errors      Reports failure through a refusal code rather than throwing
 * @frozen      no
 * @tests       persist.format.describes_itself
 */
class IDocumentFormat {
public:
    IDocumentFormat() = default;
    virtual ~IDocumentFormat() = default;
    IDocumentFormat(const IDocumentFormat&) = delete;
    IDocumentFormat& operator=(const IDocumentFormat&) = delete;

    /**
     * @brief The format this object implements. Its `name` is what a log line and a record should use.
     *
     * @ownership   borrows from this object
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   The returned reference stays valid for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       persist.format.describes_itself
     */
    [[nodiscard]] virtual const DocumentFormatDesc& format() const noexcept = 0;

    /**
     * @brief Serialises a document into `out`.
     *
     * `out` is cleared first and holds the whole document on success. A partial write is not a document,
     * and a caller who receives one would have to check a size or a trailing marker to find out -- so
     * on failure `out` is left empty instead.
     *
     * @param source The document to write, borrowed.
     * @param out    Receives the bytes. Cleared at entry.
     *
     * @ownership   observes `source`, owns `out`'s content
     * @thread      main
     * @pre         none
     * @post        On `ok`, `out` holds the whole document; otherwise `out` is empty
     * @invariant   Writing the same source twice produces the same bytes, so a save is idempotent and a
     *              test can compare bytes rather than parse them
     * @errors      noexcept; `value_kind_not_supported`, `text_not_utf8` and `non_finite_number` name
     *              the three things a text format can be asked to carry and cannot, and all three are
     *              reported before any byte of the result is kept
     * @complexity  O(nodes + edges + layouts)
     * @nondet      none
     * @frozen      no
     * @tests       persist.format.writes_and_reads_through_the_interface
     */
    [[nodiscard]] virtual DocumentRefusal to_bytes(const DocumentSource& source,
                                                  std::string& out) const noexcept = 0;

    /**
     * @brief Parses bytes into `snapshot`.
     *
     * The snapshot is left **unchanged** on failure: it is parsed into a local one and moved into place
     * only on success, so a caller whose load failed still holds whatever it held before. A half-loaded
     * document would be worse than none -- the user would see a graph missing the nodes the parser had
     * not reached yet, and would have no way to tell that from a graph that is genuinely incomplete.
     *
     * @param bytes The bytes to parse. May be anything at all: this function is handed the contents of
     *              a file the user picked, and "the file is not a document" is an ordinary answer.
     * @param out   Receives the document. Unchanged on failure.
     *
     * @ownership   observes `bytes`, owns `out`'s content
     * @thread      main
     * @pre         none
     * @post        On `ok`, `out` holds the parsed document; otherwise `out` is unchanged
     * @invariant   Returns a code rather than crashing for **any** input, including truncated and
     *              arbitrary bytes
     * @errors      noexcept; `not_a_document`, `unsupported_version`, `malformed`, `truncated`,
     *              `cyclic_graph`, `duplicate_node`, `duplicate_edge`, `dangling_edge`,
     *              `text_not_utf8`, `value_kind_not_supported`
     * @complexity  O(bytes)
     * @nondet      none
     * @frozen      no
     * @tests       persist.format.writes_and_reads_through_the_interface
     */
    [[nodiscard]] virtual DocumentRefusal from_bytes(std::string_view bytes,
                                                    DocumentSnapshot& out) const noexcept = 0;
};

}  // namespace qp::authoring
