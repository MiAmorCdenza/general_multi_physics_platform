/**
 * @file export_controller.cpp
 * @brief The pre-flight, the write, and the sentences between them.
 */
#include <qp/views/model/export_controller.hpp>

#include <string>

namespace qp::views::model {

namespace rt = qp::runtime;

rt::FormatRegistry& export_formats() noexcept {
    // A function-local static, for the reason the other two registries use one: the initialisation order of
    // namespace-scope objects across translation units is unspecified, and this is read from a window's
    // constructor.
    static rt::FormatRegistry registry;
    return registry;
}

qp::diag::Result<void> mount_export_format(rt::IExporter* exporter) noexcept {
    if (exporter == nullptr) return qp::diag::ErrorCode::invalid_argument;
    return export_formats().add(exporter);
}

std::string describe_export_refusal(rt::ExportRefusal refusal, rt::ExportSubject subject) {
    // What is being written, in the words a reader of the file would use. The **subject takes part in the sentence**
    // because one refusal code covers every table a format might not have a shape for: `subject_not_supported` says
    // "not this kind of table", and which kind is what the user needs in order to decide whether to pick another
    // format or export something else.
    const char* const what = subject == rt::ExportSubject::readings ? "readings"
                            : subject == rt::ExportSubject::fit    ? "fitted parameters"
                                                                   : "trace";
    switch (refusal) {
        case rt::ExportRefusal::ok:
            return "ready to export";
        case rt::ExportRefusal::unknown_format:
            return "no format is registered under that name";
        case rt::ExportRefusal::uncertainty_not_supported:
            // Named as the consequence rather than the rule, because the consequence is what the user
            // decides about: a table whose error bars are missing still looks like a table.
            return std::string{"this format cannot keep the measurement uncertainties, and this session has some: "
                               "the exported "} +
                   what + " would show the values without their errors";
        case rt::ExportRefusal::subject_not_supported:
            return std::string{"this format cannot write a table of "} + what +
                   ": it writes a series, one row per sample, and there is no place in it for these rows. Choose "
                   "another format";
        case rt::ExportRefusal::subject_missing:
            return std::string{"nothing was offered to export: the subject is "} + what +
                   " and the request carries none, which is a bug in the caller rather than a property of the "
                   "session";
        case rt::ExportRefusal::nothing_to_write:
            return std::string{"there is nothing to export yet: this session has no "} + what;
        case rt::ExportRefusal::shape_mismatch:
            return "the requested table and the labels do not line up: a name was given for some rows and not "
                   "others";
        case rt::ExportRefusal::could_not_write:
            return "the file could not be written (a directory that does not exist, no permission, or no "
                   "space)";
    }
    return "the export was refused";
}

ExportReport export_readings(const MeasurementModel& model, rt::IExporter& format,
                            const std::vector<std::string>& labels, const std::string& path) {
    ExportReport report;
    report.path = path;
    report.format_name = format.format().name;

    // One request, built once and used twice, exactly as the trace path does it: the pre-flight and the write must
    // not be able to disagree about what is being exported.
    const rt::ExportRequest request = model.readings_export_request(path, &labels);

    const rt::ExportRefusal ready = rt::check_export(format, request);
    if (ready != rt::ExportRefusal::ok) {
        report.refusal = ready;
        report.message = describe_export_refusal(ready, request.subject);
        return report;
    }

    const rt::ExportRefusal written = format.write(request);
    report.refusal = written;
    if (written != rt::ExportRefusal::ok) {
        report.message = describe_export_refusal(written, request.subject);
        return report;
    }

    report.ok = true;
    report.message = "exported " + std::to_string(model.dataset().readings().size()) + " readings as " +
                     report.format_name + " to " + path;
    return report;
}

ExportReport export_trace(const MeasurementModel& model, rt::IExporter& format,
                          const std::string& path) {
    ExportReport report;
    report.path = path;
    report.format_name = format.format().name;

    // The request comes from the model, so the question asked here is the one the panel answered -- the
    // trace and the uncertainty policy both. Built once and used twice on purpose: the pre-flight and the
    // write must not be able to disagree about what is being exported.
    const rt::ExportRequest request = model.export_request(path);

    const rt::ExportRefusal ready = rt::check_export(format, request);
    if (ready != rt::ExportRefusal::ok) {
        report.refusal = ready;
        report.message = describe_export_refusal(ready, request.subject);
        return report;
    }

    // Past the pre-flight, so `write` is the only thing left that can refuse: the exporter re-runs the
    // same check itself (a caller that skipped the pre-flight must still be refused) and reports whatever
    // the disk said.
    const rt::ExportRefusal written = format.write(request);
    report.refusal = written;
    if (written != rt::ExportRefusal::ok) {
        report.message = describe_export_refusal(written, request.subject);
        return report;
    }

    report.ok = true;
    report.message = "exported " + std::to_string(model.trace().size()) + " samples as " +
                     report.format_name + " to " + path;
    return report;
}

rt::ExportRequest fit_export_request(const rt::FitResult& fit, std::string path,
                                     const std::vector<std::string>* labels) {
    rt::ExportRequest request;
    request.subject = rt::ExportSubject::fit;
    request.fit = &fit;
    request.coefficient_labels = labels;
    request.path = std::move(path);
    // Unconditional, and the difference from the readings path is deliberate. A session may hold readings nobody
    // quantified and still be worth exporting; a fit's coefficients always have a standard uncertainty, and a table
    // that dropped it would not be a weaker result but a different one. See the contract.
    request.require_uncertainty = true;
    return request;
}

rt::ExportRefusal fit_readiness(const rt::IExporter& format, const rt::FitResult& fit) noexcept {
    return rt::check_export(format, fit_export_request(fit, "unused", nullptr));
}

ExportReport export_fit(const rt::FitResult& fit, rt::IExporter& format,
                        const std::vector<std::string>& labels, const std::string& path) {
    ExportReport report;
    report.path = path;
    report.format_name = format.format().name;

    // One request, built once and used twice, exactly as the other two paths do it: the pre-flight and the write must
    // not be able to disagree about what is being exported.
    const rt::ExportRequest request = fit_export_request(fit, path, &labels);

    const rt::ExportRefusal ready = rt::check_export(format, request);
    if (ready != rt::ExportRefusal::ok) {
        report.refusal = ready;
        report.message = describe_export_refusal(ready, request.subject);
        return report;
    }

    const rt::ExportRefusal written = format.write(request);
    report.refusal = written;
    if (written != rt::ExportRefusal::ok) {
        report.message = describe_export_refusal(written, request.subject);
        return report;
    }

    report.ok = true;
    // The three quality numbers are always written -- chi-squared, the degrees of freedom and R-squared -- so the
    // sentence can count them without asking the format what it did.
    report.message = "exported " + std::to_string(fit.coefficients.size()) + " fitted parameters and 3 quality " +
                     "numbers as " + report.format_name + " to " + path;
    return report;
}

}  // namespace qp::views::model
