/**
 * @file measurement_panel.hpp
 * @brief The measurement side of the window: readings, uncertainty, and the report.
 *
 * ## What this panel is for
 *
 * The platform's whole reason to exist is the loop **measure, record, quantify the
 * uncertainty, report**. The canvas and the property panel cover the graph; this covers the
 * experiment. It is the surface a student actually reads a number off, so it is also the
 * surface where the platform's honesty about uncertainty is either visible or lost.
 *
 * ## Why it is thin
 *
 * Everything it displays comes from `views::model::MeasurementModel`, which is Qt-free and
 * asserted by the ordinary suite on both compilers. This file builds widgets and formats
 * strings; it makes **no** decision about when a number is unknown. That split is
 * deliberate: the rule "unknown uncertainty is not zero uncertainty" is arithmetic and
 * bookkeeping, and a rule like that written inside a Qt slot is a rule nobody tests --
 * testing it costs a QApplication and an event loop, so it does not get tested, and it is
 * quietly wrong for a year.
 *
 * So the panel's contract with the model is: ask, do not derive. `gaps()` is rendered
 * verbatim rather than re-worded here, and `report_line()`'s empty optionals are rendered as
 * blanks rather than as zeros -- because a zero mean over no readings is a fabrication, and
 * it is the one a hurried implementation produces.
 *
 * ## What it deliberately does not do
 *
 * No plotting. Charter C5 makes the visual language part of a view plugin's acceptance, and
 * ADR-0006 bans Qt Charts (GPL-3.0) in favour of QCustomPlot or QWT. Drawing a curve with
 * the wrong licence is worse than not drawing one, so the trace is summarised numerically
 * and the plotting decision is left where it belongs.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         The model outlives this widget
 * @post        none
 * @invariant   No number shown here was computed in this file
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.measurement.gaps_are_shown_verbatim
 */
#pragma once

#include <QWidget>

#include <qp/graph/ir/ids.hpp>
#include <qp/views/model/measurement_model.hpp>

#include <optional>

class QLabel;
class QTableWidget;

namespace qp::views {

/**
 * @brief Readings table, a summary line, and the list of what is missing.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         `model` outlives this widget
 * @post        none
 * @invariant   The table shows `model.dataset().readings()` in order, rejected ones included
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.measurement.gaps_are_shown_verbatim
 */
class MeasurementPanel final : public QWidget {
    Q_OBJECT

public:
    explicit MeasurementPanel(qp::views::model::MeasurementModel& model, QWidget* parent = nullptr);

    /// @brief Rebuilds the table, the summary and the gap list from the model.
    ///
    /// Called by the window whenever the model changes rather than by the panel polling:
    /// a panel that refreshed on a timer would show a state that never existed.
    void refresh();

    /// @brief Adds one reading through the model and refreshes.
    ///
    /// Here rather than in the window so that the "record a reading" action and the table
    /// that displays it cannot disagree about which model they belong to.
    void record_reading(double value, qp::runtime::UncertaintyKind kind, double u);

    /// @brief The summary text the panel is currently showing.
    ///
    /// Exposed for the test, which asserts the panel renders what the model says rather
    /// than re-deriving it. A test that could only read pixels would have to be a Qt test,
    /// and this is the property most likely to rot.
    [[nodiscard]] QString summary_text() const;

    /// @brief The gap lines currently displayed, one per model gap, in the model's order.
    [[nodiscard]] QStringList gap_lines() const;

    /**
     * @brief The source of the reading selected in the table, or nothing.
     *
     * The direction the loop was missing: every other path in this window goes **from** the graph **to** the
     * numbers, and this is the one that goes back. A student reading "0.4998 +/- 0.0003" should be able to ask
     * which device produced it without remembering which node they ran.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        The source of the selected row when it has one, otherwise nothing
     * @invariant   Agrees with `MeasurementModel::source_of` for the selected row
     * @errors      Reports nothing: a row with no source and no selected row are the same answer here, which is
     *              why the return is an optional rather than a validity flag
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.measurement.a_reading_points_at_its_node
     */
    [[nodiscard]] std::optional<qp::runtime::Measurement::Source> selected_source() const;

Q_SIGNALS:
    /**
     * @brief Emitted when the user selects a row, naming the node that produced that reading.
     *
     * Only for a row that **has** a source. A reading typed in by hand has none, and emitting an invalid id would
     * make the window clear its selection to point at nothing -- which reads as "the reading was discarded".
     *
     * @param node The node that produced the selected reading. Always valid when emitted.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         The selected row has a source
     * @post        A receiver can reveal and select `node`
     * @invariant   Never emitted for a reading with no source
     * @errors      None: a signal carries no failure, and a row with nothing behind it is reported by silence
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.measurement.a_reading_points_at_its_node
     */
    void reading_selected(qp::graph::NodeId node);

private:
    /// @brief Wires the table's selection to `reading_selected`.
    void on_row_selected(int row);

    qp::views::model::MeasurementModel& model_;
    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* gaps_ = nullptr;
};

}  // namespace qp::views
