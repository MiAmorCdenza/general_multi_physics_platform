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

std::string describe_export_refusal(rt::ExportRefusal refusal) {
    switch (refusal) {
        case rt::ExportRefusal::ok:
            return "ready to export";
        case rt::ExportRefusal::unknown_format:
            return "no format is registered under that name";
        case rt::ExportRefusal::uncertainty_not_supported:
            // Named as the consequence rather than the rule, because the consequence is what the user
            // decides about: a table whose error bars are missing still looks like a table.
            return "this format cannot keep the measurement uncertainties, and this session has some: the "
                   "exported file would show the values without their errors";
        case rt::ExportRefusal::nothing_to_write:
            return "there is nothing to export yet: this measurement session has no samples";
        case rt::ExportRefusal::shape_mismatch:
            return "the recorded samples and channels disagree about how many values a sample has";
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
        report.message = describe_export_refusal(ready);
        return report;
    }

    const rt::ExportRefusal written = format.write(request);
    report.refusal = written;
    if (written != rt::ExportRefusal::ok) {
        report.message = describe_export_refusal(written);
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
        report.message = describe_export_refusal(ready);
        return report;
    }

    // Past the pre-flight, so `write` is the only thing left that can refuse: the exporter re-runs the
    // same check itself (a caller that skipped the pre-flight must still be refused) and reports whatever
    // the disk said.
    const rt::ExportRefusal written = format.write(request);
    report.refusal = written;
    if (written != rt::ExportRefusal::ok) {
        report.message = describe_export_refusal(written);
        return report;
    }

    report.ok = true;
    report.message = "exported " + std::to_string(model.trace().size()) + " samples as " +
                     report.format_name + " to " + path;
    return report;
}

}  // namespace qp::views::model
