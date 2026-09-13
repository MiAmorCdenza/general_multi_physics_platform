/**
 * @file test_export_controller.cpp
 * @brief Tests for the export path: ask first, write second, and report what happened.
 *
 * ## What is worth testing here
 *
 * The export is the last step of the platform's closed loop -- measure, record, uncertainty, report -- so the failure
 * that matters is the quiet one: a file that is written, opens, looks right, and has lost the error bars.
 * `check_export` exists to refuse that **before** anything touches the filesystem, and what is asserted
 * here is that this path really goes through it:
 *
 *   - a format that cannot carry the uncertainty is refused, and **no file is created**;
 *   - the refusal is the one the panel's own readiness check gives, because both ask the model's request;
 *   - a writable format writes a file whose uncertainty columns are actually there;
 *   - a destination that cannot be written is reported rather than counted as a success.
 *
 * Both formats are exercised through the same function the window calls, so nothing here depends on the
 * window existing -- which is the point of the split.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/export_controller.hpp>
#include <qp/views/model/fit_session.hpp>

#include <qp/plugins/csv/csv_exporter.hpp>

#include <qp/runtime/file/file.hpp>

#include <support/temp_dir.hpp>
#include <qp/units/dimensions.hpp>
#include <qp/runtime/io.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace qp::views::model;
using qp::test::TempDir;

namespace {

namespace rt = qp::runtime;


/// @brief A format that declares it cannot keep an uncertainty: the lossy case, as a real exporter.
///
/// It writes nothing, and that is deliberate: the assertion that matters is not what such a format does
/// with a file, it is that the path **never reaches it**.
class LossyExporter final : public rt::IExporter {
public:
    [[nodiscard]] const rt::FormatDesc& format() const noexcept override {
        static const rt::FormatDesc desc = [] {
            rt::FormatDesc d;
            d.name = "test.lossy";
            d.label = "Bare numbers";
            d.extensions = {"lossy"};
            d.capabilities.keeps_uncertainty = false;
            d.capabilities.keeps_time = true;
            d.capabilities.is_text = true;
            return d;
        }();
        return desc;
    }

    [[nodiscard]] rt::ExportRefusal write(const rt::ExportRequest& request) noexcept override {
        ++write_calls;
        (void)request;
        // A format that says it cannot keep the uncertainty and is asked to anyway must refuse rather than
        // write a bare table -- the io module's contract says the refusals the pre-flight would report are
        // reported here too, and this is what makes "called directly" safe.
        const bool needed = request.require_uncertainty;
        return needed ? rt::ExportRefusal::uncertainty_not_supported : rt::ExportRefusal::ok;
    }

    [[nodiscard]] bool is_available() const noexcept override { return true; }

    mutable int write_calls = 0;
};

/// @brief The real CSV exporter, one instance for the whole file.
[[nodiscard]] qp::plugins::csv::CsvExporter& csv_exporter() {
    static qp::plugins::csv::CsvExporter exporter;
    return exporter;
}

/// @brief A measurement session with a quantified reading and a short trace.
///
/// The quantified reading is what makes the model require the uncertainty: a format that drops it is then
/// refused rather than silently writing bare numbers.
[[nodiscard]] rt::RunLedger& ledger() {
    static rt::RunLedger instance;
    return instance;
}

struct Session final {
    MeasurementModel model{ledger(), "length", qp::units::dims::length};

    Session() {
        model.add_reading(1.0, rt::UncertaintyKind::standard, 0.05);
        REQUIRE(model.add_channel("x", qp::units::dims::length).has_value());
        REQUIRE(model.add_sample(0.0, std::vector<double>{1.0}, 0.05).has_value());
        REQUIRE(model.add_sample(0.01, std::vector<double>{1.1}, 0.05).has_value());
    }
};

}  // namespace

TEST_CASE("export.formats_are_mounted_once", "[export]") {
    // The registry is the application's, and the view layer only offers what is in it. Registering the same
    // exporter twice would make a "save as" list show one format twice and -- worse -- make which writer
    // runs depend on load order for an extension collision.
    rt::FormatRegistry& registry = export_formats();
    REQUIRE(&export_formats() == &registry);

    const std::size_t before = registry.size();
    REQUIRE(mount_export_format(&csv_exporter()).has_value());
    REQUIRE(registry.size() == before + 1);
    REQUIRE(registry.find_by_name("qp.csv") == &csv_exporter());
    REQUIRE(registry.find_by_extension("csv") == &csv_exporter());

    // Mounting it again is refused by the registry rather than silently replacing the entry: two formats
    // under one name would make the same document produce different files on two machines.
    REQUIRE_FALSE(mount_export_format(&csv_exporter()).has_value());
    REQUIRE(registry.size() == before + 1);

    // A null exporter is refused rather than stored.
    REQUIRE_FALSE(mount_export_format(nullptr).has_value());
    REQUIRE(registry.size() == before + 1);

    // The CSV format declares it keeps the uncertainty, so it is in the list a "report-quality" export
    // would offer -- the list is computed from the declaration, so a format that lied would appear here
    // and fail later.
    const std::vector<rt::IExporter*> with_uncertainty = registry.with_uncertainty();
    REQUIRE(std::find(with_uncertainty.begin(), with_uncertainty.end(), &csv_exporter()) !=
            with_uncertainty.end());
}

TEST_CASE("export.writes_the_panels_trace", "[export]") {
    // The loop, end to end: the pre-flight answers, the file is written, and the error bars are in it.
    const TempDir dir;
    const std::string path = dir.path("trace.csv");
    const Session session;

    // The panel's own readiness check and the export's pre-flight are the same question.
    REQUIRE(session.model.export_readiness(csv_exporter()) == rt::ExportRefusal::ok);

    const ExportReport report = export_trace(session.model, csv_exporter(), path);
    REQUIRE(report.ok);
    REQUIRE(report.refusal == rt::ExportRefusal::ok);
    REQUIRE(report.format_name == "qp.csv");
    REQUIRE(report.path == path);
    REQUIRE(report.message.find(path) != std::string::npos);
    REQUIRE(report.message.find("2 samples") != std::string::npos);

    std::string contents;
    REQUIRE(rt::read_whole_file(path, contents) == rt::FileOutcome::ok);
    REQUIRE(contents.find("x [m]") != std::string::npos);
    REQUIRE(contents.find("x_u [m]") != std::string::npos);
    REQUIRE(contents.find("0.05") != std::string::npos);
}

TEST_CASE("export.a_readings_table_is_written_with_its_sources", "[export]") {
    // The loop's last link, end to end: the pre-flight answers, the file is written, and what is in it is the
    // readings -- each with its uncertainty, its kind and the device it came from. The trace's case asserts the
    // series; this one asserts the table a lab report quotes, and the two are different artifacts of one session.
    const TempDir dir;
    const std::string path = dir.path("readings.csv");
    const Session session;

    // The panel's readiness check and the export's pre-flight are the same question here too.
    REQUIRE(session.model.readings_readiness(csv_exporter()) == rt::ExportRefusal::ok);

    // The labels are the caller's: this layer has the graph, `runtime/io` may not, and a reading whose source does
    // not resolve gets an empty field rather than a guess.
    const std::vector<std::string> labels{"metre rule", "metre rule", ""};
    const ExportReport report = export_readings(session.model, csv_exporter(), labels, path);
    REQUIRE(report.ok);
    REQUIRE(report.refusal == rt::ExportRefusal::ok);
    REQUIRE(report.format_name == "qp.csv");
    REQUIRE(report.path == path);
    REQUIRE(report.message.find("readings") != std::string::npos);
    REQUIRE(report.message.find(path) != std::string::npos);

    std::string contents;
    REQUIRE(rt::read_whole_file(path, contents) == rt::FileOutcome::ok);
    REQUIRE(contents.find("value [m]") != std::string::npos);
    REQUIRE(contents.find("uncertainty [m]") != std::string::npos);
    REQUIRE(contents.find("kind") != std::string::npos);
    REQUIRE(contents.find("metre rule") != std::string::npos);
    REQUIRE(contents.find("standard") != std::string::npos);
    // The header is the table's shape, and the reading rows are under it: three readings in this session's fixture.
    const std::size_t header_end = contents.find('\n');
    REQUIRE(header_end != std::string::npos);
    const std::size_t first_row = header_end + 1;
    REQUIRE(contents.compare(first_row, 2, "0,") == 0);

    // A label vector that is **short** is a caller's bug, and the pre-flight refuses it before a file exists -- the
    // property that keeps a source column from silently emptying for the last rows. The case owns the shape it
    // asserts: the fixture's session holds one reading, and one label is not short against one reading, so a session
    // with three is built here rather than assuming the fixture's size.
    Session fuller;
    fuller.model.add_reading(2.0, rt::UncertaintyKind::standard, 0.05);
    fuller.model.add_reading(3.0, rt::UncertaintyKind::exact, 0.0);
    REQUIRE(fuller.model.dataset().readings().size() == 3);

    const std::vector<std::string> one_label{"metre rule"};
    REQUIRE(rt::check_export(csv_exporter(), fuller.model.readings_export_request("x", &one_label)) ==
            rt::ExportRefusal::shape_mismatch);
    // ... and a vector of the right length, or none at all, is not refused: the labels are optional, so "nobody
    // named the rows" is a legitimate request and only a *partial* naming is a bug.
    REQUIRE(rt::check_export(csv_exporter(), fuller.model.readings_export_request("x", nullptr)) ==
            rt::ExportRefusal::ok);
    const std::vector<std::string> all_three{"a", "b", "c"};
    REQUIRE(rt::check_export(csv_exporter(), fuller.model.readings_export_request("x", &all_three)) ==
            rt::ExportRefusal::ok);

    const ExportReport refused = export_readings(fuller.model, csv_exporter(), one_label, dir.path("no.csv"));
    REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.refusal == rt::ExportRefusal::shape_mismatch);
    // The sentence changed when the refusal taxonomy became orthogonal: `shape_mismatch` now says what actually
    // disagrees -- the table and its labels -- rather than "the samples and channels", which was the only case it had
    // when it was written.
    REQUIRE(refused.message.find("line up") != std::string::npos);
    // No file, because the refusal came first: the whole point of a pre-flight rather than an error after a write.
    std::string absent;
    REQUIRE(rt::read_whole_file(dir.path("no.csv"), absent) != rt::FileOutcome::ok);
}

TEST_CASE("export.a_fit_table_leaves_the_window", "[export]") {
    // The third export path, end to end: a request built from a fit the **panel** produced, the pre-flight, and the
    // file. What makes it worth a case of its own is the policy: a fit always requires the uncertainty, and the
    // request says so unconditionally -- for the readings the flag depends on what the session quantified, and here
    // it cannot, because a coefficient without its error bar is not a weaker result but a different one.
    const TempDir dir;
    const std::string path = dir.path("fit.csv");

    rt::FitResult fit;
    fit.model = "linear";
    fit.coefficients = {0.0101, 0.512};
    fit.covariance = {1.0201e-8, 0.0, 0.0, 9.0e-6};
    fit.chi_squared = 1.2e-4;
    fit.degrees_of_freedom = 7;
    fit.r_squared = 0.9998;

    const std::vector<std::string> labels{fit_coefficient_name(0), fit_coefficient_name(1)};
    const rt::ExportRequest request = fit_export_request(fit, path, &labels);
    REQUIRE(request.subject == rt::ExportSubject::fit);
    REQUIRE(request.fit == &fit);
    REQUIRE(request.coefficient_labels == &labels);
    REQUIRE(request.path == path);
    // **Unconditional**, and the property this case exists for.
    REQUIRE(request.require_uncertainty);

    // The readiness question and the export's pre-flight are the same question.
    REQUIRE(fit_readiness(csv_exporter(), fit) == rt::ExportRefusal::ok);

    const ExportReport report = export_fit(fit, csv_exporter(), labels, path);
    REQUIRE(report.ok);
    REQUIRE(report.refusal == rt::ExportRefusal::ok);
    REQUIRE(report.format_name == "qp.csv");
    REQUIRE(report.path == path);
    REQUIRE(report.message.find("2 fitted parameters") != std::string::npos);
    REQUIRE(report.message.find(path) != std::string::npos);

    // The file: the coefficients by the names the window shows, each with its uncertainty, and the quality numbers
    // with an empty uncertainty field.
    std::string contents;
    REQUIRE(rt::read_whole_file(path, contents) == rt::FileOutcome::ok);
    REQUIRE(contents.find("model,parameter,value,uncertainty") != std::string::npos);
    REQUIRE(contents.find("linear,a,0.0101,") != std::string::npos);
    REQUIRE(contents.find("linear,b,0.512,0.003") != std::string::npos);
    REQUIRE(contents.find("linear,chi_squared,0.00012,\n") != std::string::npos);
    REQUIRE(contents.find("linear,degrees_of_freedom,7,\n") != std::string::npos);
    REQUIRE(contents.find("linear,r_squared,0.9998,\n") != std::string::npos);

    // Nothing fitted is nothing to write, and the request says so by carrying no fit at all: a caller that hands the
    // pre-flight an empty subject is refused rather than written an empty file for.
    const rt::ExportRequest missing = fit_export_request(fit, path, &labels);
    rt::ExportRequest no_fit = missing;
    no_fit.fit = nullptr;
    REQUIRE(rt::check_export(csv_exporter(), no_fit) == rt::ExportRefusal::subject_missing);
    // A fit with no coefficients is a different finding again.
    const rt::FitResult empty;
    REQUIRE(rt::check_export(csv_exporter(), fit_export_request(empty, path, nullptr)) ==
            rt::ExportRefusal::nothing_to_write);

    // And the sentence a user sees names the table, because one refusal code covers every table a format cannot
    // write: "this format cannot write a table of fitted parameters" is actionable, "cannot write that" is not.
    const std::string sentence = describe_export_refusal(rt::ExportRefusal::subject_not_supported,
                                                         rt::ExportSubject::fit);
    REQUIRE(sentence.find("fitted parameters") != std::string::npos);
    const std::string readings_sentence = describe_export_refusal(rt::ExportRefusal::subject_not_supported,
                                                                  rt::ExportSubject::readings);
    REQUIRE(readings_sentence.find("readings") != std::string::npos);
    REQUIRE(readings_sentence != sentence);
}

TEST_CASE("export.refuses_before_writing", "[export]") {
    // The refusal comes first, and nothing else happens: no file, no write call, no success reported. This
    // is the case the io module's pre-flight exists for, and the one a losing-data defect would hide in.
    const TempDir dir;
    const std::string path = dir.path("never.csv");
    const Session session;
    LossyExporter lossy;

    // The panel would already have answered this, so the window can grey the entry out instead of offering
    // an export that then fails.
    REQUIRE(session.model.export_readiness(lossy) == rt::ExportRefusal::uncertainty_not_supported);

    const ExportReport report = export_trace(session.model, lossy, path);
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.refusal == rt::ExportRefusal::uncertainty_not_supported);
    REQUIRE(report.format_name == "test.lossy");
    // The sentence says what the user loses, not what the code is called.
    REQUIRE(report.message.find("uncertainties") != std::string::npos);
    REQUIRE(report.message.find("without their errors") != std::string::npos);

    // Nothing was written and the exporter was never asked to write: the refusal happened before a path
    // was any use.
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE(lossy.write_calls == 0);
}

TEST_CASE("export.reports_a_format_that_cannot_carry_the_uncertainty", "[export]") {
    // The same property from the other side: with nothing quantified, the uncertainty is not required, and
    // the same lossy format is allowed to export. A rule that refused it anyway would refuse every format
    // for a session that never measured an error, which is not something a user could act on.
    const TempDir dir;
    const std::string path = dir.path("plain.lossy");
    rt::RunLedger other_ledger;
    MeasurementModel plain{other_ledger, "length", qp::units::dims::length};
    REQUIRE(plain.add_channel("x", qp::units::dims::length).has_value());
    REQUIRE(plain.add_sample(0.0, std::vector<double>{1.0}).has_value());

    REQUIRE(plain.export_readiness(csv_exporter()) == rt::ExportRefusal::ok);
    LossyExporter lossy;
    REQUIRE(plain.export_readiness(lossy) == rt::ExportRefusal::ok);

    const ExportReport report = export_trace(plain, lossy, path);
    REQUIRE(report.ok);
    REQUIRE(lossy.write_calls == 1);
}

TEST_CASE("export.reports_a_destination_it_cannot_write", "[export]") {
    // A destination the disk refuses is reported, never counted as a success: a user who believes they have
    // their data is worse off than one who was told to pick another folder.
    const TempDir dir;
    const Session session;

    const ExportReport report =
        export_trace(session.model, csv_exporter(), dir.path("no_such_directory") + "/trace.csv");
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.refusal == rt::ExportRefusal::could_not_write);
    REQUIRE(report.message.find("could not be written") != std::string::npos);

    // An empty session is refused earlier, by the pre-flight, with the sentence that says what is missing.
    rt::RunLedger empty_ledger;
    MeasurementModel empty{empty_ledger, "length", qp::units::dims::length};
    const ExportReport nothing =
        export_trace(empty, csv_exporter(), dir.path("empty.csv"));
    REQUIRE_FALSE(nothing.ok);
    REQUIRE(nothing.refusal == rt::ExportRefusal::nothing_to_write);
    REQUIRE(nothing.message.find("nothing to export") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(dir.path("empty.csv")));
}
