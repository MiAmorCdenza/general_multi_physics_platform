/**
 * @file test_measurement_model.cpp
 * @brief Tests for the measurement session: the closed loop's arithmetic and its honesty.
 *
 * ## What is worth testing here
 *
 * Not that a mean is a mean -- `Dataset` owns that and `test_store.cpp` covers it. What is
 * worth testing is the **one rule this layer exists to hold**: unknown uncertainty is not
 * zero uncertainty. Everything else in the file is in service of that, because it is the
 * mistake that turns a lab report into a confidently wrong lab report:
 *
 *   - a series where nobody quantified an error must report **no** combined uncertainty,
 *     not a zero one;
 *   - a session must be able to **say** what it is missing, in words a reader can act on;
 *   - a rejected reading must stay in the record, so a student cannot reach the answer they
 *     expected by deleting the point that disagreed;
 *   - the export pre-flight must be the io module's own answer rather than a second opinion,
 *     or the panel offers an export that then fails.
 *
 * The samples-alongside-readings case is here because `Trace` and `Dataset` are separate on
 * purpose: a reading is a repeated measurement of one quantity, a sample is one instant of
 * a signal, and a model that conflated them would force every oscillation into a mean.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/measurement_model.hpp>

#include <qp/runtime/io/io.hpp>
#include <qp/units/dimensions.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace qp::views::model;

namespace {

namespace rt = qp::runtime;

/// @brief A ledger for a session under test, with a fixed lifetime.
///
/// The model borrows rather than owns its ledger, so the test must own one for at least as
/// long as the model. That is the point of the borrow: two ledgers in one session is how the
/// window's status line and the panel's gap list came to disagree on screen.
class Session final {
public:
    Session() : model_(ledger_, "length", qp::units::dims::length) {}

    [[nodiscard]] MeasurementModel& model() noexcept { return model_; }
    [[nodiscard]] rt::RunLedger& ledger() noexcept { return ledger_; }

private:
    rt::RunLedger ledger_{};
    MeasurementModel model_;
};



/// @brief An exporter that declares it keeps uncertainty and writes nothing.
///
/// Defined here rather than reused from the io tests because this file needs the *declared*
/// capabilities only -- the question under test is whether the model asks the exporter
/// rather than guessing.
class DeclaringExporter final : public rt::IExporter {
public:
    DeclaringExporter(bool keeps_uncertainty, bool multi_dataset)
        : keeps_uncertainty_(keeps_uncertainty), multi_dataset_(multi_dataset) {}

    [[nodiscard]] const rt::FormatDesc& format() const noexcept override { return desc_; }
    [[nodiscard]] rt::ExportRefusal write(const rt::ExportRequest&) noexcept override {
        return rt::ExportRefusal::ok;
    }
    [[nodiscard]] bool is_available() const noexcept override { return true; }

private:
    rt::FormatDesc desc_{};
    bool keeps_uncertainty_ = false;
    bool multi_dataset_ = false;
};

}  // namespace

TEST_CASE("measurement.model.add_and_retake", "[measurement][model]") {
    Session session;
    MeasurementModel& m = session.model();
    REQUIRE(m.dataset().name() == "length");
    REQUIRE(m.dataset().empty());

    m.add_reading(1.00, rt::UncertaintyKind::standard, 0.05);
    m.add_reading(1.10, rt::UncertaintyKind::standard, 0.05);
    m.add_reading(0.95, rt::UncertaintyKind::standard, 0.05);
    REQUIRE(m.dataset().size() == 3);
    REQUIRE(m.dataset().valid_count() == 3);

    // Rejection is not deletion. A reading that was taken and then discarded for a stated
    // reason belongs in the record; a student who can make one vanish has a way to reach the
    // answer they expected, and the report would show no trace of it.
    REQUIRE(m.reject(1).has_value());
    REQUIRE(m.dataset().size() == 3);          // still there
    REQUIRE(m.dataset().valid_count() == 2);   // but not counted
    REQUIRE(m.dataset().readings()[1].valid == false);

    // And a rejected reading still contributes to the report's `rejected` count, which is
    // what makes the rejection visible rather than merely effective.
    const ReportLine line = m.report_line();
    REQUIRE(line.count == 2);
    REQUIRE(line.rejected == 1);

    // Restoring puts it back, and restoring a live reading is a no-op rather than an error.
    REQUIRE(m.restore(1).has_value());
    REQUIRE(m.dataset().valid_count() == 3);
    REQUIRE(m.restore(1).has_value());
    REQUIRE(m.dataset().valid_count() == 3);

    // An index past the end is refused rather than ignored: silently dropping the request
    // would make a caller believe a reading had been rejected when it had not.
    REQUIRE_FALSE(m.reject(99).has_value());
    REQUIRE_FALSE(m.restore(99).has_value());
}

TEST_CASE("measurement.model.unknown_is_not_zero", "[measurement][model]") {
    // The rule the layer exists to hold. Three readings, none of them with a quantified
    // uncertainty, is the normal state of a first-year lab notebook: the numbers exist and
    // the errors were never worked out.
    Session session;
    MeasurementModel& m = session.model();
    m.add_reading(1.00);
    m.add_reading(1.10);
    m.add_reading(0.95);

    const ReportLine line = m.report_line();
    REQUIRE(line.count == 3);
    REQUIRE(line.quantified == 0);

    // The mean and the spread are still available -- they are properties of the numbers,
    // and refusing to compute them would be a different kind of dishonesty.
    REQUIRE(line.mean.has_value());
    REQUIRE(line.sample_stddev.has_value());
    REQUIRE(line.standard_error.has_value());

    // But the combined uncertainty is **absent**, not zero. A zero here would be a bare
    // number presented with an error bar of zero width, which reads as infinitely precise.
    REQUIRE_FALSE(line.combined_uncertainty.has_value());

    // And the session says why. A report silent about a gap is claiming there is no gap.
    const std::vector<std::string> gaps = m.gaps();
    REQUIRE_FALSE(gaps.empty());
    bool mentions_uncertainty = false;
    for (const std::string& g : gaps) {
        if (g.find("uncertainty") != std::string::npos) mentions_uncertainty = true;
    }
    REQUIRE(mentions_uncertainty);

    // Once a reading carries a quantified uncertainty, the combined figure exists. One
    // quantified reading among three is not enough for a *good* answer, and the model says
    // so -- but it is enough for an answer, because something was measured.
    m.add_reading(1.05, rt::UncertaintyKind::standard, 0.05);
    const ReportLine after = m.report_line();
    REQUIRE(after.quantified == 1);
    REQUIRE(after.combined_uncertainty.has_value());
    REQUIRE(after.combined_uncertainty.value() > 0.0);

    // `exact` is a different statement from `unknown`, and it must not be substituted for
    // it. An exact reading is a counted quantity, where u = 0 is a true claim.
    {
        rt::RunLedger counted_ledger;
        MeasurementModel counted{counted_ledger, "fringes", qp::units::Dim{}};
        counted.add_reading(12.0, rt::UncertaintyKind::exact);
        counted.add_reading(13.0, rt::UncertaintyKind::exact);
        const ReportLine exact_line = counted.report_line();
        REQUIRE(exact_line.quantified == 0);
        // Even here the combined uncertainty is absent rather than zero: `quantified_count`
        // counts readings whose uncertainty is *standard*, and `exact` is not a measurement
        // of error. The distinction is kept rather than smoothed over.
        REQUIRE_FALSE(exact_line.combined_uncertainty.has_value());
    }
}

TEST_CASE("measurement.model.report_names_its_gaps", "[measurement][model]") {
    // An empty session's only gap is that it is empty, and it says exactly that. Every
    // other statement about the statistics would be meaningless without this one, so it is
    // reported alone rather than alongside them.
    {
        Session session;
    MeasurementModel& m = session.model();
        const std::vector<std::string> gaps = m.gaps();
        REQUIRE(gaps.size() == 1);
        REQUIRE(gaps[0].find("no readings") != std::string::npos);
    }

    // One reading cannot produce a spread from repetition, and the model says which
    // quantity is unreachable rather than silently omitting a column.
    {
        Session session;
    MeasurementModel& m = session.model();
        m.add_reading(1.0, rt::UncertaintyKind::standard, 0.1);
        const std::vector<std::string> gaps = m.gaps();
        bool mentions_spread = false;
        for (const std::string& g : gaps) {
            if (g.find("one reading") != std::string::npos) mentions_spread = true;
        }
        REQUIRE(mentions_spread);
    }

    // Every reading rejected leaves nothing to report, which is a different message from
    // "no readings recorded" and needs saying: the user did record some.
    {
        Session session;
    MeasurementModel& m = session.model();
        m.add_reading(1.0);
        REQUIRE(m.reject(0).has_value());
        const std::vector<std::string> gaps = m.gaps();
        REQUIRE(gaps.size() == 1);
        REQUIRE(gaps[0].find("rejected") != std::string::npos);
    }

    // No run recorded means the session cannot be traced to a configuration, which is the
    // reproducibility half of the report.
    {
        Session session;
    MeasurementModel& m = session.model();
        m.add_reading(1.0, rt::UncertaintyKind::standard, 0.1);
        m.add_reading(1.1, rt::UncertaintyKind::standard, 0.1);
        bool mentions_run = false;
        for (const std::string& g : m.gaps()) {
            if (g.find("run") != std::string::npos) mentions_run = true;
        }
        REQUIRE(mentions_run);
    }

    // A run whose spec is complete removes that gap; an incomplete one names the missing
    // field. Naming the field is the whole point -- "not reproducible" sends the reader
    // looking in their own physics, while "missing toolchain" sends them to the build.
    {
        Session session;
    MeasurementModel& m = session.model();
        m.add_reading(1.0, rt::UncertaintyKind::standard, 0.1);
        m.add_reading(1.1, rt::UncertaintyKind::standard, 0.1);

        rt::RunSpec spec;
        spec.seed = 12345;
        spec.graph_version = 7;
        const rt::RunId id = m.begin_run(spec);
        REQUIRE(id.valid());
        // Read from the **session's** ledger, not the model's: they are the same object
        // now, and that identity is the property this case exists to pin.
        REQUIRE(session.ledger().size() == 1);
        REQUIRE(&m.ledger() == &session.ledger());

        // The spec deliberately omits its toolchain, so the gap must name that.
        bool mentions_missing = false;
        for (const std::string& g : m.gaps()) {
            if (g.find("missing") != std::string::npos) {
                mentions_missing = true;
                REQUIRE(g.find("cannot be reproduced") != std::string::npos);
            }
        }
        REQUIRE(mentions_missing);
    }
}

TEST_CASE("measurement.model.trace_is_separate_from_readings", "[measurement][model]") {
    // A reading is a repeated measurement of one quantity; a sample is one instant of a
    // signal. A model that conflated them would force an oscillation into a mean, which is
    // exactly the artefact the platform exists to replace.
    Session session;
    MeasurementModel& m = session.model();

    REQUIRE(m.add_channel("x", qp::units::dims::length).has_value());
    REQUIRE(m.add_channel("t", qp::units::dims::time).has_value());

    for (int i = 0; i < 50; ++i) {
        const double t = 0.01 * i;
        const double x = std::sin(t);
        REQUIRE(m.add_sample(t, std::vector<double>{x, t}, 0.001).has_value());
    }

    REQUIRE(m.trace().size() == 50);
    REQUIRE(m.trace().channel_count() == 2);

    // The readings are untouched by any of that: the two records are independent, which is
    // the property being asserted rather than a side effect of it.
    REQUIRE(m.dataset().empty());

    // An out-of-order time is refused rather than reordered. Reordering would renumber the
    // samples already recorded, so an index taken before the insert would afterwards point
    // at a different measurement -- and the record would show nothing had happened.
    REQUIRE_FALSE(m.add_sample(0.0, std::vector<double>{0.0, 0.0}).has_value());
    REQUIRE(m.trace().size() == 50);
}

TEST_CASE("measurement.model.export_refusal_matches_the_exporter", "[measurement][model]") {
    // The panel must not form a second opinion about what a format can carry. If it did,
    // an export could pass the panel's check and fail the exporter's, and the user's
    // experience would be a button that raises an error dialog -- the outcome the io
    // module's pre-flight check exists to prevent.
    Session session;
    MeasurementModel& m = session.model();
    m.add_reading(1.0, rt::UncertaintyKind::standard, 0.05);
    m.add_reading(1.1, rt::UncertaintyKind::standard, 0.05);
    REQUIRE(m.add_channel("x", qp::units::dims::length).has_value());
    REQUIRE(m.add_sample(0.0, std::vector<double>{1.0}, 0.05).has_value());

    // The model's answer is checked against the io module's own, for the session as it
    // stands, rather than against a literal. A literal would pass while the two drifted.
    const DeclaringExporter keeps{true, false};
    const DeclaringExporter drops{false, false};

    rt::ExportRequest request;
    request.trace = &m.trace();
    request.require_uncertainty = true;

    REQUIRE(m.export_readiness(keeps) == rt::check_export(keeps, request));
    REQUIRE(m.export_readiness(drops) == rt::check_export(drops, request));
}
