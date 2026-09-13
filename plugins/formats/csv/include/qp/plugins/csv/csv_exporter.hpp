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
     * @brief The fitted parameters: one row per coefficient, plus the fit's own quality numbers.
     *
     * **The other number a lab report quotes, and the one a grade usually rests on.** The fit panel says it about its
     * own third column -- "a coefficient without its error bar is the number a lab report overstates, and this column
     * is the reason the fit was worth doing at all" -- and until this existed that column could not leave the window.
     *
     * The table has **one row shape for everything the fit produces**, which is why there is no second table and no
     * comment header:
     *
     * ```
     * model,parameter,value,uncertainty
     * linear,a,0.01010,1.4e-06
     * linear,b,0.51200,0.00300
     * linear,chi_squared,1.2e-04,
     * linear,degrees_of_freedom,7,
     * linear,r_squared,0.99980,
     * ```
     *
     *   - **`model` is a column rather than a line of prose.** This format refuses `#` comment lines by contract --
     *     they would turn the first row of every reader's table into a row of data -- so the fit's identity has to be
     *     a field, and repeating it on each row is what keeps the file machine-readable and self-describing.
     *   - **`parameter` names the row**, and the names of the coefficients are the **caller's**: the panel already
     *     names a line's intercept `a` and its slope `b`, and an exporter that invented its own names would print a
     *     table that does not match the window it came from. The quality rows' names are the format's own and are
     *     fixed here, because they are this table's vocabulary rather than a model's.
     *   - **the uncertainty of a quality number is an empty field**, not `0`: nobody quantified the error of a
     *     chi-squared, and the platform's rule is that unquantified is not zero. A coefficient whose covariance is
     *     missing writes an empty field for the same reason, which is also what the panel shows in words
     *     ("not fitted") rather than as a number.
     *   - **`degrees_of_freedom` is written as it stands**, including the `-1` the fit result uses for "not
     *     applicable": a reader of the file has to be able to see that the fit did not define it, and `0` would say
     *     something else.
     *
     * @param fit    The fit result. Borrowed, and never re-fitted here: an exporter that fitted again would be a
     *               second source of one number.
     * @param labels One label per coefficient, or null. Borrowed.
     * @param out    Receives the table on success.
     *
     * @ownership   owns the text it appends
     * @thread      main
     * @pre         none
     * @post        On ok, `out` holds a header line and one line per coefficient plus one per quality number
     * @invariant   `out` is untouched when the call is refused
     * @errors      `nothing_to_write` for a fit with no coefficients; `shape_mismatch` for a label vector that is
     *              present, not empty, and shorter than the coefficient count
     * @complexity  O(coefficients)
     * @nondet      none
     * @frozen      no
     * @tests       csv.export.a_fit_table_carries_the_error_bar
     */
    [[nodiscard]] qp::runtime::ExportRefusal to_fit_text(const qp::runtime::FitResult& fit,
                                                         const std::vector<std::string>* labels,
                                                         std::string& out);

    /**
     * @brief The readings table: one row per measurement, with its uncertainty and where it came from.
     *
     * **The platform's own loop ends in a report, and until this existed the artifact that report quotes could not be
     * written.** A trace is a *series* -- one row per sample, one column per channel -- while the product of
     * the platform's loop -- measure, record, quantify an uncertainty -- is a *table of readings*: no time axis, one
     * its own standard uncertainty and its own provenance. A student who has taken five readings of a length has five
     * numbers and five error bars, and had no way to get them out of the session.
     *
     * The columns are
     *
     * ```
     * index,value [m],uncertainty [m],kind,source,valid
     * 0,0.01010,1.4e-06,standard,n3,true
     * 1,0.01020,,unknown,n3,true
     * 2,0.00980,,exact,hand,false
     * ```
     *
     * and every one of them is a decision rather than a decoration:
     *
     *   - **`kind` is a column, and the uncertainty of an `unknown` reading is an empty field.** The platform's central
     *     rule is that unquantified is not zero; a table that wrote `0` for both would destroy that distinction in the
     *     export step, after which every reader believes the error was measured and found to be zero. `exact` writes
     *     `0`, because that one really is zero.
     *   - **`valid` is a column, because rejection is a judgement and the store keeps it.** A dataset marks a rejected
     *     reading rather than deleting it -- "a reader could not tell an outlier that was rejected from one that was
     *     never taken" -- and an export that dropped the flag would lose exactly what that decision preserved.
     *   - **`source` is a label supplied by the caller.** The dataset records a reading's origin as a graph
     *     `(index, generation)` pair, and this module may not know what a graph is; the window resolves node names and
     *     passes them in. Rows whose label is missing are written with an empty field rather than a guess.
     *   - **`index` is the row's identity in the session**, which is what a reader needs to follow a number back.
     *
     * @param readings The dataset. Borrowed.
     * @param labels   One label per reading, or null. Borrowed.
     * @param out      Receives the table on success.
     *
     * @ownership   owns the text it appends
     * @thread      main
     * @pre         none
     * @post        On ok, `out` holds a header line and one line per reading
     * @invariant   `out` is untouched when the call is refused
     * @errors      `nothing_to_write` for an empty dataset; `shape_mismatch` for a label vector that is present, not
     *              empty, and shorter than the dataset
     * @complexity  O(readings)
     * @nondet      none
     * @frozen      no
     * @tests       csv.export.a_readings_table_carries_the_kind_and_the_source
     */
    [[nodiscard]] qp::runtime::ExportRefusal to_readings_text(const qp::runtime::Dataset& readings,
                                                              const std::vector<std::string>* labels,
                                                              std::string& out);

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
