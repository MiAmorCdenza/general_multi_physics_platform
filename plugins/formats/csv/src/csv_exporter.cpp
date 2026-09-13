/**
 * @file csv_exporter.cpp
 * @brief The CSV writer: a table, its header, and the two rules that make it a measurement.
 *
 * The rendering is a pure function of the trace and the file writing is a thin shell around it, which is
 * what lets every property worth asserting -- the error bar's column, `unknown` written as nothing,
 * quoting, non-finite values, the byte-order mark -- be asserted on a string rather than on a temporary
 * file. The file path adds exactly two things the string does not have: `check_export`, run first, and
 * an honest answer when the disk refuses.
 */
#include <qp/plugins/csv/csv_exporter.hpp>

#include <qp/runtime/file/file.hpp>
#include <qp/units/unit_symbol.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <system_error>

namespace qp::plugins::csv {
namespace {

namespace rt = qp::runtime;

/// @brief Appends one number in the shortest form that reads back identically.
///
/// Shortest round-tripping rather than a fixed precision, for the same reason the document format uses
/// it: `0.1` stays `0.1` and the value a reader parses is bit-identical, so a table can be checked
/// against the trace it came from.
///
/// Infinities and NaNs are written as `inf`, `-inf` and `nan`. This is the one place CSV differs from
/// `qpjson`, and deliberately: the document format refuses a non-finite parameter because a document is
/// a record of a configuration, while a trace is a record of what happened -- and a diverged run is
/// exactly what the user needs to see. An empty field would make it look like a gap in the data.
void append_number(std::string& out, double v) {
    if (std::isnan(v)) {
        out += "nan";
        return;
    }
    if (std::isinf(v)) {
        out += v < 0.0 ? "-inf" : "inf";
        return;
    }
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    if (r.ec == std::errc{}) {
        out.append(buf, r.ptr);
    } else {
        // Unreachable: 40 bytes holds the longest shortest-round-trip form of a double with room to
        // spare. A defined answer beats an empty field, which a reader would take for a missing sample.
        out += "nan";
    }
}

/// @brief Appends one field, quoting it when the separator, a quote or a line break would break the row.
///
/// RFC 4180's rule, applied to the header and to text fields alike. A channel named `x,y` is a channel
/// name a user is entitled to type, and an exporter that wrote it unquoted would produce a table with
/// one more column in the header than in the data -- a file that opens, looks plausible, and is wrong.
void append_field(std::string& out, std::string_view field) {
    const bool needs_quotes = field.find_first_of(",\"\n\r") != std::string_view::npos;
    if (!needs_quotes) {
        out += field;
        return;
    }
    out += '"';
    for (const char c : field) {
        if (c == '"') out += '"';   // doubled, which is how a quote is escaped
        out += c;
    }
    out += '"';
}

/// @brief The column header for a quantity: its name, and its unit when it has one.
///
/// The unit goes in brackets because a column of numbers without one is the commonest way a lab report
/// becomes wrong -- and the trace carries the dimension precisely so that it need not be lost here. A
/// dimensionless quantity gets no brackets rather than `[1]`, which is noise.
[[nodiscard]] std::string column_header(std::string_view name, qp::units::Dim dim) {
    std::string out{name};
    if (!dim.is_dimensionless()) {
        out += " [";
        out += qp::units::unit_symbol(dim);
        out += ']';
    }
    return out;
}

/// @brief Appends the uncertainty field of one value.
///
/// The three states are the platform's central distinction, and this is where it would be lost:
/// an unquantified uncertainty is **nothing** (an empty field), an exact one is `0`, and a quantified
/// one is its standard uncertainty. Writing `0` for the first would turn "nobody measured the error"
/// into "the error was measured and is zero", and every reader of the file would believe it.
void append_uncertainty(std::string& out, const rt::UncertainValue& value) {
    switch (value.kind) {
        case rt::UncertaintyKind::unknown:
            break;
        case rt::UncertaintyKind::exact:
            out += '0';
            break;
        case rt::UncertaintyKind::standard:
            append_number(out, value.u);
            break;
    }
}

/// @brief Renders the whole table. `trace` has already passed `check_export`.
void render(const rt::Trace& trace, std::string& out) {
    // -- the header ----------------------------------------------------------
    out += "t [s]";
    for (const rt::Channel& channel : trace.channels()) {
        out += CsvExporter::kSeparator;
        append_field(out, column_header(channel.name, channel.dim));
        out += CsvExporter::kSeparator;
        append_field(out, column_header(std::string{channel.name} + CsvExporter::kUncertaintySuffix,
                                        channel.dim));
    }
    out += '\n';

    // -- the samples ---------------------------------------------------------
    //
    // One row per sample, in the trace's own order, with no sorting or resampling: the file is a
    // transcript of the run, and a reader that wants it smoothed can do that with the numbers in hand.
    for (const rt::Sample& sample : trace.samples()) {
        append_number(out, sample.t);
        for (const rt::UncertainValue& value : sample.values) {
            out += CsvExporter::kSeparator;
            append_number(out, value.value);
            out += CsvExporter::kSeparator;
            append_uncertainty(out, value);
        }
        out += '\n';
    }
}

/// @brief Whether any byte of `text` is above ASCII.
///
/// The condition for writing a byte-order mark: a table of ASCII is read correctly by every encoding,
/// while a table holding anything else needs a reader that knows it is UTF-8 -- and the mark is how a
/// reader that would otherwise guess ANSI (Excel on a Chinese Windows, typically) is told.
[[nodiscard]] bool needs_byte_order_mark(const std::string& text) noexcept {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) > 0x7F) return true;
    }
    return false;
}

}  // namespace

const rt::FormatDesc& CsvExporter::format() const noexcept {
    static const rt::FormatDesc desc = [] {
        rt::FormatDesc d;
        d.name = "qp.csv";
        d.label = "Comma-separated table (CSV)";
        d.extensions = {"csv"};
        // What it can carry, stated rather than assumed -- `check_export` reads these, and a wrong
        // `keeps_uncertainty` is the difference between a refused export and a published table whose
        // error bars are gone.
        d.capabilities.keeps_uncertainty = true;
        d.capabilities.keeps_time = true;
        d.capabilities.is_text = true;
        d.capabilities.keeps_dimension = true;
    // ... and a readings table, which is the other table this platform produces and the one a lab report quotes.
    // Declared here so a caller can ask before choosing a format rather than discovering it when the write is
    // refused -- the argument the uncertainty flag already carries.
    d.capabilities.keeps_readings = true;
    // ... and a parameter table, which is the same CSV shape with different columns: one row per quantity, its unit
    // implied by the model rather than carried in a cell.
    d.capabilities.keeps_fit = true;
        // One table per file. Two traces in one CSV would need a second header row in the middle, which
        // readers do not agree on and which a spreadsheet shows as data.
        d.capabilities.multi_dataset = false;
        return d;
    }();
    return desc;
}

rt::ExportRefusal CsvExporter::to_text(const rt::Trace& trace, std::string& out) const noexcept {
    out.clear();

    // The pre-flight, not a second opinion about it. A caller that renders without writing and a caller
    // that writes must never disagree about whether a trace is exportable, so both ask the module that
    // owns the question.
    rt::ExportRequest request;
    request.trace = &trace;
    const rt::ExportRefusal ready = rt::check_export(*this, request);
    if (ready != rt::ExportRefusal::ok) return ready;

    std::string rendered;
    // A column count per channel pair plus the time, so the growth does not reallocate per field.
    rendered.reserve(64 + trace.samples().size() * (8 + 24 * trace.channel_count() * 2));
    render(trace, rendered);
    if (needs_byte_order_mark(rendered)) {
        out += kByteOrderMark;
    }
    out += rendered;
    return rt::ExportRefusal::ok;
}

rt::ExportRefusal CsvExporter::to_fit_text(const rt::FitResult& fit, const std::vector<std::string>* labels,
                                           std::string& out) {
    if (fit.coefficients.empty()) return rt::ExportRefusal::nothing_to_write;
    // Present and short is a caller's bug, exactly as in the readings table: a `parameter` column that stops naming
    // its rows halfway down is worse than one that never named them.
    if (labels != nullptr && !labels->empty() && labels->size() < fit.coefficients.size()) {
        return rt::ExportRefusal::shape_mismatch;
    }

    // One row shape for everything the fit produced. The model is repeated on every row because this format has
    // nowhere else to put it -- `#` comment lines are refused by contract, since they would turn the first row of
    // every reader's table into data -- and a file that does not say which model produced `a` and `b` is a file
    // whose numbers cannot be checked.
    const std::string model = fit.model.empty() ? std::string{"unnamed"} : fit.model;
    std::string table;
    append_field(table, "model");
    table += ',';
    append_field(table, "parameter");
    table += ',';
    append_field(table, "value");
    table += ',';
    append_field(table, "uncertainty");
    table += '\n';

    const auto row = [&](std::string_view parameter, const std::optional<double>& value,
                         const std::optional<double>& uncertainty) {
        append_field(table, model);
        table += ',';
        append_field(table, parameter);
        table += ',';
        if (value.has_value()) {
            append_number(table, *value);
        } else {
            append_field(table, "");       // nothing to write is an empty field, never a zero
        }
        table += ',';
        if (uncertainty.has_value()) {
            append_number(table, *uncertainty);
        } else {
            append_field(table, "");
        }
        table += '\n';
    };

    for (std::size_t index = 0; index < fit.coefficients.size(); ++index) {
        // The label is the caller's, because the panel already names these rows: inventing `k0`, `k1` here would
        // print a table that does not match the window it came from. Unlabelled rows get an empty field.
        const std::string name = (labels != nullptr && index < labels->size()) ? (*labels)[index] : std::string{};
        row(name, fit.coefficients[index], fit.coefficient_uncertainty(index));
    }

    // The fit's own numbers, in the same shape. Their uncertainties are **absent rather than zero**: nobody
    // quantified the error of a chi-squared, and the platform's central rule is that unquantified is not zero.
    row("chi_squared", fit.chi_squared, std::nullopt);
    // Written as it stands, including the `-1` that means "not applicable": a reader has to be able to see that the
    // fit did not define this, and `0` would say something else.
    row("degrees_of_freedom", static_cast<double>(fit.degrees_of_freedom), std::nullopt);
    // Absent when the fit cannot define one -- every ordinate the same -- and `0` would read as "the model explains
    // nothing", which is a different statement from "undefined".
    row("r_squared", fit.r_squared, std::nullopt);

    const bool needs_mark = std::any_of(table.begin(), table.end(),
                                        [](unsigned char c) { return c >= 0x80; });
    out.clear();
    if (needs_mark) out += kByteOrderMark;
    out += table;
    return rt::ExportRefusal::ok;
}

rt::ExportRefusal CsvExporter::to_readings_text(const rt::Dataset& readings, const std::vector<std::string>* labels,
                                                 std::string& out) {
    if (readings.readings().empty()) return rt::ExportRefusal::nothing_to_write;
    // **A label vector that is present and short is a caller's bug**, not a table with a hole in it: a source column
    // that empties for the last rows is worse than one that was never written, because the reader trusts what is
    // there. So it is refused, with the same code the trace uses for a shape that does not line up.
    if (labels != nullptr && !labels->empty() && labels->size() < readings.readings().size()) {
        return rt::ExportRefusal::shape_mismatch;
    }

    const qp::units::Dim dim = readings.dim();
    std::string table;
    // The same header convention as the trace: a name and its unit in brackets, and no brackets for a
    // dimensionless quantity. `kind` and `valid` carry no unit -- they are words, not quantities -- and the source
    // column is a name.
    append_field(table, "index");
    table += ',';
    append_field(table, column_header("value", dim));
    table += ',';
    append_field(table, column_header("uncertainty", dim));
    table += ',';
    append_field(table, "kind");
    table += ',';
    append_field(table, "source");
    table += ',';
    append_field(table, "valid");
    table += '\n';

    const auto format_number = [](double value, std::string& target) {
        // The same shortest-round-trip rule the trace uses, through the same helper: a reading that read back as a
        // different number than it was taken as would make the table unverifiable against the session.
        append_number(target, value);
    };

    const std::vector<rt::Measurement>& rows = readings.readings();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const rt::Measurement& row = rows[index];
        append_number(table, static_cast<double>(index));
        table += ',';
        format_number(row.reading.value, table);
        table += ',';
        // The platform's central distinction, in the same place and by the same rule as the trace's uncertainty
        // column: `unknown` is an empty field, `exact` is `0`, everything else is its standard uncertainty.
        append_uncertainty(table, row.reading);
        table += ',';
        append_field(table, rt::to_string(row.reading.kind));
        table += ',';
        if (labels != nullptr && index < labels->size()) {
            append_field(table, (*labels)[index]);
        } else {
            // No label: an empty field rather than a guess. The reading's own source is a graph address, and this
            // module does not know what a graph is -- inventing text here would be a provenance that cannot be
            // followed back.
            append_field(table, "");
        }
        table += ',';
        append_field(table, row.valid ? "true" : "false");
        table += '\n';
    }

    // The byte-order mark only when the table holds a byte above ASCII -- the same rule and the same reason as the
    // trace's, and here the only thing that can carry one is a source label.
    const bool needs_mark = std::any_of(table.begin(), table.end(),
                                        [](unsigned char c) { return c >= 0x80; });
    out.clear();
    if (needs_mark) out += "\xEF\xBB\xBF";
    out += table;
    return rt::ExportRefusal::ok;
}

rt::ExportRefusal CsvExporter::write(const rt::ExportRequest& request) noexcept {
    // The pre-flight first, and in the caller's own terms: this is the check that refuses an uncertain
    // export before a file exists, and running it here as well is what stops a caller that skipped it
    // from publishing a table whose error bars were dropped.
    const rt::ExportRefusal ready = rt::check_export(*this, request);
    if (ready != rt::ExportRefusal::ok) return ready;

    std::string text;
    const rt::ExportRefusal rendered =
        request.subject == rt::ExportSubject::readings
            ? to_readings_text(*request.readings, request.reading_labels, text)
            : request.subject == rt::ExportSubject::fit
                  ? to_fit_text(*request.fit, request.coefficient_labels, text)
                  : to_text(*request.trace, text);
    if (rendered != rt::ExportRefusal::ok) return rendered;

    // The bytes reach the disk through `runtime/file`, which is the one place that knows how a UTF-8 path
    // crosses into the platform's own open call -- the conversion a trace exporter has no business
    // reimplementing, and the reason that module exists. The outcome is translated rather than returned:
    // `ExportRefusal` is this interface's vocabulary, and a caller reading a refusal should not have to
    // know about a second enum to understand a failed export.
    const rt::FileOutcome written = rt::write_whole_file(request.path, text);
    return written == rt::FileOutcome::ok ? rt::ExportRefusal::ok : rt::ExportRefusal::could_not_write;
}

}  // namespace qp::plugins::csv
