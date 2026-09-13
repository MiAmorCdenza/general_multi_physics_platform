/**
 * @file fit_panel.cpp
 * @brief Rendering for the fit, and the one call into `plugins/analysis`. No arithmetic lives here.
 *
 * The `#if` in the middle of this file is the reason it is in `views/qt` rather than in the Qt-free half: the
 * fit is performed by a **plugin**, and `views/model` is compiled in configurations where no plugin exists. See
 * the header for why that is a dependency decision rather than a preference, and for what a build without the
 * plugin shows instead of a fit.
 */
#include "fit_panel.hpp"

#if defined(QP_HAS_ANALYSIS_PLUGIN)
#include <qp/plugins/analysis/fit.hpp>
#endif

#include "theme.hpp"

#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qp::views {
namespace {

namespace model = qp::views::model;

/// @brief Formats a number the way a lab notebook does, or the panel's "not available" text.
QString number(double v) {
    if (!std::isfinite(v)) return FitPanel::unavailable_text();
    // Six significant figures: enough to see a gradient's third digit, few enough that a double's last two are
    // not printed as though they meant something. A fitted coefficient is never known to seventeen digits.
    return QString::number(v, 'g', 6);
}

#if defined(QP_HAS_ANALYSIS_PLUGIN)

/// @brief Copies the session's points into the plugin's own type.
///
/// Two types with the same three fields, and the copy is the price of the dependency rule: `fit_session.hpp` may
/// not name anything from a plugin, or the Qt-free half of the view layer stops building where no plugin exists.
std::vector<qp::plugins::analysis::Point> to_analysis_points(const std::vector<model::FitPoint>& points) {
    std::vector<qp::plugins::analysis::Point> out;
    out.reserve(points.size());
    for (const model::FitPoint& p : points) out.push_back(qp::plugins::analysis::Point{p.x, p.y, p.sigma});
    return out;
}

#endif

/// @brief The name of a coefficient, from the **model** layer's one rule.
///
/// It used to be defined here, and moving it was part of exporting a fit: the table's `parameter` column has to say
/// `a` where the window says `a`, and two naming rules would drift the first time one of them changed.
QString coefficient_name(std::size_t index) {
    return QString::fromStdString(model::fit_coefficient_name(index));
}

}  // namespace

QString FitPanel::unavailable_text() {
    // A word rather than a dash, for the reason the confidence panel gives: `0` is a claim. "not fitted" says
    // the fit did not happen, which is a different statement from "the coefficient is zero".
    return QStringLiteral("not fitted");
}

FitPanel::FitPanel(model::FitSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    auto* layout = new QVBoxLayout(this);

    auto* title = new QLabel(tr("Fit"), this);
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    auto* controls = new QWidget(this);
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(0, 0, 0, 0);
    controls_layout->addWidget(new QLabel(tr("channel"), controls));
    channels_ = new QComboBox(controls);
    controls_layout->addWidget(channels_);
    controls_layout->addWidget(new QLabel(tr("degree"), controls));
    degree_ = new QSpinBox(controls);
    degree_->setRange(0, static_cast<int>(model::kMaximumFitDegree));
    degree_->setValue(1);
    controls_layout->addWidget(degree_);
    layout->addWidget(controls);

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("term"), tr("value"), tr("uncertainty")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(table_);

    excluded_ = new QLabel(this);
    excluded_->setWordWrap(true);
    excluded_->setStyleSheet(
        QStringLiteral("color: %1;").arg(qt::theme::to_qcolor(qt::theme::palette().warning).name()));
    layout->addWidget(excluded_);

    QObject::connect(channels_, &QComboBox::currentTextChanged, this, [this](const QString&) { refresh(); });
    QObject::connect(degree_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { refresh(); });

    show_channels();
}

void FitPanel::show_channels() {
    const QString previous = channels_->currentText();
    channels_->blockSignals(true);
    channels_->clear();
    for (const qp::runtime::Channel& channel : session_.trace().channels()) {
        channels_->addItem(QString::fromStdString(channel.name));
    }
    // The previous choice survives a rebuild when the trace still has it: a run that adds a channel must not
    // silently move the user's selection back to the first one.
    const int restored = channels_->findText(previous);
    if (restored >= 0) {
        channels_->setCurrentIndex(restored);
    } else if (channels_->count() > 0) {
        channels_->setCurrentIndex(0);
    }
    channels_->blockSignals(false);
    apply_selection();
    // And **render**, because a rebuilt list is a changed selection: the trace's channels do not exist until a run
    // declared them, so the refresh in the constructor ran against an empty list and left the summary blank. The
    // first version called only `apply_selection` here, and the panel then reported a refusal with no sentence
    // beside it -- the reason was in the model and on no screen, which is the defect this whole file is about.
    refresh();
}

std::string FitPanel::chosen_channel() const {
    return channels_->currentText().toStdString();
}

void FitPanel::choose_channel(const std::string& channel) {
    const int index = channels_->findText(QString::fromStdString(channel));
    if (index < 0) return;
    channels_->setCurrentIndex(index);
    apply_selection();
}

std::size_t FitPanel::chosen_degree() const { return static_cast<std::size_t>(degree_->value()); }

void FitPanel::choose_degree(std::size_t degree) {
    const int clamped = static_cast<int>(degree > model::kMaximumFitDegree ? model::kMaximumFitDegree : degree);
    degree_->setValue(clamped);
    apply_selection();
}

void FitPanel::apply_selection() {
    model::FitRequest request;
    request.channel = chosen_channel();
    request.degree = chosen_degree();
    // The stated uncertainties are taken as known, which is the convention a lab report uses: a student who
    // wrote down each reading's uncertainty meant it. The scaled convention is available in the model and is a
    // different question; it is not this panel's to answer, and a second number on screen with no explanation
    // is how a report ends up quoting the wrong one.
    request.scale_covariance = false;
    session_.choose(request);
}

std::vector<std::string> FitPanel::offered_channels() const {
    std::vector<std::string> names;
    for (int i = 0; i < channels_->count(); ++i) {
        names.push_back(channels_->itemText(i).toStdString());
    }
    return names;
}

void FitPanel::refresh() {
    apply_selection();
    const model::FitReport report = session_.report();

    // The summary always says what was asked and what was found, in that order, so a reader can tell "no fit"
    // from "a fit of something else".
    QStringList summary_lines;
    summary_lines << tr("%1 points, %2 parameters, %3 degrees of freedom")
                         .arg(report.points.size())
                         .arg(report.request.degree + 1)
                         .arg(report.degrees_of_freedom());

    if (report.refusal.has_value()) {
        summary_lines << tr("not fitted: %1").arg(QString::fromLatin1(model::to_string(*report.refusal)));
        // The channels list is still shown, so a user can see that the panel is alive and what it is offering.
        channels_->setEnabled(true);
        degree_->setEnabled(true);
        summary_->setText(summary_lines.join(QStringLiteral("\n")));
        table_->setRowCount(0);
        shown_rows_ = 0;
        // **The result goes with the table**, at every site that empties it: an export that wrote a fit the panel is
        // no longer showing would be a file about a state the user cannot see.
        result_.reset();
        excluded_->setText(exclusion_lines().join(QStringLiteral("\n")));
        excluded_->setVisible(!excluded_->text().isEmpty());
#if !defined(QP_HAS_ANALYSIS_PLUGIN)
        // Not the same statement as a refusal: the build cannot fit at all. Saying so is the difference between
        // "your data is not fittable" and "this build has no fitter", and a student who read the first when the
        // second is true would go looking for the problem in their own physics.
        summary_lines << tr("this build has no fit plugin");
#endif
        summary_->setText(summary_lines.join(QStringLiteral("\n")));
        return;
    }

#if defined(QP_HAS_ANALYSIS_PLUGIN)
    const auto fit = qp::plugins::analysis::fit_polynomial(to_analysis_points(report.points),
                                                           report.request.degree,
                                                           report.request.scale_covariance);
    if (!fit.has_value()) {
        // The fit itself refused. The code is shown verbatim rather than translated into a sentence, because the
        // model layer owns the wording of a refusal and this widget must not invent a second one.
        summary_lines << tr("the fit refused: %1").arg(QString::fromLatin1(qp::diag::to_string(fit.error())));
        summary_->setText(summary_lines.join(QStringLiteral("\n")));
        table_->setRowCount(0);
        shown_rows_ = 0;
        // **The result goes with the table**, at every site that empties it: an export that wrote a fit the panel is
        // no longer showing would be a file about a state the user cannot see.
        result_.reset();
        excluded_->setText(exclusion_lines().join(QStringLiteral("\n")));
        excluded_->setVisible(!excluded_->text().isEmpty());
        return;
    }

    const qp::runtime::FitResult& result = fit.value();
    result_ = result;
    const double reduced = qp::plugins::analysis::reduced_chi_squared(result).value_or(std::nan(""));
    summary_lines << tr("chi-squared %1 over %2 degrees of freedom")
                         .arg(number(result.chi_squared))
                         .arg(result.degrees_of_freedom);
    summary_lines << tr("reduced chi-squared %1").arg(number(reduced));
    if (result.r_squared.has_value()) {
        summary_lines << tr("R-squared %1").arg(number(*result.r_squared));
    } else {
        // Absent rather than zero: a coefficient of determination is undefined when every ordinate is the same,
        // and printing 0 would read as "the line explains nothing".
        summary_lines << tr("R-squared %1").arg(unavailable_text());
    }

    table_->setRowCount(static_cast<int>(result.coefficients.size()));
    shown_rows_ = result.coefficients.size();
    for (std::size_t i = 0; i < result.coefficients.size(); ++i) {
        table_->setItem(static_cast<int>(i), 0, new QTableWidgetItem(coefficient_name(i)));
        table_->setItem(static_cast<int>(i), 1, new QTableWidgetItem(number(result.coefficients[i])));
        // The uncertainty is never omitted. A coefficient without its error bar is the number a lab report
        // overstates, and this column is the reason the fit was worth doing at all.
        const std::optional<double> u = result.coefficient_uncertainty(i);
        table_->setItem(static_cast<int>(i), 2,
                        new QTableWidgetItem(u.has_value() ? number(*u) : unavailable_text()));
    }
#else
    summary_lines << tr("this build has no fit plugin");
    table_->setRowCount(0);
    shown_rows_ = 0;
#endif

    summary_->setText(summary_lines.join(QStringLiteral("\n")));
    excluded_->setText(exclusion_lines().join(QStringLiteral("\n")));
    excluded_->setVisible(!excluded_->text().isEmpty());
}

QString FitPanel::summary_text() const { return summary_->text(); }

QStringList FitPanel::exclusion_lines() const {
    const model::FitReport report = session_.report();
    QStringList lines;
    for (const std::pair<model::FitExclusion, std::size_t>& entry : report.excluded) {
        // Verbatim from the model, with the count: the wording of a refusal lives in the Qt-free half, and this
        // widget must not be a second place that decides what a gap is called.
        lines << tr("%1 readings left out: %2")
                     .arg(entry.second)
                     .arg(QString::fromLatin1(model::to_string(entry.first)));
    }
    return lines;
}

std::vector<std::vector<QString>> FitPanel::coefficient_rows() const {
    // A plain nested `std::vector` rather than `QStringList`: the accessor exists for the tests, and a Qt
    // container in its signature would force every caller to be a Qt translation unit. The widget itself is Qt;
    // what it hands out need not be.
    std::vector<std::vector<QString>> rows;
    for (int r = 0; r < table_->rowCount(); ++r) {
        std::vector<QString> row;
        for (int c = 0; c < table_->columnCount(); ++c) {
            const QTableWidgetItem* item = table_->item(r, c);
            row.push_back(item != nullptr ? item->text() : QString{});
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace qp::views
