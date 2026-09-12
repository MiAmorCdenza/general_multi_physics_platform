/**
 * @file document_controller.hpp
 * @brief File menu behaviour: new, save, save as, open -- and the sentences each outcome produces.
 *
 * ## What this object is, and what it is not
 *
 * It is the piece that joins three things that must not know about each other: the **session** (which
 * owns the live graph), the **document** (which owns the title, the path and the per-view layout slots),
 * and a **format** (which turns a document into bytes and back). None of the three may hold the others:
 * a format that knew about a session could not be tested without one, `authoring/document` says in its
 * own file comment that it holds no graph, and the session is the single place a graph may be mutated.
 *
 * So this is where the save path borrows a graph and where an open path installs one, and it is
 * deliberately *not* where bytes are parsed or produced.
 *
 * ## Why the formats arrive as a mounted list
 *
 * The same inversion as `execution_binders`, for the same reason: `views` must not depend on `plugins`,
 * so the view layer declares **where formats come from** and the application -- the one place allowed to
 * know which plugins exist -- puts them there. A window that named `QpJsonFormat` by type could not be
 * built without that plugin.
 *
 * ## Why it listens to the session
 *
 * The dirty flag is what stands between a user and losing work, and the object that must set it is the one
 * that owns the document. A window that marked dirty on the paths it happened to know about would go
 * clean the moment something else edited the graph -- a panel, a command from a script, an undo. The
 * controller therefore registers as a change listener and marks dirty on every change **except** the ones
 * its own load produces, which is why opening a document ends clean rather than immediately dirty.
 *
 * @ownership   observes (the session, the formats) / owns (its document)
 * @thread      main
 * @pre         The session and every mounted format outlive this object
 * @post        none
 * @invariant   Never writes a document the current format refuses
 * @errors      See each declaration
 * @frozen      no
 * @tests       document.save_writes_a_file_that_reopens
 */
#pragma once

#include <qp/authoring/commands/session.hpp>
#include <qp/authoring/document/document.hpp>
#include <qp/authoring/persist/persist.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace qp::views::model {

/**
 * @brief The document formats the application mounted, in the order a "save as" list should offer them.
 *
 * @ownership   borrows (the returned reference outlives any caller)
 * @thread      main
 * @pre         none
 * @post        Returns an empty list when nothing has been mounted
 * @invariant   The same object every call
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       document.formats_are_mounted_once_and_in_order
 */
[[nodiscard]] std::vector<qp::authoring::IDocumentFormat*>& document_formats() noexcept;

/**
 * @brief Adds `format` to the list the window offers.
 *
 * @ownership   observes `format` (the caller keeps ownership)
 * @thread      main
 * @pre         none
 * @post        `format` appears exactly once in `document_formats()` afterwards
 * @invariant   Mounting does not reorder what is already there: order is the order offered
 * @errors      May allocate; allocation failure terminates
 * @complexity  O(n) in the mounted count
 * @nondet      none
 * @frozen      no
 * @tests       document.formats_are_mounted_once_and_in_order
 */
void mount_document_format(qp::authoring::IDocumentFormat* format);

/**
 * @brief What one save or open produced.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `message` is empty exactly when `ok` is true
 * @errors      noexcept
 * @frozen      no
 * @tests       document.save_writes_a_file_that_reopens
 */
struct DocumentReport final {
    /// Whether the operation completed.
    bool ok = false;
    /// The path it was about, as given.
    std::string path{};
    /// The format's name, for the record. Empty when no format was reached.
    std::string format_name{};
    /// Nodes and edges written or read, so a status line can say more than "ok".
    std::size_t nodes = 0;
    std::size_t edges = 0;
    /// What to tell the user. A sentence for the status line, not an error code: the code is in the log,
    /// and a student reading the window needs to know what to do next.
    std::string message{};
};

/**
 * @brief Saves and opens documents for one session, and holds that session's document.
 *
 * @ownership   observes the session, owns the document
 * @thread      main
 * @pre         `session` outlives this object
 * @post        none
 * @invariant   The document's layouts are the ones a save writes and an open installs
 * @errors      See each declaration
 * @frozen      no
 * @tests       document.save_writes_a_file_that_reopens
 */
class DocumentController final : public qp::authoring::IChangeListener {
public:
    /**
     * @brief Binds a controller to a session and the formats it may use.
     *
     * @param session The editing session. Non-const, because opening a document replaces its graph --
     *                through `Session::replace_graph`, which is the one mutation that does not go through
     *                the command bus, and the reason `ChangeKind::reset` exists.
     * @param formats Consulted by the window; the first is the default for a "save as".
     *
     * @ownership   observes both
     * @thread      main
     * @pre         `session` outlives this object
     * @post        The controller receives the session's changes
     * @invariant   A controller is registered as exactly one listener, and removes itself on destruction
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(formats)
     * @nondet      none
     * @frozen      no
     * @tests       document.dirty_tracks_the_session
     */
    DocumentController(qp::authoring::Session& session,
                       std::vector<qp::authoring::IDocumentFormat*> formats);

    DocumentController(const DocumentController&) = delete;
    DocumentController& operator=(const DocumentController&) = delete;
    ~DocumentController() override;

    /// @brief The document being edited: its title, where it came from, and its per-view layout slots.
    [[nodiscard]] const qp::authoring::Document& document() const noexcept { return document_; }
    [[nodiscard]] qp::authoring::Document& document() noexcept { return document_; }

    /// @brief Whether edits have happened since the last save or open.
    [[nodiscard]] bool is_dirty() const noexcept { return document_.is_dirty(); }

    /// @brief Whether the document has never been written to a file.
    [[nodiscard]] bool is_untitled() const noexcept { return document_.is_untitled(); }

    /// @brief The formats this controller was given, in order.
    [[nodiscard]] const std::vector<qp::authoring::IDocumentFormat*>& formats() const noexcept {
        return formats_;
    }

    /// @brief The format a plain "save" should use, or null when none is mounted.
    ///
    /// The first mounted one. When the document was opened from a file, the caller passes that format
    /// explicitly instead -- resaving in a different format than the one it was read as would be a
    /// silent conversion. See the file comment.
    ///
    /// @ownership   borrows
    /// @thread      main
    /// @pre         none
    /// @post        Returns the first non-null format, or null
    /// @invariant   Consistent with `formats()`
    /// @errors      noexcept
    /// @complexity  O(formats)
    /// @nondet      none
    /// @frozen      no
    /// @tests       document.formats_are_mounted_once_and_in_order
    [[nodiscard]] qp::authoring::IDocumentFormat* default_format() const noexcept;

    /**
     * @brief Empties the session and starts a new document.
     *
     * The graph is replaced rather than edited away node by node: "new document" is one event, and an
     * undo stack holding the removal of every node of the previous document would make the next Ctrl+Z
     * an act of archaeology.
     *
     * @ownership   owns the result
     * @thread      main
     * @pre         none
     * @post        The session's graph is empty, the document is untitled and clean, and no layout slot
     *              survives
     * @invariant   The window's canvas rebuilds, because `replace_graph` announces a `reset`
     * @errors      Never fails; every outcome is a sentence in the report
     * @complexity  O(previous graph)
     * @nondet      none
     * @frozen      no
     * @tests       document.new_document_is_empty_and_clean
     */
    [[nodiscard]] DocumentReport new_document();

    /**
     * @brief Writes the session's graph and the document's layouts with `format`.
     *
     * @param format The format to write with. Its own `to_bytes` decides what it can carry, and a
     *               refusal is reported rather than worked around: a document that lost a parameter
     *               because the writer dropped it is worse than one that was not written.
     * @param path   Where to write. Any path the platform accepts; the bytes go out through
     *               `runtime/file`, the one place that knows how a UTF-8 path crosses into an open call.
     *
     * @ownership   owns the result
     * @thread      main
     * @pre         none
     * @post        On success the file exists, `document().source_path()` is `path`, and the document is
     *              clean
     * @invariant   On failure nothing about the document changes: a refused save does not claim the path
     *              and does not clear the dirty flag
     * @errors      Never throws; a format refusal or a file failure becomes a sentence naming the reason
     * @complexity  O(nodes + edges + layouts)
     * @nondet      only through the filesystem
     * @frozen      no
     * @tests       document.save_writes_a_file_that_reopens,
     *              document.a_refused_save_changes_nothing
     */
    [[nodiscard]] DocumentReport save(qp::authoring::IDocumentFormat& format, const std::string& path);

    /**
     * @brief Reads `path` with `format` and installs the document it holds.
     *
     * @param format The format to read with.
     * @param path   The file to read.
     *
     * @ownership   owns the result
     * @thread      main
     * @pre         none
     * @post        On success the session's graph is the one the file held, the document's layouts and
     *              title are the file's, `source_path()` is `path`, and the document is clean
     * @invariant   On failure the session's graph, the document and the dirty flag are all unchanged: a
     *              failed open never leaves half a document behind
     * @errors      Never throws; a file failure or a format refusal becomes a sentence naming the reason
     * @complexity  O(bytes)
     * @nondet      only through the filesystem
     * @frozen      no
     * @tests       document.save_writes_a_file_that_reopens,
     *              document.a_failed_open_changes_nothing,
     *              document.opening_replaces_the_graph_and_clears_undo
     */
    [[nodiscard]] DocumentReport open(qp::authoring::IDocumentFormat& format, const std::string& path);

    /**
     * @brief Marks the document dirty when the session changes.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The document is dirty, unless this object is the one making the change
     * @invariant   A load in progress does not mark the document dirty
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       document.dirty_tracks_the_session
     */
    void on_change(const qp::authoring::Change& change) noexcept override;

private:
    qp::authoring::Session* session_;
    std::vector<qp::authoring::IDocumentFormat*> formats_;
    qp::authoring::Document document_{};
    qp::authoring::ListenerId listener_{};
    /// @brief Whether a change in flight is this object's own load. See the file comment.
    bool loading_ = false;
};

}  // namespace qp::views::model
