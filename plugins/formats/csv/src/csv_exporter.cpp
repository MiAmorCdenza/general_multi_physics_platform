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

rt::ExportRefusal CsvExporter::write(const rt::ExportRequest& request) noexcept {
    // The pre-flight first, and in the caller's own terms: this is the check that refuses an uncertain
    // export before a file exists, and running it here as well is what stops a caller that skipped it
    // from publishing a table whose error bars were dropped.
    const rt::ExportRefusal ready = rt::check_export(*this, request);
    if (ready != rt::ExportRefusal::ok) return ready;

    std::string text;
    const rt::ExportRefusal rendered = to_text(*request.trace, text);
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
