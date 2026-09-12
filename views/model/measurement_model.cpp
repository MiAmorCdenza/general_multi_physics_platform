/**
 * @file measurement_model.cpp
 * @brief Implementation of the measurement session model.
 *
 * The arithmetic is all delegated: `Dataset` owns the statistics and `Trace` owns the time
 * axis, and neither is reimplemented here. What this file adds is the **judgement** the
 * panel must not make for itself -- when a number is unknown rather than zero, and which
 * absences a reader has to be told about.
 */
#include <qp/views/model/measurement_model.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace qp::views::model {
namespace {

namespace store = qp::runtime;

}  // namespace

MeasurementModel::MeasurementModel(store::RunLedger& ledger, std::string quantity,
                                     qp::units::Dim dim, store::RunId run)
    : ledger_(ledger), dataset_(std::move(quantity), dim), trace_(run) {}

void MeasurementModel::add_reading(double value, store::UncertaintyKind kind, double u) {
    // The uncertainty is stored only for the state that has one. Writing `u` through for an
    // `exact` reading would put a number in a field whose meaning is "we quantified this",
    // and `is_usable()` would then report a quantity nobody measured.
    //
    // `UncertainValue`'s three factory functions are used rather than assembling the struct
    // field by field, so that the invariant "u is meaningless unless kind == standard" is
    // stated once, in the type that owns it, instead of being re-asserted here.
    switch (kind) {
        case store::UncertaintyKind::exact:
            dataset_.add(store::UncertainValue::exact(value, dataset_.dim()));
            break;
        case store::UncertaintyKind::standard:
            dataset_.add(store::UncertainValue::measured(value, u, dataset_.dim()));
            break;
        case store::UncertaintyKind::unknown:
            dataset_.add(store::UncertainValue::unquantified(value, dataset_.dim()));
            break;
    }
}

diag::Result<void> MeasurementModel::reject(std::size_t index) { return dataset_.reject(index); }

diag::Result<void> MeasurementModel::restore(std::size_t index) { return dataset_.restore(index); }

diag::Result<std::size_t> MeasurementModel::add_channel(std::string name, qp::units::Dim dim) {
    store::Channel channel;
    channel.name = std::move(name);
    channel.dim = dim;
    return trace_.add_channel(std::move(channel));
}

diag::Result<void> MeasurementModel::add_sample(double t,
                                                std::vector<store::UncertainValue> values) {
    return trace_.append(t, std::move(values));
}

diag::Result<void> MeasurementModel::add_sample(double t, const std::vector<double>& values,
                                                double uncertainty) {
    // Built through the overload above rather than appending directly, so the two entry
    // points cannot drift apart on what a sample means.
    std::vector<store::UncertainValue> converted;
    converted.reserve(values.size());
    for (const double v : values) {
        converted.push_back(store::UncertainValue::measured(v, uncertainty, dataset_.dim()));
    }
    return add_sample(t, std::move(converted));
}

store::RunId MeasurementModel::begin_run(store::RunSpec spec) {
    return ledger_.begin(std::move(spec));
}

ReportLine MeasurementModel::report_line() const {
    ReportLine line;
    line.quantity = dataset_.name();
    line.count = dataset_.valid_count();
    line.rejected = dataset_.size() - line.count;
    line.quantified = dataset_.quantified_count();

    // Every optional is filled only from the dataset's own answer. `Dataset` returns
    // `std::optional` from these accessors precisely so that "no readings" and "a mean of
    // zero" cannot be confused, and unwrapping with `value_or(0.0)` here would undo that
    // decision one layer up -- which is where a rushed panel would do it.
    line.mean = dataset_.mean();
    line.sample_stddev = dataset_.sample_stddev();
    line.standard_error = dataset_.standard_error();

    // The combined uncertainty exists only when something was quantified. This is the rule
    // the whole model exists to hold: `unknown` is not `exact`, so a series where nobody
    // measured an error has **no** combined uncertainty rather than a zero one. Reporting
    // zero here would produce a bare number with no error bar, presented as a result.
    if (line.quantified > 0) {
        line.combined_uncertainty = dataset_.weighted_mean_uncertainty();
    }
    return line;
}

std::vector<std::string> MeasurementModel::gaps() const {
    std::vector<std::string> out;

    // Ordered by what a reader needs first. "No readings at all" comes before anything about
    // uncertainty, because every other statement about the statistics is meaningless without
    // it -- and a message about uncertainty on an empty series reads as though there were
    // numbers to be uncertain about.
    if (dataset_.empty()) {
        out.emplace_back("no readings recorded");
        return out;
    }
    if (dataset_.valid_count() == 0) {
        out.emplace_back("every reading has been rejected, so there is nothing to report");
        return out;
    }
    if (dataset_.valid_count() == 1) {
        out.emplace_back(
            "one reading: a sample standard deviation needs at least two, so no uncertainty "
            "can be derived from the spread");
    }
    if (dataset_.quantified_count() == 0) {
        // Named as an absence rather than a value. The alternative -- showing 0 or "N/A" --
        // is how a report ends up claiming an error bar nobody measured.
        out.emplace_back(
            "no reading carries a quantified uncertainty, so the Type A and combined "
            "uncertainties are unknown rather than zero");
    } else if (dataset_.quantified_count() < dataset_.valid_count()) {
        out.emplace_back("some readings carry no quantified uncertainty; the combined figure "
                         "uses only those that do");
    }

    // The reproducibility claim, taken from the ledger's own completeness judgement rather
    // than re-derived. A record that is missing its toolchain is not reproducible, and the
    // honest thing is to say which field is missing rather than to print "reproducible: no".
    if (ledger_.size() == 0) {
        out.emplace_back("no run recorded, so this session cannot be traced to a configuration");
    } else {
        const store::RunRecord* last = ledger_.find(ledger_.last_id());
        if (last != nullptr && !last->is_complete()) {
            const std::vector<std::string> missing = last->spec.missing_names();
            std::string message = "the last run is missing ";
            for (std::size_t i = 0; i < missing.size(); ++i) {
                if (i != 0) message += ", ";
                message += missing[i];
            }
            message += "; it cannot be reproduced as recorded";
            out.push_back(std::move(message));
        }
    }
    return out;
}

store::ExportRefusal MeasurementModel::export_readiness(const store::IExporter& format) const {
    // Delegated, not re-derived. If the panel computed this itself then a format could pass
    // the panel's check and fail the exporter's, and the user's experience would be an
    // export button that raises an error dialog -- the exact outcome the io module's
    // pre-flight check exists to prevent.
    store::ExportRequest request;
    request.trace = &trace_;
    request.path = "unused";
    request.require_uncertainty = dataset_.quantified_count() > 0;
    return store::check_export(format, request);
}

}  // namespace qp::views::model
