/**
 * @file io.cpp
 * @brief Implementation of the export registry and the pre-flight check.
 */
#include <qp/runtime/io/io.hpp>

#include <algorithm>
#include <cctype>

namespace qp::runtime {
namespace {

/// @brief A lower-case copy, for case-insensitive extension comparison.
[[nodiscard]] std::string lowered(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

diag::Result<void> FormatRegistry::add(IExporter* exporter) noexcept {
    if (exporter == nullptr) return diag::ErrorCode::invalid_argument;

    const FormatDesc& format = exporter->format();
    if (format.name.empty()) return diag::ErrorCode::invalid_argument;
    // A format with no extension cannot be found from a path, and a "save as"
    // dialog would offer a choice it cannot act on.
    if (format.extensions.empty()) return diag::ErrorCode::invalid_argument;

    // Refused rather than replaced. Two exporters under one name would make "save
    // as CSV" depend on plugin load order, so the same document would produce
    // different files on two machines -- and a file is evidence.
    if (find_by_name(format.name) != nullptr) return diag::ErrorCode::duplicate_connection;

    // An extension collision is worse than a name collision: the caller chose by
    // extension (they typed a path), so which writer runs would depend on load
    // order in a way the user cannot see.
    for (const std::string& extension : format.extensions) {
        if (extension.empty()) return diag::ErrorCode::invalid_argument;
        if (find_by_extension(extension) != nullptr) return diag::ErrorCode::duplicate_connection;
    }

    entries_.push_back(exporter);
    return {};
}

diag::Result<void> FormatRegistry::remove(std::string_view name) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [name](const IExporter* e) {
                                     return e->format().name == name;
                                 });
    if (it == entries_.end()) return diag::ErrorCode::unknown_node;
    entries_.erase(it);
    return {};
}

IExporter* FormatRegistry::find_by_name(std::string_view name) const noexcept {
    if (name.empty()) return nullptr;
    for (IExporter* e : entries_) {
        if (e->format().name == name) return e;
    }
    return nullptr;
}

IExporter* FormatRegistry::find_by_extension(std::string_view extension) const noexcept {
    if (extension.empty()) return nullptr;
    // A leading dot is stripped rather than rejected: a caller that has ".csv" from
    // a file dialog and one that has "csv" from a list should both work, and
    // insisting on one spelling is how a "save as" silently finds nothing.
    while (!extension.empty() && extension.front() == '.') {
        extension.remove_prefix(1);
    }
    const std::string want = lowered(extension);
    for (IExporter* e : entries_) {
        for (const std::string& candidate : e->format().extensions) {
            if (lowered(candidate) == want) return e;
        }
    }
    return nullptr;
}

std::vector<IExporter*> FormatRegistry::all() const noexcept {
    return entries_;
}

std::vector<IExporter*> FormatRegistry::with_uncertainty() const noexcept {
    std::vector<IExporter*> out;
    out.reserve(entries_.size());
    for (IExporter* e : entries_) {
        if (e->format().capabilities.keeps_uncertainty) out.push_back(e);
    }
    return out;
}

ExportRefusal check_export(const IExporter& exporter, const ExportRequest& request) noexcept {
    if (request.trace == nullptr) return ExportRefusal::nothing_to_write;

    const Trace& trace = *request.trace;
    if (trace.empty()) return ExportRefusal::nothing_to_write;

    // The trace's channel count is the shape every consumer agrees on; a trace
    // whose samples disagree with its channels is malformed, and writing it would
    // produce a file whose columns do not line up.
    if (!trace.is_consistent()) return ExportRefusal::shape_mismatch;

    // The check this module exists for. Refused before anything touches the
    // filesystem, so a caller learns that the format cannot keep the error bars
    // rather than discovering it in a published table.
    if (request.require_uncertainty && !exporter.format().capabilities.keeps_uncertainty) {
        return ExportRefusal::uncertainty_not_supported;
    }

    return ExportRefusal::ok;
}

}  // namespace qp::runtime
