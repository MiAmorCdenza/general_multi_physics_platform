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

    /**
     * Whether the format has a shape for a **table of readings**.
     *
     * A readings table is not a series: one row per measurement, each carrying a value, its standard uncertainty and
     * where it came from, with no time axis at all. A format that writes a trace -- one row per sample, one column
     * per channel -- cannot express it, and one that writes a table usually can. Declared separately from
     * `keeps_uncertainty` because the two are independent questions: a CSV keeps both, a plot dump might keep
     * neither, and a spreadsheet-shaped format could easily have a place for the error bar and no place for the
     * reading *kind*.
     *
     * The platform's own loop ends in a report, and the table this flag is about is the artifact that report quotes:
     * a student's list of readings with their error bars and the device each came from. It is declared here rather
     * than discovered when the write fails, which is the same argument the uncertainty flag carries.
     */
    bool keeps_readings = false;

    /**
     * Whether the format has a shape for a **table of fitted parameters**.
     *
     * A third kind of table, and a third question -- appended here rather than inserted above, because a positional
     * aggregate **is** a shape and a field added at the front silently re-labels every one of them. That was learned
     * on `keeps_readings`, which cost a red case (`with_uncertainty` returned one format instead of two) before the
     * rule was written down.
     *
     * A format that writes a readings table almost always writes this one, and the two are still declared
     * separately, for the reason every capability here is declared rather than inferred: "almost always" is not a
     * fact about a format, and a caller must be able to find out before choosing one.
     */
    bool keeps_fit = false;
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
 * @frozen      yes -- the **tags** are frozen; the set may gain a reason, because the alternative is a
 *              writer that cannot report a full disk and has to report success instead
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
    /// The destination could not be written: a directory that does not exist, no permission, no space.
    ///
    /// Reported by the writer rather than by the pre-flight, and the distinction is the point of having
    /// it: the pre-flight answers "may this export proceed", which is knowable before touching the
    /// filesystem, while this answers "did it", which only the write knows. Found by writing the first
    /// real exporter -- until then there was no code for the most ordinary failure an exporter has, and
    /// the choice was between reporting success on a failed write and reusing a code that means
    /// something else.
    could_not_write = 5,
    /// The request names a subject the format has no shape for: a table, in a format that writes a series.
    ///
    /// **One code for every subject, and the subject names itself.** The first version of this was
    /// `readings_not_supported`, which was the minimal shape while there was one table to be refused; a second
    /// subject (the fitted parameters) is the condition under which a minimal shape is replaced by the complete one,
    /// because `fit_not_supported` beside `readings_not_supported` would be two codes for one condition and a third
    /// would follow. What a caller does about it is the same either way -- pick another format rather than another
    /// policy -- and `ExportSubject` says which table was refused, so a sentence can still be specific.
    ///
    /// Kept distinct from `uncertainty_not_supported`, which is a different problem with a different fix: that one
    /// says "this format would lose your error bars", this one says "this format cannot write this *kind* of table".
    subject_not_supported = 6,
    /// The request names a subject and does not carry it: `subject == readings` with no `readings` pointer.
    ///
    /// A caller's bug rather than a format's limitation, and it is refused for the reason `shape_mismatch` is: the
    /// alternative is a writer that writes an empty table and reports success.
    subject_missing = 7,
};

/// @brief Stable short name of a refusal, for a message or a log line.
[[nodiscard]] constexpr const char* to_string(ExportRefusal r) noexcept {
    switch (r) {
        case ExportRefusal::ok: return "ok";
        case ExportRefusal::unknown_format: return "unknown_format";
        case ExportRefusal::uncertainty_not_supported: return "uncertainty_not_supported";
        case ExportRefusal::nothing_to_write: return "nothing_to_write";
        case ExportRefusal::shape_mismatch: return "shape_mismatch";
        case ExportRefusal::could_not_write: return "could_not_write";
        case ExportRefusal::subject_not_supported: return "subject_not_supported";
        case ExportRefusal::subject_missing: return "subject_missing";
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
/**
 * @brief What a request asks a format to write.
 *
 * Two subjects and no third: the platform's loop produces a **series** (a run's samples, over time) and a
 * **table of readings** (one row per measurement, no time axis), and they are written to different files because
 * they are different tables. See `ExportRequest::subject`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A value of this type names exactly one of the two tables
 * @errors      noexcept
 * @frozen      no
 * @tests       io.export.a_readings_table_is_not_a_series,
 *              io.export.a_fit_table_is_a_third_subject
 */
enum class ExportSubject : std::uint8_t {
    /// The run's series: one row per sample, one column per channel.
    trace = 0,
    /// The measurements: one row per reading, with its uncertainty and its source.
    readings = 1,
    /// The fitted parameters: one row per coefficient, with its standard uncertainty, plus the fit's own quality
    /// numbers.
    ///
    /// The third artifact of one session, and the one a grade usually rests on: a lab report quotes the readings and
    /// then the fitted numbers, and of the two the uncertainty of a coefficient is what the result *is*. It is a
    /// table rather than a series for the same reason the readings are -- no time axis, one row per quantity.
    fit = 2,
};

/// @brief Stable short name of a subject, for a message or a log line.
[[nodiscard]] constexpr const char* to_string(ExportSubject subject) noexcept {
    switch (subject) {
        case ExportSubject::trace: return "trace";
        case ExportSubject::readings: return "readings";
        case ExportSubject::fit: return "fit";
    }
    return "unknown";
}

struct ExportRequest final {
    /// Which of the two tables the caller wants written.
    ///
    /// **Two subjects rather than one request that carries everything.** A format writes one table per file -- a CSV
    /// with two tables in it is not a CSV -- so a request that carried both would leave the choice to the writer,
    /// which is the format quietly deciding what the user asked for. The subject says it instead, and `check_export`
    /// refuses a request whose subject and pointers disagree.
    ExportSubject subject = ExportSubject::trace;
    /// The series: samples, channels and uncertainties. Set when `subject == trace`.
    const Trace* trace = nullptr;
    /// The readings: one row per measurement, each with its own value, uncertainty and kind. Set when
    /// `subject == readings`.
    ///
    /// Borrowed, like the trace, and for the same reason: an exporter owns no data. What a reading *is* lives in
    /// `runtime/store`, which is why this is a `Dataset` and not a list of numbers.
    const Dataset* readings = nullptr;
    /// The fitted parameters. Set when `subject == fit`.
    ///
    /// Borrowed, and read only: an export never runs a fit. Whoever ran it -- the fit panel, a script, a future
    /// headless tool -- owns the result, and an exporter that fitted again would be a second source of one number,
    /// which is the failure this whole repository is arranged against. A caller with nothing fitted has nothing to
    /// export, and says so before choosing a file name.
    const FitResult* fit = nullptr;
    /// One label per coefficient, naming it as the model names it -- `a` and `b` for a line's intercept and slope.
    /// Optional, and the same rule as `reading_labels`: a vector that is present and short is refused rather than
    /// padded.
    const std::vector<std::string>* coefficient_labels = nullptr;
    /// One label per reading, naming where it came from -- a node's name, a device's, a file's. Optional.
    ///
    /// **A label rather than a `NodeId`, and this is the layering rule showing up in a signature.** The dataset
    /// records each reading's source as `(index, generation)` in the graph, and `runtime/io` may not depend on
    /// `graph`: this module is about turning values into text, and a module that had to know what a node is in
    /// order to write a column would have made every format's task depend on the graph. So the caller that *does*
    /// know -- the window, which holds both -- resolves the names and passes them here. When the vector is null or
    /// too short, the column is written with the reading's index instead, which is still a provenance a reader can
    /// follow back through the session.
    const std::vector<std::string>* reading_labels = nullptr;
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
