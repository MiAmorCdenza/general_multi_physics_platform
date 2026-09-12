/**
 * @file csv_exporter.hpp
 * @brief The first real export format: a comma-separated table a person can open and a report can quote.
 *
 * ## What this format is for
 *
 * The trace panel's numbers are for looking at; a CSV is for the report. That is the artifact a student
 * pastes into a lab write-up and a teacher opens in a spreadsheet, so the two properties that matter are
 * that a human can read it and that every number arrives with what makes it a measurement.
 *
 * ## The decision this file exists to make: yes, CSV can carry the uncertainty
 *
 * `check_export` refuses an export when the caller requires the uncertainty and the format declares it
 * cannot keep one. For CSV the honest answer is **yes**, and the reason is the shape of a lab table:
 * every measured quantity gets a column for its value and a column for its standard uncertainty, exactly
 * as `x`, `Δx`, `t`, `Δt` are tabulated by hand. So the columns are
 *
 * ```
 * t [s],displacement [m],displacement_u [m],velocity [m/s],velocity_u [m/s]
 * ```
 *
 * and the `_u` column next to a value is where its error bar lives. A format that answered "no" here
 * would be refusing to do the one thing this platform is for.
 *
 * ## What makes it worth writing carefully: unknown is not zero
 *
 * `UncertaintyKind` has three states and the platform's central rule is that the third is not the
 * second: `unknown` means nobody has quantified the error, `exact` means it is genuinely zero. A naive
 * exporter writes `0.0` for both and destroys the distinction in the export step -- after which every
 * reader of the file believes the error was measured and found to be zero. So an unquantified
 * uncertainty is written as an **empty field**, which is how a table says "nothing here", and an exact
 * one is written as `0`.
 *
 * ## What a CSV deliberately does not carry
 *
 * The run id, the seed, the toolchain, the plugin versions: a CSV is a table and has nowhere to put
 * them. A `#` comment line would be the usual trick and it is not used here, because it turns the
 * first row of every reader's table into a row of data -- the information would be there and the table
 * would be wrong. Provenance belongs to the run ledger and to the document; a caller who needs it
 * exports the document (`.qpd`), which carries the graph and the record alongside the numbers.
 *
 * ## Non-finite values are written, not dropped
 *
 * A diverged run is exactly what a user needs to *see*, so an infinity is written as `inf` and a NaN as
 * `nan`. Neither is a number in the strict sense and both are what every numeric reader parses them as;
 * writing an empty field instead would make a diverged run look like a gap in the data.
 *
 * ## Encoding, and the one case where a byte-order mark helps
 *
 * The file is UTF-8. A byte-order mark is written **only when the table holds a byte above ASCII**,
 * which is precisely when a reader that guesses the encoding will guess wrong: Excel on a Chinese
 * Windows reads a BOM-less file as ANSI and shows mojibake, and an all-ASCII table is unaffected by the
 * choice -- so the three bytes appear where they do good and never where they are merely noise.
 *
 * @ownership   observes (an exporter owns no data)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The number of data columns is `1 + 2 * channel_count()`
 * @errors      Reports through `ExportRefusal` rather than throwing
 * @frozen      no
 * @tests       csv.export.uncertainty_is_carried_in_its_own_column
 */
#pragma once

#include <qp/runtime/io.hpp>

#include <string>

namespace qp::plugins::csv {

/**
 * @brief Writes a trace as a comma-separated table.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `to_text` and `write` agree about whether a trace is exportable
 * @errors      noexcept; every failure is an `ExportRefusal`
 * @frozen      no
 * @tests       csv.export.uncertainty_is_carried_in_its_own_column
 */
class CsvExporter final : public qp::runtime::IExporter {
public:
    /// @brief The field separator. Named because the quoting rule is about this character.
    static constexpr const char* kSeparator = ",";

    /// @brief The suffix marking a column as the standard uncertainty of the one before it.
    static constexpr const char* kUncertaintySuffix = "_u";

    /// @brief The bytes a UTF-8 file starts with when a byte-order mark is written.
    static constexpr const char* kByteOrderMark = "\xEF\xBB\xBF";

    /**
     * @brief The format this exporter writes.
     *
     * A block comment rather than `///` lines, and not for looks: the contract gate reads block
     * comments and treats `///` runs as ordinary notes, so a contract written with `///` is a contract
     * the gate cannot see -- its `@tests` ids vanish and the case they name is reported as an orphan.
     * The first version of this file did exactly that, and the gate said so.
     *
     * @ownership   borrows (a function-local static, valid for the process)
     * @thread      main
     * @pre         none
     * @post        `name` is "qp.csv" and `extensions` holds "csv"
     * @invariant   The same object every call
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       csv.export.declares_what_it_can_carry
     */
    [[nodiscard]] const qp::runtime::FormatDesc& format() const noexcept override;

    /**
     * @brief Renders `trace` as the table's text, without touching the filesystem.
     *
     * Separated from `write` so the content can be asserted without a temporary file, and so a caller
     * that wants the table in memory -- a test, the clipboard, a report generator -- is not forced
     * through a path. It runs the **same** pre-flight the file path runs, so the bytes and the file can
     * never disagree about whether a trace is exportable.
     *
     * @param trace The trace to render. May be empty, inconsistent, or hold non-finite values.
     * @param out   Cleared at entry; holds the whole table on success.
     *
     * @ownership   observes `trace`, owns `out`'s content
     * @thread      main
     * @pre         none
     * @post        On `ok`, `out` is a complete table and every row has the same column count;
     *              otherwise `out` is empty
     * @invariant   The same trace produces the same bytes
     * @errors      noexcept; `nothing_to_write` for an empty trace, `shape_mismatch` for a trace whose
     *              samples disagree with its channels
     * @complexity  O(samples * channels)
     * @nondet      none
     * @frozen      no
     * @tests       csv.export.uncertainty_is_carried_in_its_own_column,
     *              csv.export.unknown_is_not_zero,
     *              csv.export.fields_with_separators_are_quoted,
     *              csv.export.a_diverged_sample_is_written,
     *              csv.export.byte_order_mark_only_when_it_helps,
     *              csv.export.refuses_a_trace_with_nothing_in_it
     */
    [[nodiscard]] qp::runtime::ExportRefusal to_text(const qp::runtime::Trace& trace,
                                                    std::string& out) const noexcept;

    /**
     * @brief Writes the table to `request.path`.
     *
     * Runs `check_export` first, so a caller that skipped the pre-flight is told the format cannot keep
     * the uncertainty rather than discovering it in a file that has already been published.
     *
     * @param request The trace and the destination.
     *
     * @ownership   observes `request`
     * @thread      main
     * @pre         none
     * @post        On `ok`, the file at `request.path` exists and holds the whole trace; on any other
     *              answer no file was created by this call
     * @invariant   Never reports success for a write that failed
     * @errors      noexcept; the pre-flight's answers plus `could_not_write` when the destination
     *              cannot be opened or the write fails part-way (a full disk)
     * @complexity  O(samples * channels) plus the write
     * @nondet      only through the filesystem
     * @frozen      no
     * @tests       csv.export.writes_a_file_a_reader_can_open,
     *              csv.export.refuses_a_destination_it_cannot_write
     */
    [[nodiscard]] qp::runtime::ExportRefusal write(
        const qp::runtime::ExportRequest& request) noexcept override;

    /**
     * @brief Whether this exporter can be used.
     *
     * Always true: the format needs nothing but the standard library. Declared rather than omitted so a
     * "save as" list has one rule for every format instead of a special case for the ones that cannot
     * fail.
     *
     * A block comment as well, so this contract is one the gate can read.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Always true
     * @invariant   Constant
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       csv.export.declares_what_it_can_carry
     */
    [[nodiscard]] bool is_available() const noexcept override { return true; }
};

}  // namespace qp::plugins::csv
