/**
 * @file confidence_panel.cpp
 * @brief Widgets for C8's diagnostics. No arithmetic lives here.
 */
#include "confidence_panel.hpp"

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

/// @brief Row labels, in the order a reader wants them: the headline first.
enum Row { kEnergyDrift = 0, kDriftRate, kSpan, kSamples, kClamps, kRowCount };

const char* const kRowNames[kRowCount] = {
    "energy drift", "drift per second", "time span", "samples", "clamped values"};

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

    table_ = new QTableWidget(kRowCount, 2, this);
    table_->setHorizontalHeaderLabels({tr("diagnostic"), tr("value")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int r = 0; r < kRowCount; ++r) {
        table_->setItem(r, 0, new QTableWidgetItem(QString::fromLatin1(kRowNames[r])));
    }
    layout->addWidget(table_);

    notes_ = new QLabel(this);
    notes_->setWordWrap(true);
    notes_->setStyleSheet(QStringLiteral("color: #8a6d00;"));
    layout->addWidget(notes_);

    refresh();
}

void ConfidencePanel::refresh() {
    const model::ConfidenceReport r = model_.report();

    const auto put = [this](int row, const QString& text) {
        table_->setItem(row, 1, new QTableWidgetItem(text));
    };

    // Every optional is rendered through `unavailable_text()` when absent. The `has_value` checks
    // are the entire reason `ConfidenceReport`'s fields are optional rather than plain doubles.
    put(kEnergyDrift, r.energy_drift.has_value() ? number(r.energy_drift.value())
                                                 : unavailable_text());
    put(kDriftRate, r.energy_drift_rate.has_value() ? number(r.energy_drift_rate.value())
                                                     : unavailable_text());
    put(kSpan, r.span.has_value() ? number(r.span.value()) : unavailable_text());
    put(kSamples, QString::number(r.samples));
    // Never unavailable: the count comes from the operator rather than from the data, so zero is a
    // real answer meaning "the clamp never fired".
    put(kClamps, QString::number(r.clamps_fired));

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
    for (int r = 0; r < kRowCount; ++r) {
        if (QString::fromLatin1(kRowNames[r]) == row_name) {
            const QTableWidgetItem* item = table_->item(r, 1);
            return item != nullptr ? item->text() : QString{};
        }
    }
    return {};
}

}  // namespace qp::views
