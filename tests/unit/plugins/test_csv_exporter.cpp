/**
 * @file test_csv_exporter.cpp
 * @brief Tests for the CSV exporter: the table it writes, and the two ends of `check_export`.
 *
 * ## What is worth testing here
 *
 * This is the first export format that produces something a person keeps, so the cases are about the
 * three ways an export can be wrong while looking right:
 *
 *   - the **error bar is missing or zeroed** -- the table opens, the numbers are right, and the
 *     measurement has become a bare number. The platform's whole claim fails at the last step;
 *   - the **row has the wrong number of columns** -- one channel name holding a comma, and the header
 *     no longer lines up with the data. A reader sees a plausible table with shifted columns;
 *   - the **file is not what was asked for** -- a path that cannot be written, or a write that failed
 *     part-way and was reported as success.
 *
 * The pre-flight is asserted at both ends, because that is the property `check_export` exists for: the
 * answer must come **before** the file, and the file must then match the answer.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/csv/csv_exporter.hpp>

#include <qp/runtime/io.hpp>

#include <support/temp_dir.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/runtime/trace/trace.hpp>
#include <qp/units/dimensions.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace qp::plugins::csv;
using qp::test::TempDir;

namespace {

namespace rt = qp::runtime;

/// @brief A trace with the two channels a mechanics run records, and a few samples.
///
/// Built through `Trace`'s own API, so the fixture cannot produce a shape the real code never makes.
[[nodiscard]] rt::Trace sample_trace() {
    rt::Trace trace{rt::RunId{7}};
    REQUIRE(trace.add_channel(rt::Channel{"displacement", qp::units::dims::length}).has_value());
    REQUIRE(trace.add_channel(rt::Channel{"velocity", qp::units::dims::velocity}).has_value());

    // Three states of uncertainty in one table, which is the case that catches a writer that treats
    // `unknown` as `0`: sample 0 has both error bars quantified, sample 1 has none, sample 2's
    // displacement is exact.
    REQUIRE(trace.append(0.0,
                         {rt::UncertainValue::measured(0.01, 1.0e-4, qp::units::dims::length),
                          rt::UncertainValue::measured(0.0, 1.0e-5, qp::units::dims::velocity)})
                .has_value());
    REQUIRE(trace.append(1.0e-4,
                         {rt::UncertainValue::unquantified(0.0099, qp::units::dims::length),
                          rt::UncertainValue::unquantified(-0.02, qp::units::dims::velocity)})
                .has_value());
    REQUIRE(trace.append(2.0e-4,
                         {rt::UncertainValue::exact(0.0098, qp::units::dims::length),
                          rt::UncertainValue::measured(-0.04, 2.0e-5, qp::units::dims::velocity)})
                .has_value());
    return trace;
}

/// @brief Splits a rendered table into rows of fields, following the CSV quoting rule.
///
/// A reader of the format rather than a `split(',')`: the point of some cases is that a quoted field may
/// contain the separator, and a helper that split naively would report the exporter as wrong for being
/// right.
[[nodiscard]] std::vector<std::vector<std::string>> parse_rows(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            row.push_back(field);
            field.clear();
        } else if (c == '\n') {
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
        } else if (c != '\r') {
            field += c;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}


/// @brief The whole file as bytes.
[[nodiscard]] std::string read_file(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.good());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("csv.export.a_readings_table_carries_the_kind_and_the_source", "[csv][export]") {
    // **The platform's own loop ends in a report, and this is the artifact that report quotes.** A trace is a
    // series -- one row per sample, one column per channel -- while the product of measurement, record and
    // uncertainty is a table of readings: one row per measurement, no time axis, each with its own value, its own
    // standard uncertainty and its own provenance. Until this existed a student could export the trajectory and not
    // the five numbers they had written down.
    //
    // Every column is a decision, and the case asserts each one rather than the table's shape: `kind` distinguishes
    // the three states of an uncertainty, `valid` keeps a rejection visible, and `source` names the device -- which
    // the dataset cannot do itself, because it records a graph address and this module is not allowed to know what
    // a graph is.
    CsvExporter exporter;

    rt::Dataset readings{"length", qp::units::dims::length};
    // A measured reading, one nobody quantified, an exact one, and a rejected one: the whole vocabulary the store
    // has, and a table that lost any of them would be a table about a different session.
    readings.add(rt::UncertainValue::measured(0.01010, 1.4e-6, qp::units::dims::length));
    readings.add(rt::UncertainValue::unquantified(0.01020, qp::units::dims::length));
    readings.add(rt::UncertainValue::exact(0.00980, qp::units::dims::length));
    readings.add(rt::UncertainValue::measured(0.01040, 1.4e-6, qp::units::dims::length));
    REQUIRE(readings.reject(3).has_value());

    const std::vector<std::string> labels{"metre rule", "metre rule", "hand", "metre rule"};
    std::string table;
    REQUIRE(exporter.to_readings_text(readings, &labels, table) == rt::ExportRefusal::ok);

    const std::vector<std::vector<std::string>> rows = parse_rows(table);
    REQUIRE(rows.size() == 5);        // a header and four readings, and no trailing empty row
    // The header names each column and carries the unit where there is one. `kind`, `source` and `valid` are words,
    // so they get no brackets -- the same rule the trace's header follows.
    REQUIRE(rows[0].size() == 6);
    REQUIRE(rows[0][0] == "index");
    REQUIRE(rows[0][1] == "value [m]");
    REQUIRE(rows[0][2] == "uncertainty [m]");
    REQUIRE(rows[0][3] == "kind");
    REQUIRE(rows[0][4] == "source");
    REQUIRE(rows[0][5] == "valid");

    // A measured reading: its value, its own standard uncertainty, its kind, its device and its validity.
    REQUIRE(rows[1][1] == "0.0101");
    REQUIRE(rows[1][2] == "1.4e-06");
    REQUIRE(rows[1][3] == "standard");
    REQUIRE(rows[1][4] == "metre rule");
    REQUIRE(rows[1][5] == "true");
    // **The central rule, in this table too**: an unquantified uncertainty is an **empty field**, not a zero.
    REQUIRE(rows[2][2].empty());
    REQUIRE(rows[2][3] == "unknown");
    // ... and an exact one really is zero, so it is written as one. The two are different numbers and the table
    // says so.
    REQUIRE(rows[3][2] == "0");
    REQUIRE(rows[3][3] == "exact");
    // A rejected reading stays in the table with its flag: the store marks a judgement rather than deleting it,
    // because a reader could not otherwise tell an outlier that was rejected from one that was never taken. An
    // export that dropped the flag would lose exactly what that decision preserved.
    REQUIRE(rows[4][5] == "false");
    REQUIRE(rows[4][3] == "standard");
    // The index column is the reading's identity in the session, which is what a reader follows a number back by.
    REQUIRE(rows[1][0] == "0");
    REQUIRE(rows[4][0] == "3");

    // ---- The labels, which the caller resolves --------------------------------------------------------------
    //
    // With no labels the source column is **empty rather than guessed**: a reading's origin is a graph address in
    // the dataset, and this module may not know what a graph is.
    std::string unlabelled;
    REQUIRE(exporter.to_readings_text(readings, nullptr, unlabelled) == rt::ExportRefusal::ok);
    const std::vector<std::vector<std::string>> bare = parse_rows(unlabelled);
    REQUIRE(bare.size() == rows.size());
    REQUIRE(bare[1][4].empty());
    REQUIRE(bare[1][1] == rows[1][1]);
    // A label vector that is present and **short** is refused, not padded: a source column that silently empties
    // for the last rows is worse than one that was never written, because the reader trusts what is there -- and a
    // refused call must not have touched the caller's string.
    const std::vector<std::string> too_few{"metre rule"};
    std::string refused;
    REQUIRE(exporter.to_readings_text(readings, &too_few, refused) == rt::ExportRefusal::shape_mismatch);
    REQUIRE(refused.empty());

    // An empty dataset is nothing to write, which is a different finding from a shape that does not line up.
    const rt::Dataset empty{"length", qp::units::dims::length};
    std::string nothing;
    REQUIRE(exporter.to_readings_text(empty, nullptr, nothing) == rt::ExportRefusal::nothing_to_write);
    REQUIRE(nothing.empty());

    // ---- The pre-flight, which is what a dialog asks before a file exists -----------------------------------
    rt::ExportRequest request;
    request.subject = rt::ExportSubject::readings;
    request.readings = &readings;
    request.reading_labels = &labels;
    REQUIRE(rt::check_export(exporter, request) == rt::ExportRefusal::ok);
    // The capability that makes it legal, and the flag a caller reads before choosing a format.
    REQUIRE(exporter.format().capabilities.keeps_readings);
    // A subject with nothing behind it is a caller's bug and is refused **before** the format is consulted: the
    // alternative is a writer that writes an empty table and reports success.
    rt::ExportRequest missing = request;
    missing.readings = nullptr;
    REQUIRE(rt::check_export(exporter, missing) == rt::ExportRefusal::subject_missing);
    // The names are stable, because a refusal reaches a user and a log.
    REQUIRE(std::string{rt::to_string(rt::ExportRefusal::readings_not_supported)} == "readings_not_supported");
    REQUIRE(std::string{rt::to_string(rt::ExportRefusal::subject_missing)} == "subject_missing");
    REQUIRE(std::string{rt::to_string(rt::ExportSubject::readings)} == "readings");
    REQUIRE(std::string{rt::to_string(rt::ExportSubject::trace)} == "trace");

    // ---- The file, because the bytes are the artifact ------------------------------------------------------
    //
    // The string and the file must be the same table: the pre-flight runs twice, once in the caller and once inside
    // `write`, and the two must not be able to disagree about what was exported.
    const TempDir dir;
    request.path = dir.path("readings.csv");
    REQUIRE(exporter.write(request) == rt::ExportRefusal::ok);
    REQUIRE(read_file(request.path) == table);
    // ... and a request that names the readings and leaves the pointer null is refused on the file path too, rather
    // than falling back to a trace it also does not carry.
    rt::ExportRequest wrong = missing;
    wrong.path = dir.path("wrong.csv");
    REQUIRE(exporter.write(wrong) == rt::ExportRefusal::subject_missing);
}

TEST_CASE("csv.export.declares_what_it_can_carry", "[csv][export]") {
    // The declaration is what `check_export` reads, so a wrong flag is the difference between a refused
    // export and a published table whose error bars are gone. For CSV the honest answer on uncertainty is
    // **yes**: a column holds it, and a lab table is written that way by hand.
    CsvExporter exporter;
    const rt::FormatDesc& desc = exporter.format();

    REQUIRE(desc.name == "qp.csv");
    REQUIRE_FALSE(desc.label.empty());
    REQUIRE(desc.extensions.size() == 1);
    REQUIRE(desc.extensions.front() == "csv");
    REQUIRE(desc.capabilities.is_text);
    REQUIRE(desc.capabilities.keeps_uncertainty);
    REQUIRE(desc.capabilities.keeps_time);
    REQUIRE(desc.capabilities.keeps_dimension);
    REQUIRE_FALSE(desc.capabilities.multi_dataset);
    REQUIRE(exporter.is_available());

    // The same object every call: a registry holds this reference.
    REQUIRE(&exporter.format() == &desc);

    // And the format registers, is found by name and by extension with or without a leading dot. The
    // dot is stripped rather than rejected: a caller holding `".csv"` from a file dialog and one holding
    // `"csv"` from a list must both find it, and insisting on one spelling is how a "save as" silently
    // finds nothing.
    rt::FormatRegistry registry;
    REQUIRE(registry.add(&exporter).has_value());
    REQUIRE(registry.find_by_name("qp.csv") == &exporter);
    REQUIRE(registry.find_by_extension("csv") == &exporter);
    REQUIRE(registry.find_by_extension(".CSV") == &exporter);
    REQUIRE(registry.with_uncertainty() == std::vector<rt::IExporter*>{&exporter});
}

TEST_CASE("csv.export.uncertainty_is_carried_in_its_own_column", "[csv][export]") {
    // The pre-flight answers **before** the file, and the file then proves the answer. This is the loop
    // the io module exists for, run for the first time against a format that really writes something.
    CsvExporter exporter;
    const rt::Trace trace = sample_trace();

    rt::ExportRequest request;
    request.trace = &trace;
    request.require_uncertainty = true;   // the evidence-grade request: a lab report, not a glance
    REQUIRE(rt::check_export(exporter, request) == rt::ExportRefusal::ok);

    std::string text;
    REQUIRE(exporter.to_text(trace, text) == rt::ExportRefusal::ok);

    const std::vector<std::vector<std::string>> rows = parse_rows(text);
    REQUIRE(rows.size() == 4);   // one header plus three samples
    REQUIRE(rows[0].size() == 5);   // t, x, x_u, v, v_u

    // The header names the quantity and its unit, and the uncertainty column sits next to the value it
    // belongs to. A column of numbers without its unit is the commonest way a report becomes wrong.
    REQUIRE(rows[0][0] == "t [s]");
    REQUIRE(rows[0][1] == "displacement [m]");
    REQUIRE(rows[0][2] == "displacement_u [m]");
    REQUIRE(rows[0][3] == "velocity [m/s]");
    REQUIRE(rows[0][4] == "velocity_u [m/s]");

    // Every row has the header's column count. A table whose rows disagree is one a spreadsheet will
    // happily shift into plausible nonsense.
    for (const std::vector<std::string>& row : rows) {
        REQUIRE(row.size() == rows[0].size());
    }

    // Sample 0 has both quantified, and the numbers arrive.
    REQUIRE(rows[1][0] == "0");
    REQUIRE(rows[1][1] == "0.01");
    REQUIRE(rows[1][2] == "1e-04");   // the shortest form that reads back as the same double
    REQUIRE(rows[1][3] == "0");
    REQUIRE(rows[1][4] == "1e-05");
}

TEST_CASE("csv.export.unknown_is_not_zero", "[csv][export]") {
    // The platform's central distinction, in the one place it is easiest to lose: a writer that puts `0`
    // in an unquantified error bar turns "nobody measured this" into "this was measured and is exactly
    // right", and every reader of the file believes the second.
    CsvExporter exporter;
    const rt::Trace trace = sample_trace();

    std::string text;
    REQUIRE(exporter.to_text(trace, text) == rt::ExportRefusal::ok);
    const std::vector<std::vector<std::string>> rows = parse_rows(text);
    REQUIRE(rows.size() == 4);

    // Sample 1: both values unquantified.
    REQUIRE(rows[2][1] == "0.0099");
    REQUIRE(rows[2][2].empty());
    REQUIRE(rows[2][3] == "-0.02");
    REQUIRE(rows[2][4].empty());

    // Sample 2: the displacement is **exact** (genuinely zero error) while the velocity is quantified.
    // Empty and `0` are different answers, and both appear in this table.
    REQUIRE(rows[3][1] == "0.0098");
    REQUIRE(rows[3][2] == "0");
    REQUIRE(rows[3][4] == "2e-05");

    // The distinction survives a round trip through the text: an empty field means unquantified, and
    // nothing else in the table is empty.
    for (std::size_t column = 0; column < rows[0].size(); ++column) {
        for (std::size_t row = 1; row < rows.size(); ++row) {
            const bool expected_empty = (row == 2 && (column == 2 || column == 4));
            REQUIRE(rows[row][column].empty() == expected_empty);
        }
    }
}

TEST_CASE("csv.export.fields_with_separators_are_quoted", "[csv][export]") {
    // A channel name is text a user is entitled to choose, and one holding a comma or a quote would
    // break the row. Quoting is RFC 4180's rule, applied to names as well as to values.
    rt::Trace trace{rt::RunId{1}};
    REQUIRE(trace.add_channel(rt::Channel{"x,y", qp::units::dims::length}).has_value());
    REQUIRE(trace.add_channel(rt::Channel{"say \"hi\"", {}}).has_value());
    REQUIRE(trace.append(0.0, {rt::UncertainValue::measured(1.0, 0.1, qp::units::dims::length),
                               rt::UncertainValue::exact(2.0)})
                .has_value());

    CsvExporter exporter;
    std::string text;
    REQUIRE(exporter.to_text(trace, text) == rt::ExportRefusal::ok);

    // The raw bytes carry the quoting...
    REQUIRE(text.find("\"x,y [m]\"") != std::string::npos);
    REQUIRE(text.find("\"say \"\"hi\"\"\"") != std::string::npos);

    // ...and a reader that follows the rule recovers the names exactly, with the header still lining up
    // with the data: five columns, one row.
    const std::vector<std::vector<std::string>> rows = parse_rows(text);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].size() == 5);
    REQUIRE(rows[1].size() == 5);
    REQUIRE(rows[0][1] == "x,y [m]");
    REQUIRE(rows[0][2] == "x,y_u [m]");
    REQUIRE(rows[0][3] == "say \"hi\"");
    REQUIRE(rows[0][4] == "say \"hi\"_u");
    REQUIRE(rows[1][1] == "1");
}

TEST_CASE("csv.export.a_diverged_sample_is_written", "[csv][export]") {
    // A run that diverged is what the user needs to see, so an infinity or a NaN is written rather than
    // dropped. An empty field would make a diverged run indistinguishable from a gap in the data, and
    // the confidence panel's whole job is to make the difference visible.
    rt::Trace trace{rt::RunId{2}};
    REQUIRE(trace.add_channel(rt::Channel{"x", qp::units::dims::length}).has_value());
    REQUIRE(trace.append(0.0, {rt::UncertainValue::unquantified(1.0, qp::units::dims::length)})
                .has_value());
    REQUIRE(trace.append(1.0, {rt::UncertainValue::unquantified(
                                  std::numeric_limits<double>::infinity(), qp::units::dims::length)})
                .has_value());
    REQUIRE(trace.append(2.0, {rt::UncertainValue::unquantified(-std::numeric_limits<double>::infinity(),
                                                               qp::units::dims::length)})
                .has_value());
    REQUIRE(trace.append(3.0, {rt::UncertainValue::unquantified(
                                  std::numeric_limits<double>::quiet_NaN(), qp::units::dims::length)})
                .has_value());

    CsvExporter exporter;
    std::string text;
    REQUIRE(exporter.to_text(trace, text) == rt::ExportRefusal::ok);

    const std::vector<std::vector<std::string>> rows = parse_rows(text);
    REQUIRE(rows.size() == 5);
    REQUIRE(rows[1][1] == "1");
    REQUIRE(rows[2][1] == "inf");
    REQUIRE(rows[3][1] == "-inf");
    REQUIRE(rows[4][1] == "nan");
    // The `_u` column of a diverged sample is still empty rather than `0`: the value is infinite, the
    // error was never quantified, and the two facts are independent.
    REQUIRE(rows[2][2].empty());
}

TEST_CASE("csv.export.byte_order_mark_only_when_it_helps", "[csv][export]") {
    // A byte-order mark is written exactly when the table holds a byte above ASCII -- which is when a
    // reader that guesses the encoding guesses wrong (Excel on a Chinese Windows reads a BOM-less file as
    // ANSI). An all-ASCII table needs no help and stays byte-clean, so the three bytes never appear as
    // noise.
    CsvExporter exporter;

    const rt::Trace ascii = sample_trace();
    std::string plain;
    REQUIRE(exporter.to_text(ascii, plain) == rt::ExportRefusal::ok);
    REQUIRE(plain.rfind(CsvExporter::kByteOrderMark, 0) != 0);
    REQUIRE(plain.substr(0, 3) == "t [");

    // A channel named with two Chinese characters, written as bytes rather than as a literal: the case
    // is about **bytes above ASCII**, so the test should say which bytes it means instead of depending on
    // how the compiler reads this file. U+4F4D U+79FB.
    static constexpr const char* kNonAsciiName = "\xE4\xBD\x8D\xE7\xA7\xBB";
    rt::Trace named{rt::RunId{3}};
    REQUIRE(named.add_channel(rt::Channel{kNonAsciiName, qp::units::dims::length}).has_value());
    REQUIRE(named.append(0.0, {rt::UncertainValue::unquantified(1.0, qp::units::dims::length)})
                .has_value());
    std::string marked;
    REQUIRE(exporter.to_text(named, marked) == rt::ExportRefusal::ok);
    REQUIRE(marked.rfind(CsvExporter::kByteOrderMark, 0) == 0);
    // The mark is a prefix, not a column: a reader that strips it -- or one that was told the file is
    // UTF-8 -- sees the same header it would otherwise, with the name's bytes intact.
    REQUIRE(marked.substr(3).rfind(std::string{"t [s],"} + kNonAsciiName + " [m]", 0) == 0);
}

TEST_CASE("csv.export.writes_a_file_a_reader_can_open", "[csv][export]") {
    // The end of the loop: the pre-flight's answer, then a file, then the file's contents read back by
    // an ordinary reader. Anything less leaves the panel's promise ("this format keeps your
    // uncertainties") unverified at the point where it matters.
    const TempDir dir;
    CsvExporter exporter;
    const rt::Trace trace = sample_trace();
    const std::string path = dir.path("trace.csv");

    rt::ExportRequest request;
    request.trace = &trace;
    request.path = path;
    request.require_uncertainty = true;
    REQUIRE(rt::check_export(exporter, request) == rt::ExportRefusal::ok);
    REQUIRE(exporter.write(request) == rt::ExportRefusal::ok);

    // Written by `write`, and identical to what `to_text` produces: the file and the in-memory table are
    // one answer, not two.
    std::string expected;
    REQUIRE(exporter.to_text(trace, expected) == rt::ExportRefusal::ok);
    REQUIRE(read_file(path) == expected);

    // And the uncertainty survived the trip to disk, in the column next to its value.
    REQUIRE(expected.find("displacement_u [m]") != std::string::npos);
    REQUIRE(expected.find("1e-04") != std::string::npos);

    // Writing again replaces rather than appends: a second export is not a second table.
    REQUIRE(exporter.write(request) == rt::ExportRefusal::ok);
    REQUIRE(read_file(path) == expected);
}

TEST_CASE("csv.export.refuses_a_destination_it_cannot_write", "[csv][export]") {
    // The refusal that the first real exporter needed and the enum did not have. Reporting success on a
    // failed write is the one outcome that is worse than refusing: the user believes they have their
    // data. Each of these paths is one a file dialog can produce.
    const TempDir dir;
    CsvExporter exporter;
    const rt::Trace trace = sample_trace();

    rt::ExportRequest request;
    request.trace = &trace;
    request.path = dir.path("no_such_directory") + "/trace.csv";
    REQUIRE(exporter.write(request) == rt::ExportRefusal::could_not_write);

    // A path that names an existing directory rather than a file.
    request.path = dir.path(".");
    REQUIRE(exporter.write(request) == rt::ExportRefusal::could_not_write);

    // An empty path: no file is named, so nothing is written rather than something surprising.
    request.path.clear();
    REQUIRE(exporter.write(request) == rt::ExportRefusal::could_not_write);

    // A refusal before the filesystem is touched: a null trace never reaches a path.
    request.trace = nullptr;
    request.path = dir.path("never.csv");
    REQUIRE(exporter.write(request) == rt::ExportRefusal::nothing_to_write);
    REQUIRE_FALSE(std::filesystem::exists(request.path));
}

TEST_CASE("csv.export.refuses_a_trace_with_nothing_in_it", "[csv][export]") {
    // Both ends answer the same way, and the answer comes before any bytes exist. A file holding only a
    // header would look like an experiment that produced nothing, which is a different story from "there
    // was nothing to export".
    CsvExporter exporter;
    rt::Trace empty{rt::RunId{9}};
    REQUIRE(empty.add_channel(rt::Channel{"x", qp::units::dims::length}).has_value());

    std::string text = "stale";
    REQUIRE(exporter.to_text(empty, text) == rt::ExportRefusal::nothing_to_write);
    REQUIRE(text.empty());

    // A trace with no channels at all is refused by the same check for the same reason.
    rt::Trace bare{rt::RunId{10}};
    REQUIRE(exporter.to_text(bare, text) == rt::ExportRefusal::nothing_to_write);
    REQUIRE(text.empty());

    // `shape_mismatch` -- a trace whose samples disagree with its channels -- is **not** asserted here,
    // and the reason is worth recording: `Trace::append` refuses a sample whose width does not match the
    // channel count, so a trace in that state cannot be built through the public API. The check in
    // `check_export` guards a state only a corrupt load could produce; a test that forced it would have to
    // reach past the type that exists to prevent it.
}
