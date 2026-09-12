/**
 * @file document_controller.cpp
 * @brief New, save, open: the sequencing, and the sentence each outcome produces.
 *
 * The sentences are written here rather than in the window for the same reason the measurement panel's
 * gaps are: a message the user reads is a decision about what matters, and two places deciding how to word
 * one warning means the one on screen is not the one that was tested.
 */
#include <qp/views/model/document_controller.hpp>

#include <qp/runtime/file/file.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace qp::views::model {
namespace {

namespace authoring = qp::authoring;
namespace rt = qp::runtime;

/// @brief A sentence for a format refusal, naming what the user can do about it.
///
/// Every branch says something more specific than the code's name, because the reader is a person looking
/// at a message box: `text_not_utf8` tells them nothing they can act on, and "a name in this document is
/// not valid UTF-8" tells them the file cannot hold it rather than that they did something wrong.
[[nodiscard]] std::string describe(authoring::DocumentRefusal refusal) {
    switch (refusal) {
        case authoring::DocumentRefusal::ok:
            return "ok";
        case authoring::DocumentRefusal::not_a_document:
            return "this file is not a document of that format";
        case authoring::DocumentRefusal::unsupported_version:
            return "this file was written by a newer build of the platform";
        case authoring::DocumentRefusal::malformed:
            return "the file is not shaped the way this format requires";
        case authoring::DocumentRefusal::truncated:
            return "the file ends in the middle of the document -- a copy or a download that did not "
                   "finish";
        case authoring::DocumentRefusal::cyclic_graph:
            return "the graph it describes has a cycle, which this platform cannot evaluate";
        case authoring::DocumentRefusal::duplicate_node:
            return "two nodes in it claim the same identity";
        case authoring::DocumentRefusal::duplicate_edge:
            return "two connections in it feed the same input";
        case authoring::DocumentRefusal::dangling_edge:
            return "a connection in it names a node the document does not hold";
        case authoring::DocumentRefusal::text_not_utf8:
            return "it holds text that is not valid UTF-8, which a text format cannot carry without "
                   "changing it";
        case authoring::DocumentRefusal::value_kind_not_supported:
            return "a node parameter holds a live field handle: it names data in this process, so it "
                   "cannot be in a file";
        case authoring::DocumentRefusal::non_finite_number:
            return "a node parameter is infinite or not a number, which JSON cannot write";
    }
    return "the format refused the document";
}

/// @brief A sentence for a file-level failure.
[[nodiscard]] std::string describe(rt::FileOutcome outcome, const std::string& path, bool writing) {
    switch (outcome) {
        case rt::FileOutcome::ok:
            return "ok";
        case rt::FileOutcome::absent:
            return "there is no file at " + path;
        case rt::FileOutcome::unreadable:
            return "the file at " + path + " could not be read (a directory, a lock, or no permission)";
        case rt::FileOutcome::unwritable:
            return std::string{"could not write "} + path +
                   " (a directory that does not exist, no permission, or no space)";
    }
    return writing ? "the write failed" : "the read failed";
}

}  // namespace

std::vector<authoring::IDocumentFormat*>& document_formats() noexcept {
    // A function-local static, for the reason `execution_binders` uses one: the initialisation order of
    // namespace-scope objects across translation units is unspecified, and this list is read from a
    // constructor.
    static std::vector<authoring::IDocumentFormat*> formats;
    return formats;
}

void mount_document_format(authoring::IDocumentFormat* format) {
    if (format == nullptr) return;
    std::vector<authoring::IDocumentFormat*>& formats = document_formats();
    if (std::find(formats.begin(), formats.end(), format) != formats.end()) return;
    formats.push_back(format);
}

DocumentController::DocumentController(qp::authoring::Session& session,
                                       std::vector<authoring::IDocumentFormat*> formats)
    : session_(&session), formats_(std::move(formats)) {
    listener_ = session_->add_listener(*this);
    // A document that was never written is not "dirty": there is nothing a save would recover.
    document_.mark_saved();
}

DocumentController::~DocumentController() {
    if (listener_.valid()) (void)session_->remove_listener(listener_);
}

authoring::IDocumentFormat* DocumentController::default_format() const noexcept {
    for (authoring::IDocumentFormat* format : formats_) {
        if (format != nullptr) return format;
    }
    return nullptr;
}

void DocumentController::on_change(const qp::authoring::Change& change) noexcept {
    (void)change;
    // Skipped for the load this object is performing: opening a document is not an edit, and a window that
    // showed "unsaved changes" immediately after opening a file would train its user to ignore the flag.
    if (loading_) return;
    document_.mark_dirty();
}

DocumentReport DocumentController::new_document() {
    DocumentReport report;
    report.nodes = 0;
    report.edges = 0;

    loading_ = true;
    session_->replace_graph(qp::graph::Graph{});
    loading_ = false;

    // The document itself is replaced, not reset field by field: a title, a path or a layout slot that
    // survived "new" would be attached to a document that is not the one being edited.
    document_ = authoring::Document{};
    document_.mark_saved();

    report.ok = true;
    report.message = "new document";
    return report;
}

DocumentReport DocumentController::save(authoring::IDocumentFormat& format, const std::string& path) {
    DocumentReport report;
    report.path = path;
    report.format_name = format.format().name;

    // The graph is **borrowed** here, not copied: see `DocumentSource`'s contract. Nothing on the save
    // path may hold a second copy of the graph, because a second copy is a second answer to "what is in
    // this document" and the two drift the moment either is edited.
    const authoring::DocumentSource source{session_->graph(), document_.layouts(), document_.title()};

    std::string bytes;
    const authoring::DocumentRefusal refused = format.to_bytes(source, bytes);
    if (refused != authoring::DocumentRefusal::ok) {
        report.message = "cannot save as " + report.format_name + ": " + describe(refused);
        return report;
    }

    const rt::FileOutcome written = rt::write_whole_file(path, bytes);
    if (written != rt::FileOutcome::ok) {
        report.message = describe(written, path, /*writing=*/true);
        return report;
    }

    // Only now: a path the document claims but does not occupy would make the next Ctrl+S write somewhere
    // the user did not choose, and clearing the dirty flag on a failed save is how work gets lost.
    document_.set_source_path(path);
    document_.mark_saved();

    report.ok = true;
    report.nodes = session_->graph().node_count();
    report.edges = session_->graph().edge_count();
    report.message = "saved " + std::to_string(report.nodes) + " nodes and " +
                     std::to_string(report.edges) + " edges to " + path + " as " + report.format_name;
    return report;
}

DocumentReport DocumentController::open(authoring::IDocumentFormat& format, const std::string& path) {
    DocumentReport report;
    report.path = path;
    report.format_name = format.format().name;

    std::string bytes;
    const rt::FileOutcome read = rt::read_whole_file(path, bytes);
    if (read != rt::FileOutcome::ok) {
        report.message = describe(read, path, /*writing=*/false);
        return report;
    }

    // Parsed into a local snapshot: `from_bytes` fills it only on success, so a refusal below cannot have
    // touched the session or the document.
    authoring::DocumentSnapshot loaded;
    const authoring::DocumentRefusal refused = format.from_bytes(bytes, loaded);
    if (refused != authoring::DocumentRefusal::ok) {
        report.message = "cannot open " + path + " as " + report.format_name + ": " + describe(refused);
        return report;
    }

    const std::size_t nodes = loaded.graph.node_count();
    const std::size_t edges = loaded.graph.edge_count();

    loading_ = true;
    session_->replace_graph(std::move(loaded.graph));
    loading_ = false;

    document_.set_title(loaded.title);
    document_.set_source_path(path);
    document_.layouts() = std::move(loaded.layouts);
    document_.mark_saved();

    report.ok = true;
    report.nodes = nodes;
    report.edges = edges;
    report.message = "opened " + path + ": " + std::to_string(nodes) + " nodes, " +
                     std::to_string(edges) + " edges";
    return report;
}

}  // namespace qp::views::model
