/**
 * @file measurement_panel.cpp
 * @brief Widgets for the measurement session. No arithmetic lives here.
 */
#include "measurement_panel.hpp"

#include "theme.hpp"

#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <qp/runtime/store/store.hpp>
#include <qp/units/unit_symbol.hpp>

#include <cmath>

namespace qp::views {
namespace {

namespace rt = qp::runtime;

/// @brief Formats a number the way a lab notebook does: fixed significant figures.
///
/// Six significant figures rather than `%g`'s default. `%g` switches to exponential
/// notation at its own threshold, so a column of readings can come out as "0.0012" and
/// "1.2e-05" side by side, and comparing magnitudes down a column -- which is the first
/// thing a reader does with repeated measurements -- becomes a reading exercise.
QString fixed_number(double v) {
    if (!std::isfinite(v)) return QStringLiteral("--");
    return QString::number(v, 'g', 6);
}

/// @brief How an uncertainty's state is spelled in the table.
///
/// Three states, spelled differently, because they are three different claims. The middle
/// one is the whole point of the model: "unknown" is not "zero", and a table that printed
/// "0" for it would be claiming an error bar of zero width, which reads as infinitely
/// precise.
QString uncertainty_text(const rt::UncertainValue& v) {
    switch (v.kind) {
        case rt::UncertaintyKind::standard:
            return QStringLiteral("+/- ") + fixed_number(v.u);
        case rt::UncertaintyKind::exact:
            return QStringLiteral("exact");
        case rt::UncertaintyKind::unknown:
            return QStringLiteral("unknown");
    }
    return QStringLiteral("?");
}

}  // namespace

MeasurementPanel::MeasurementPanel(model::MeasurementModel& model, QWidget* parent)
    : QWidget(parent), model_(model) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("Measurements"), this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels(
        {tr("#"), tr("value"), tr("uncertainty"), tr("status")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Explicit widths per column instead of `setStretchLastSection`.
    //
    // The first version stretched the last section, which made "status" the only column with
    // room and left "uncertainty" -- the one column this whole platform exists to show --
    // truncated to "+/- 0.000". A table whose widest column is the least informative one is a
    // table that hides its own point, and the first screenshot of this panel showed exactly
    // that.
    //
    // The value and uncertainty columns are sized to their content and the status column takes
    // what is left, so an instrument with more digits gets the room it needs without a change
    // here.
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(table_);

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    gaps_ = new QLabel(this);
    gaps_->setWordWrap(true);
    // Amber rather than red. A gap is not an error: it is a statement about what has not
    // been done yet, and painting it red trains the reader to ignore it.
    gaps_->setStyleSheet(QStringLiteral("color: %1;").arg(qt::theme::to_qcolor(qt::theme::palette().warning).name()));
    layout->addWidget(gaps_);

    refresh();
}

void MeasurementPanel::record_reading(double value, rt::UncertaintyKind kind, double u) {
    model_.add_reading(value, kind, u);
    refresh();
}

void MeasurementPanel::refresh() {
    const auto& readings = model_.dataset().readings();
    table_->setRowCount(static_cast<int>(readings.size()));
    for (std::size_t i = 0; i < readings.size(); ++i) {
        const rt::Measurement& m = readings[i];
        const int row = static_cast<int>(i);

        table_->setItem(row, 0, new QTableWidgetItem(QString::number(i + 1)));
        table_->setItem(row, 1, new QTableWidgetItem(fixed_number(m.reading.value)));
        table_->setItem(row, 2, new QTableWidgetItem(uncertainty_text(m.reading)));

        // A rejected reading stays in the table with its row marked, rather than being
        // removed. Rejection is not deletion: the record keeps it, because a student who can
        // make a reading vanish has a way to reach the answer they expected and leave no
        // trace of having done it.
        if (m.valid) {
            table_->setItem(row, 3, new QTableWidgetItem(tr("counted")));
        } else {
            auto* item = new QTableWidgetItem(tr("rejected"));
            item->setForeground(qt::theme::to_qcolor(qt::theme::palette().warning));
            table_->setItem(row, 3, item);
        }
    }

    const model::ReportLine line = model_.report_line();
    QStringList parts;
    if (line.count == 0) {
        // No readings means no statistics, and the sentence says so rather than printing a
        // mean of zero over zero readings. That zero is the fabrication a hurried
        // implementation produces, and it is indistinguishable from a real measurement of
        // zero.
        parts << tr("no readings yet");
    } else {
        parts << tr("%1 counted").arg(line.count);
        if (line.rejected > 0) parts << tr("%1 rejected").arg(line.rejected);
        // Each statistic is appended only when the model has one. The `has_value` check is
        // the entire reason `ReportLine`'s fields are optional rather than plain doubles.
        if (line.mean.has_value()) parts << tr("mean %1").arg(fixed_number(line.mean.value()));
        if (line.sample_stddev.has_value()) {
            parts << tr("s %1").arg(fixed_number(line.sample_stddev.value()));
        }
        if (line.standard_error.has_value()) {
            parts << tr("u_A %1").arg(fixed_number(line.standard_error.value()));
        }
        if (line.combined_uncertainty.has_value()) {
            parts << tr("combined %1").arg(fixed_number(line.combined_uncertainty.value()));
        } else {
            // Spelled out rather than omitted. An omitted term reads as a term that is zero,
            // and this one is the difference between "we did not measure the error" and
            // "there is no error" -- the single most damaging confusion the platform exists
            // to prevent.
            parts << tr("combined unknown");
        }
    }
    summary_->setText(parts.join(QStringLiteral("   ")));

    const std::vector<std::string> gaps = model_.gaps();
    QStringList lines;
    lines.reserve(static_cast<int>(gaps.size()));
    for (const std::string& g : gaps) {
        // Verbatim from the model. Re-wording it here would mean two places decide what a
        // gap says, and the one that is tested would be the one nobody reads.
        lines << QStringLiteral("- ") + QString::fromStdString(g);
    }
    gaps_->setText(lines.join(QStringLiteral("\n")));
    gaps_->setVisible(!lines.isEmpty());
}

QString MeasurementPanel::summary_text() const { return summary_->text(); }

QStringList MeasurementPanel::gap_lines() const {
    const QString text = gaps_->text();
    if (text.isEmpty()) return {};
    return text.split(QStringLiteral("\n"), Qt::SkipEmptyParts);
}

}  // namespace qp::views
