/**
 * @file measurement_panel.cpp
 * @brief Widgets for the measurement session. No arithmetic lives here.
 */
#include "measurement_panel.hpp"

#include "theme.hpp"

#include <QFont>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <qp/graph/ir/ids.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/units/unit_symbol.hpp>

#include <cmath>
#include <optional>

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

    // The device list, above the table because it decides what a reading **is**: the number in the first column
    // is only a measurement when the instrument that produced it is named, and a table of values with the device
    // out of sight is the artefact this platform exists to replace.
    //
    // The panel is filled by `show_devices` rather than here. It does not own the registry, and a panel that
    // reached for one would be deciding whose devices are authoritative -- the same mistake the window already
    // removed once when it kept a second node catalog.
    devices_ = new QComboBox(this);
    devices_->setToolTip(tr("The instrument a new reading is taken with"));
    layout->addWidget(devices_);

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

    // The one wire in this window that runs from the numbers back to the graph. Every other
    // connection goes the other way -- the document changes and the canvas redraws -- so this
    // is the direction that was missing: a student reading a value off the table can ask
    // which device produced it, which is the question a lab report is graded on.
    //
    // Wired to `itemSelectionChanged` rather than to `cellClicked`, because a row can also be
    // chosen with the keyboard or by a programmatic `selectRow`, and a highlight that only
    // appeared for mouse clicks would be a highlight that stops working the moment somebody
    // navigates the table the way a screen reader does.
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
        on_row_selected(table_->currentRow());
    });

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

void MeasurementPanel::show_devices(const qp::runtime::InstrumentRegistry& registry) {
    devices_->clear();
    for (qp::runtime::IInstrument* device : registry.all()) {
        if (device == nullptr) continue;
        const qp::runtime::InstrumentDesc& desc = device->describe();
        // The **label**, not the id: a user chooses "Vernier caliper" from a bench, and the id is the record's
        // business. Both are kept -- the label is what is shown, the id is what `chosen_device()` returns -- so a
        // renamed label never breaks a caller and a caller never has to parse a display string.
        devices_->addItem(QString::fromStdString(desc.label.empty() ? desc.id : desc.label),
                          QString::fromStdString(desc.id));
    }

    // A build with no devices says so. A blank list is indistinguishable from a panel that failed to fill, and
    // "there are no instruments" is a fact the user can act on: it means this build was assembled without them.
    if (devices_->count() == 0) {
        devices_->addItem(tr("no instruments in this build"), QString{});
    }
    devices_->setCurrentIndex(0);
}

std::string MeasurementPanel::chosen_device() const {
    return devices_->currentData().toString().toStdString();
}

void MeasurementPanel::choose_device(const std::string& id) {
    const QString wanted = QString::fromStdString(id);
    for (int i = 0; i < devices_->count(); ++i) {
        if (devices_->itemData(i).toString() == wanted) {
            devices_->setCurrentIndex(i);
            return;
        }
    }
}

QString MeasurementPanel::summary_text() const { return summary_->text(); }

QStringList MeasurementPanel::gap_lines() const {
    const QString text = gaps_->text();
    if (text.isEmpty()) return {};
    return text.split(QStringLiteral("\n"), Qt::SkipEmptyParts);
}

std::optional<qp::runtime::Measurement::Source> MeasurementPanel::selected_source() const {
    const int row = table_->currentRow();
    if (row < 0) return std::nullopt;
    return model_.source_of(static_cast<std::size_t>(row));
}

void MeasurementPanel::on_row_selected(int row) {
    if (row < 0) return;

    // Asked of the model rather than read off the widget. A row number in a table is a display
    // artifact -- sorting, filtering or an inserted separator would each change what it means --
    // and the model is the only place that maps a position in `readings()` to the node behind it.
    const std::optional<rt::Measurement::Source> source =
        model_.source_of(static_cast<std::size_t>(row));

    // Silence for a reading with no source, rather than an invalid id. See the signal's docs: an
    // invalid id would make the window clear its canvas selection, and "nothing is selected"
    // reads as "the reading was thrown away".
    if (!source.has_value() || !source->valid()) return;

    Q_EMIT reading_selected(qp::graph::NodeId{source->index, source->generation});
}

}  // namespace qp::views
