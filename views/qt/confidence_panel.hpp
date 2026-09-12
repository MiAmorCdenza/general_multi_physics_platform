/**
 * @file confidence_panel.hpp
 * @brief C8's panel: the numbers that say whether a run is physics or arithmetic.
 *
 * ## What it shows and why it is this and not a plot
 *
 * Charter C8 requires that energy drift and field divergence be **visible**, so that numerical
 * error is not mistaken for physics. The easy misreading is that this means a graph; it does not.
 * A plot of `x(t)` for a decaying oscillation looks identical whether the decay is damping in the
 * model or dissipation in RK4, which is precisely the confusion the clause exists to prevent. What
 * resolves it is a **number that should have been constant and was not**, with a sentence naming
 * the mechanism.
 *
 * So the panel is a small table of diagnostics and a list of notes, and `views/model/confidence_model`
 * decides every value and every word. This file builds widgets.
 *
 * ## Where the note text comes from
 *
 * `ConfidenceModel::notes()` verbatim, the same rule the measurement panel follows for `gaps()`.
 * Re-wording a note here would mean two places decide what a warning says, and the one the ordinary
 * suite asserts on both compilers -- the model's -- would be the one nobody reads.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         The model outlives this widget
 * @post        none
 * @invariant   No number shown here was computed in this file
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.confidence.shows_the_models_notes,
 *              qt.views.confidence.unmeasurable_is_not_zero,
 *              qt.views.confidence.energy_drift_is_shown
 */
#pragma once

#include <QWidget>

#include <qp/views/model/confidence_model.hpp>

class QLabel;
class QTableWidget;

namespace qp::views {

/**
 * @brief Diagnostics table plus the model's notes, in a dock.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         `model` outlives this widget
 * @post        none
 * @invariant   Every displayed value comes from `model.report()` or `model.notes()`
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.confidence.shows_the_models_notes,
 *              qt.views.confidence.unmeasurable_is_not_zero,
 *              qt.views.confidence.energy_drift_is_shown
 */
class ConfidencePanel final : public QWidget {
    Q_OBJECT

public:
    explicit ConfidencePanel(qp::views::model::ConfidenceModel& model, QWidget* parent = nullptr);

    /// @brief Rebuilds the table and the note list from the model.
    ///
    /// Called by the window when a run finishes rather than on a timer: a panel that refreshed
    /// periodically would show states the model never had, and the clamp count arrives in one piece
    /// at the end of a run.
    void refresh();

    /// @brief How the panel renders a missing diagnostic. Exposed so the test can assert the
    ///        distinction the whole feature rests on: an unaskable quantity is shown as **not
    ///        measurable**, never as `0`.
    [[nodiscard]] static QString unavailable_text();

    /// @brief The note lines currently displayed, one per model note, in the model's order.
    [[nodiscard]] QStringList note_lines() const;

    /// @brief The text in the row named `row_name`, or an empty string when there is no such row.
    [[nodiscard]] QString row_text(const QString& row_name) const;

private:
    qp::views::model::ConfidenceModel& model_;
    QTableWidget* table_ = nullptr;
    QLabel* notes_ = nullptr;
};

}  // namespace qp::views
