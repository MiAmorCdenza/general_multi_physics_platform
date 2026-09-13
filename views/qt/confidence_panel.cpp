/**
 * @file confidence_panel.cpp
 * @brief Widgets for C8's diagnostics. No arithmetic lives here.
 */
#include "confidence_panel.hpp"

#include "theme.hpp"

#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>

namespace qp::views {
namespace {

/// @brief Formats a number the way a lab notebook does.
QString number(double v) {
    if (!std::isfinite(v)) return ConfidencePanel::unavailable_text();
    return QString::number(v, 'g', 6);
}

/// @brief The rows that are always present, below the checks.
enum StandingRow { kSpan = 0, kSamples, kClamps, kStandingCount };

const char* const kStandingNames[kStandingCount] = {"time span", "samples", "clamped values"};

/// @brief How many rows one invariant check needs: its relative drift, and that per second.
constexpr int kRowsPerCheck = 2;

/// @brief The name of a check's drift row, said so that **the kind of number it is** is in the label.
///
/// The old panel had one row called "energy drift" and one called "drift per second", and the second did not even
/// say *what* was drifting once there was more than one law to measure. Both problems are fixed by putting the
/// quantity in both labels and, for a check whose drift is the configuration's rather than the integrator's, by
/// naming that too: "mu drift (physical, not error)" cannot be read as a bug report.
[[nodiscard]] QString check_label(const model::InvariantCheck& check) {
    QString name = QString::fromLatin1(check.name);
    if (!check.measures_error) return name + QObject::tr(" drift (of the physics, not the step)");
    return name + QObject::tr(" drift");
}

}  // namespace

QString ConfidencePanel::unavailable_text() {
    // A dash rather than `0`, and the wording matters: `0` reads as "no drift was found", which is
    // a claim. "not measurable" reads as "this run was not asked", which is the truth when a channel
    // is missing. C8's whole purpose is to stop a numerical artefact being read as physics, and a
    // zero here would be the same mistake arriving from the other side.
    return QStringLiteral("not measurable");
}

ConfidencePanel::ConfidencePanel(model::ConfidenceModel& model, QWidget* parent)
    : QWidget(parent), model_(model) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("Confidence"), this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    // The row count depends on the trace: a run carries zero, one or two conservation laws, and the panel shows
    // the ones it has rather than a fixed list of rows for quantities this run never had. The table is therefore
    // rebuilt in `refresh()`; here it is only configured.
    table_ = new QTableWidget(0, 2, this);
    table_->setHorizontalHeaderLabels({tr("diagnostic"), tr("value")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    layout->addWidget(table_);

    notes_ = new QLabel(this);
    notes_->setWordWrap(true);
    notes_->setStyleSheet(QStringLiteral("color: %1;").arg(qt::theme::to_qcolor(qt::theme::palette().warning).name()));
    layout->addWidget(notes_);

    refresh();
}

void ConfidencePanel::refresh() {
    const model::ConfidenceReport r = model_.report();

    const auto put = [this](int row, const QString& text) {
        table_->setItem(row, 1, new QTableWidgetItem(text));
    };

    // One pair of rows per check, then the standing rows. Every optional is rendered through
    // `unavailable_text()` when absent: the `has_value` checks are the entire reason those fields are optional
    // rather than plain doubles, and a `0` printed for "not measurable" is the mistake C8 exists to prevent.
    const int rows = static_cast<int>(r.checks.size()) * kRowsPerCheck + kStandingCount;
    table_->setRowCount(rows);
    for (int index = 0; index < static_cast<int>(r.checks.size()); ++index) {
        const model::InvariantCheck& check = r.checks[static_cast<std::size_t>(index)];
        const int drift_row = index * kRowsPerCheck;
        table_->setItem(drift_row, 0, new QTableWidgetItem(check_label(check)));
        table_->setItem(drift_row, 1,
                        new QTableWidgetItem(check.relative_drift.has_value()
                                                 ? number(check.relative_drift.value())
                                                 : unavailable_text()));
        table_->setItem(drift_row + 1, 0, new QTableWidgetItem(check_label(check) + tr(" per second")));
        table_->setItem(drift_row + 1, 1,
                        new QTableWidgetItem(check.relative_rate.has_value()
                                                 ? number(check.relative_rate.value())
                                                 : unavailable_text()));
    }
    const int standing = static_cast<int>(r.checks.size()) * kRowsPerCheck;
    for (int index = 0; index < kStandingCount; ++index) {
        table_->setItem(standing + index, 0, new QTableWidgetItem(QString::fromLatin1(kStandingNames[index])));
    }
    put(standing + kSpan, r.span.has_value() ? number(r.span.value()) : unavailable_text());
    put(standing + kSamples, QString::number(r.samples));
    // Never unavailable: the count comes from the operator rather than from the data, so zero is a
    // real answer meaning "the clamp never fired".
    put(standing + kClamps, QString::number(r.clamps_fired));

    const std::vector<std::string> notes = model_.notes();
    QStringList lines;
    lines.reserve(static_cast<int>(notes.size()));
    for (const std::string& n : notes) {
        // Verbatim. See the header for why re-wording here would put the tested text and the
        // displayed text in different places.
        lines << QStringLiteral("- ") + QString::fromStdString(n);
    }
    notes_->setText(lines.join(QStringLiteral("\n")));
    notes_->setVisible(!lines.isEmpty());
}

QStringList ConfidencePanel::note_lines() const {
    const QString text = notes_->text();
    if (text.isEmpty()) return {};
    return text.split(QStringLiteral("\n"), Qt::SkipEmptyParts);
}

QString ConfidencePanel::row_text(const QString& row_name) const {
    // **The row labels are read off the table rather than from a fixed list**, because the list depends on the
    // trace: there is a pair of rows per conservation law the run carries, and a run may carry none, one or two.
    // A caller asking for "energy drift" on a magnetic run gets an empty string, which is the truth -- the panel is
    // not showing that row -- and a caller asking for a check's own label finds exactly what is displayed.
    for (int r = 0; r < table_->rowCount(); ++r) {
        const QTableWidgetItem* label = table_->item(r, 0);
        if (label == nullptr || label->text() != row_name) continue;
        const QTableWidgetItem* item = table_->item(r, 1);
        return item != nullptr ? item->text() : QString{};
    }
    return {};
}

}  // namespace qp::views
