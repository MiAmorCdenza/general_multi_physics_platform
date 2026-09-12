/**
 * @file export_controller.hpp
 * @brief Exporting the panel's trace: ask first, then write, and say what happened.
 *
 * ## The loop this file exists to close
 *
 * `runtime/io` owns the question "can this format carry what this session has", and the answer has to
 * come **before** a path is chosen, because the user's alternative -- pick a file name, watch the write
 * succeed, and discover later that the error bars are gone -- is the outcome the pre-flight exists to
 * prevent. So the sequence here is fixed:
 *
 *   1. `check_export` with the request the measurement model builds (the trace, the path, and the
 *      uncertainty required exactly when the session quantified any);
 *   2. only if that answers `ok`, write;
 *   3. report the answer, whether it came from the pre-flight or from the disk.
 *
 * ## Why the request is not built here
 *
 * The policy -- *when* the uncertainty is required -- belongs to the session that knows whether anything
 * was quantified, so it lives in `MeasurementModel::export_request`. A controller that assembled its own
 * request could require something the panel did not, and the panel's answer and the export's answer would
 * differ for the same session: an export button that offers what the exporter then refuses.
 *
 * ## Why the formats are a mounted registry
 *
 * The same inversion as `execution_binders` and `document_formats`: `views` must not depend on `plugins`,
 * so the view layer owns the registry and the application -- the one place allowed to know which plugins
 * exist -- registers them.
 *
 * @ownership   observes (the model, the registered exporters)
 * @thread      main
 * @pre         The mounted exporters outlive the registry
 * @post        none
 * @invariant   No export is attempted without the pre-flight having answered `ok`
 * @errors      See each declaration
 * @frozen      no
 * @tests       export.writes_the_panels_trace
 */
#pragma once

#include <qp/views/model/measurement_model.hpp>

#include <qp/runtime/io.hpp>

#include <string>

namespace qp::views::model {

/**
 * @brief The export formats the application registered, in the order a "save as" list should offer them.
 *
 * @ownership   borrows (the returned reference outlives any caller)
 * @thread      main
 * @pre         none
 * @post        Returns an empty registry when nothing has been mounted
 * @invariant   The same object every call
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       export.formats_are_mounted_once
 */
[[nodiscard]] qp::runtime::FormatRegistry& export_formats() noexcept;

/**
 * @brief Registers `exporter` in the list the window offers.
 *
 * A null exporter is ignored; a duplicate name or extension is refused by the registry itself, which is
 * where that rule belongs -- two exporters under one name would make "save as CSV" depend on plugin load
 * order, and a file is evidence.
 *
 * @ownership   observes `exporter` (the caller keeps ownership)
 * @thread      main
 * @pre         none
 * @post        On success the registry finds `exporter` by name and by extension
 * @invariant   Mounting does not reorder what is already registered
 * @errors      noexcept; a refusal from the registry is reported through the return value
 * @complexity  O(formats)
 * @nondet      none
 * @frozen      no
 * @tests       export.formats_are_mounted_once
 */
[[nodiscard]] qp::diag::Result<void> mount_export_format(qp::runtime::IExporter* exporter) noexcept;

/**
 * @brief What one export attempt produced.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `message` is empty exactly when `ok` is true
 * @errors      noexcept
 * @frozen      no
 * @tests       export.writes_the_panels_trace
 */
struct ExportReport final {
    /// Whether the file was written.
    bool ok = false;
    /// The pre-flight's answer, or the writer's. `ok` when the export succeeded.
    qp::runtime::ExportRefusal refusal = qp::runtime::ExportRefusal::ok;
    /// Where it was going, as given.
    std::string path{};
    /// The format's name, for the record.
    std::string format_name{};
    /// What to tell the user: a sentence, not a code. A student reading the window needs to know whether
    /// to change the format, the destination, or the data.
    std::string message{};
};

/**
 * @brief A sentence for an export refusal, saying what the user can do about it.
 *
 * @ownership   owns the result
 * @thread      main
 * @pre         none
 * @post        Non-empty for every enumerator, including `ok`
 * @invariant   Depends only on its argument
 * @errors      May allocate; allocation failure terminates
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       export.refuses_before_writing
 */
[[nodiscard]] std::string describe_export_refusal(qp::runtime::ExportRefusal refusal);

/**
 * @brief Runs the pre-flight and, if it passes, writes the panel's trace with `format`.
 *
 * @param model  The measurement session whose trace is exported. Its `export_request` supplies the trace
 *               and the uncertainty policy.
 * @param format The format to write with.
 * @param path   Where to write.
 *
 * @ownership   observes `model`, owns the result
 * @thread      main
 * @pre         none
 * @post        On success the file at `path` holds the trace; on any other answer no file was created by
 *              this call
 * @invariant   `report.refusal == check_export(format, model.export_request(path))` whenever the write
 *              did not fail
 * @errors      Never throws except on allocation failure, which terminates; every other failure becomes
 *              the refusal and the sentence in the report
 * @complexity  O(samples * channels) plus the write
 * @nondet      only through the filesystem
 * @frozen      no
 * @tests       export.writes_the_panels_trace,
 *              export.refuses_before_writing,
 *              export.reports_a_format_that_cannot_carry_the_uncertainty,
 *              export.reports_a_destination_it_cannot_write
 */
[[nodiscard]] ExportReport export_trace(const MeasurementModel& model,
                                        qp::runtime::IExporter& format, const std::string& path);

}  // namespace qp::views::model
