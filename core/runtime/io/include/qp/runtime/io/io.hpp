/**
 * @file io.hpp
 * @brief The export contract: what a format must be able to say, and the registry that finds one.
 *
 * ## Why the contract is foundation and every format is a plugin
 *
 * CSV, TSV, JSON, HDF5, a spreadsheet, a figure, a lab's own templated report --
 * every one is a defensible answer, and which one is right depends on the course,
 * the institution and the journal. A platform that embedded CSV would be deciding
 * for everybody, and the second format it needed would be a core change.
 *
 * So this module owns the interface and the registry. It writes no file itself.
 *
 * ## The one thing the contract refuses to lose
 *
 * Charter promise 3 is "changing an instrument's precision must actually change
 * the final uncertainty", and promise 1 is that an instrument can be added without
 * touching model code. Both depend on the uncertainty surviving the export: a CSV
 * of bare numbers is exactly the artifact this platform exists to replace, because
 * the uncertainty is then reconstructed by hand in a spreadsheet -- or, far more
 * often, dropped.
 *
 * Hence `ExportCapabilities::keeps_uncertainty`. A format that cannot carry it
 * must **say so**, and a caller who cares can refuse the format rather than
 * silently publish a table that lost the error bars. This is the difference between
 * a limitation a user can see and one they discover in review.
 *
 * ## Why a capability query rather than a "can you handle this" method
 *
 * Capabilities are static facts about a format: it either has a place to put an
 * uncertainty or it does not. Asking them as data lets a UI filter its "save as"
 * list *before* the user picks, and lets a test assert the property without
 * writing a file.
 *
 * @ownership   pure (contract) / owns (the registry)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No format is implemented in this module
 * @errors      reports refusal through diag::Result
 * @frozen      no
 * @tests       io.registry.register_and_find, io.capabilities.uncertainty_is_declared,
 *              io.export.refuses_a_format_that_cannot_carry_uncertainty,
 *              io.registry.duplicate_is_refused, io.registry.filters_by_capability,
 *              io.refusal_names_are_stable
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/runtime/trace/trace.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qp::runtime {

/**
 * @brief What a format can carry.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A format that cannot carry uncertainty never claims to
 * @errors      noexcept
 * @frozen      no
 * @tests       io.capabilities.uncertainty_is_declared
 */
struct ExportCapabilities final {
    /**
     * Whether the format has a place to put a standard uncertainty.
     *
     * The field that matters most. A format with `keeps_uncertainty == false` is
     * still useful -- a quick look, a plot, an interchange with a tool that has no
     * concept of uncertainty -- but a caller who needs the uncertainty to survive
     * must be able to find that out before writing, not after.
     */
    bool keeps_uncertainty = false;
    /// Whether one file can hold several datasets (a multi-column table, a workbook).
    bool multi_dataset = false;
    /// Whether the format can carry the sample times, so a series stays a series.
    bool keeps_time = false;
    /// Whether the format is text a human can read and diff.
    bool is_text = false;
    /// Whether the format preserves the physical dimension of each column.
    bool keeps_dimension = false;
};

/**
 * @brief Stable description of a format.
 *
 * @ownership   owns (the strings and the extension list)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `name` and `extensions` are non-empty for a registered format
 * @errors      noexcept
 * @frozen      no
 * @tests       io.registry.register_and_find
 */
struct FormatDesc final {
    /// Stable identifier, e.g. "qp.csv". Used in logs and in a saved run's record.
    std::string name{};
    /// What a user sees in a "save as" list.
    std::string label{};
    /// Lower-case extensions without the dot, e.g. {"csv"}. The first is the default.
    std::vector<std::string> extensions{};
    /// What the format can carry. See ExportCapabilities.
    ExportCapabilities capabilities{};
};

/**
 * @brief Why an export was refused.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One code per distinct reason
 * @errors      noexcept
 * @frozen      yes
 * @tests       io.export.refuses_a_format_that_cannot_carry_uncertainty
 */
enum class ExportRefusal : std::uint8_t {
    ok = 0,
    /// No format is registered under that name.
    unknown_format = 1,
    /// The format cannot carry an uncertainty and the caller required one.
    uncertainty_not_supported = 2,
    /// The dataset or trace holds nothing to write.
    nothing_to_write = 3,
    /// The trace and the dataset disagree about how many channels there are.
    shape_mismatch = 4,
};

/// @brief Stable short name of a refusal, for a message or a log line.
[[nodiscard]] constexpr const char* to_string(ExportRefusal r) noexcept {
    switch (r) {
        case ExportRefusal::ok: return "ok";
        case ExportRefusal::unknown_format: return "unknown_format";
        case ExportRefusal::uncertainty_not_supported: return "uncertainty_not_supported";
        case ExportRefusal::nothing_to_write: return "nothing_to_write";
        case ExportRefusal::shape_mismatch: return "shape_mismatch";
    }
    return "unknown";
}

/**
 * @brief What a caller is asking to export.
 *
 * Passed by value in the sense that the exporter may read it freely but must not
 * keep references: an export happens synchronously, and a format that retained a
 * pointer into a caller's trace would dangle the moment the run moved on.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `trace` outlives the export call and nothing beyond it
 * @errors      noexcept
 * @frozen      no
 * @tests       io.export.refuses_a_format_that_cannot_carry_uncertainty
 */
struct ExportRequest final {
    /// The readings, with their times and channels.
    const Trace* trace = nullptr;
    /// A destination path. Its meaning is the format's business.
    std::string path{};
    /// Whether the caller insists the uncertainty survives.
    ///
    /// Set this when the file is evidence -- a lab report, a dataset someone will
    /// analyse. Leave it false for a screenshot-grade export. A format that cannot
    /// carry it and a request that requires it is refused, with the reason named.
    bool require_uncertainty = false;
};

/**
 * @brief A format writer.
 *
 * Implementations are plugins.
 *
 * @ownership   observes (the plugin owns itself)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A failed call leaves no partial file claim: see the write contract
 * @errors      reports failure through Result rather than throwing
 * @frozen      no
 * @tests       io.export.refuses_a_format_that_cannot_carry_uncertainty
 */
class IExporter {
public:
    IExporter() = default;
    virtual ~IExporter() = default;
    IExporter(const IExporter&) = delete;
    IExporter& operator=(const IExporter&) = delete;

    /// @brief The format this exporter writes. Its `name` must match the registration.
    [[nodiscard]] virtual const FormatDesc& format() const noexcept = 0;

    /**
     * @brief Writes `request`.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `request.trace != nullptr`
     * @post        On success the file exists and holds the whole trace
     * @invariant   On failure the caller is told, rather than finding a truncated file
     * @errors      noexcept; returns a refusal rather than throwing, including the
     *              refusals check_export would have reported, so an exporter that is
     *              called directly still cannot silently drop an uncertainty
     * @complexity  implementation-defined
     * @nondet      only through the filesystem
     * @frozen      no
     * @tests       io.export.refuses_a_format_that_cannot_carry_uncertainty
     */
    [[nodiscard]] virtual ExportRefusal write(const ExportRequest& request) noexcept = 0;

    /**
     * @brief Whether this exporter can actually be used right now.
     *
     * A format whose library is missing, or whose destination directory does not
     * exist, is registered but unusable. Reported as data so a "save as" list can
     * grey an entry out rather than offering a choice that fails at the end.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Constant for the lifetime of the exporter
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       io.registry.register_and_find
     */
    [[nodiscard]] virtual bool is_available() const noexcept = 0;
};

/**
 * @brief Registry of export formats.
 *
 * @ownership   owns (the table, never the exporters)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A format name appears at most once
 * @errors      reports refusal through diag::Result
 * @frozen      no
 * @tests       io.registry.register_and_find
 */
class FormatRegistry final {
public:
    FormatRegistry() = default;
    FormatRegistry(const FormatRegistry&) = delete;
    FormatRegistry& operator=(const FormatRegistry&) = delete;

    /**
     * @brief Registers an exporter.
     *
     * @ownership   observes (`exporter` outlives the registration)
     * @thread      main
     * @pre         `exporter` is not null and its format has a name and an extension
     * @post        find_by_name and find_by_extension locate it
     * @invariant   On failure the registry is unchanged
     * @errors      noexcept; returns invalid_argument for a null exporter, an empty
     *              name, or an empty extension list; duplicate_connection for a
     *              repeated name or extension
     * @complexity  O(formats)
     * @nondet      none
     * @frozen      no
     * @tests       io.registry.register_and_find, io.registry.duplicate_is_refused
     */
    [[nodiscard]] diag::Result<void> add(IExporter* exporter) noexcept;

    /// @brief Removes an exporter, for a plugin that is unloading.
    [[nodiscard]] diag::Result<void> remove(std::string_view name) noexcept;

    /// @brief The exporter registered under `name`, or null.
    [[nodiscard]] IExporter* find_by_name(std::string_view name) const noexcept;

    /// @brief The exporter for a file extension (case-insensitive), or null.
    [[nodiscard]] IExporter* find_by_extension(std::string_view extension) const noexcept;

    /// @brief Every registered exporter, in registration order.
    [[nodiscard]] std::vector<IExporter*> all() const noexcept;

    /// @brief Every registered exporter that can carry an uncertainty.
    ///
    /// The list a "save for a lab report" dialog should offer. Computed here rather
    /// than in the view so that the property can be tested without a dialog.
    [[nodiscard]] std::vector<IExporter*> with_uncertainty() const noexcept;

    /// @brief Number of registered formats.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<IExporter*> entries_{};
};

/**
 * @brief Validates a request against a format's declared capabilities, without writing.
 *
 * Split out from the export itself so that the decision can be tested and shown to
 * a user before anything touches the filesystem -- and so that "this format cannot
 * keep your uncertainties" is answerable in a dialog rather than after a write that
 * already succeeded and lost them.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        Returns ok exactly when the export may proceed
 * @invariant   Depends only on its arguments
 * @errors      noexcept
 * @complexity  O(datasets)
 * @nondet      none
 * @frozen      no
 * @tests       io.export.refuses_a_format_that_cannot_carry_uncertainty
 */
[[nodiscard]] ExportRefusal check_export(const IExporter& exporter,
                                        const ExportRequest& request) noexcept;

}  // namespace qp::runtime
