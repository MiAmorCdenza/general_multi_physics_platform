/**
 * @file fit_panel.hpp
 * @brief The end of the loop: a fitted line, its coefficients, and their uncertainties.
 *
 * ## What this panel adds that the others do not
 *
 * `measurement_panel` reports what was recorded and how well each reading is known; `confidence_panel` says which
 * part of a run is arithmetic rather than physics. Neither answers the question a lab report is actually graded
 * on -- **what does the line through these points say, and how well does it say it** -- and that is
 * `plugins/analysis`'s `fit_polynomial`: weighted least squares, a full covariance matrix, chi-squared and R².
 *
 * Until this panel existed, that module was reachable only from its own tests. The platform's differentiator is
 * "measure, record, quantify the uncertainty, **report**", and a report with no fit in it is a table a student
 * copies into a spreadsheet -- which is precisely the workflow this platform exists to replace.
 *
 * ## Why the panel is thin, and where the two halves live
 *
 * Everything about **what may be fitted** is `views/model/fit_session.hpp`, which is Qt-free and asserted on both
 * compilers: which readings become points, which are excluded and why, and whether the degree asked for leaves a
 * degree of freedom. This file renders that report and calls the fit. It makes no decision about a weight, and it
 * cannot: `FitReport::points` carries only readings whose uncertainty was quantified.
 *
 * ## Why the fit itself is called here rather than in the model
 *
 * A dependency decision, not a layering preference. `views/model` is built **unconditionally**, while
 * `plugins/analysis` exists only under `QP_BUILD_PLUGINS`: linking the Qt-free half of the view layer against a
 * plugin would make a plugins-off build fail to configure, and that configuration is a real one. So the call
 * sits here, behind `QP_HAS_ANALYSIS_PLUGIN`, and a build without the plugin says so on screen rather than
 * showing an empty table that reads as "the fit failed".
 *
 * @ownership   owns
 * @thread      ui
 * @pre         The session's trace outlives this widget
 * @post        none
 * @invariant   No number shown here was computed in this file
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.fit.shows_the_models_report,
 *              qt.views.fit.a_refusal_is_shown_by_name,
 *              qt.views.fit.coefficients_come_with_their_uncertainties
 */
#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

#include <qp/views/model/fit_session.hpp>

#include <cstddef>
#include <string>
#include <vector>
class QComboBox;
class QLabel;
class QSpinBox;
class QTableWidget;

namespace qp::views {

/**
 * @brief The fit controls, the coefficients table, and the summary a report quotes.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         `session` outlives this widget
 * @post        none
 * @invariant   What is displayed is `session.report()` and the fit of its points, and nothing else
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.fit.shows_the_models_report,
 *              qt.views.fit.a_refusal_is_shown_by_name
 */
class FitPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FitPanel(qp::views::model::FitSession& session, QWidget* parent = nullptr);

    /**
     * @brief Fills the channel list from the fitted trace, and chooses the first channel.
     *
     * Called by the window when the trace changes rather than by the panel polling. A channel list built once in
     * the constructor would be empty for the whole session: the trace has no channels until a run declares them,
     * which is after the window is built.
     *
     * The channel names come from `session.trace().channels()`, so the list is the trace's own answer. An empty
     * list is a legitimate state -- a window that has not run anything -- and the panel says so rather than
     * showing a blank box.
     *
     * @ownership   observes the session's trace for the duration of the call
     * @thread      ui
     * @pre         none
     * @post        The list holds one entry per trace channel, in declaration order
     * @invariant   Selecting an entry changes nothing outside this widget
     * @errors      Reports nothing: a widget cannot fail to be filled
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.fit.shows_the_models_report
     */
    void show_channels();

    /// @brief The channel the user has chosen, or an empty string.
    [[nodiscard]] std::string chosen_channel() const;

    /**
     * @brief Chooses a channel by name, so a caller can drive the same path the list does.
     *
     * @param channel The channel name. A name the trace does not have leaves the choice unchanged.
     *
     * @ownership   observes `channel` for the call
     * @thread      ui
     * @pre         none
     * @post        `chosen_channel() == channel` when the trace has such a channel
     * @invariant   The session's trace is not touched
     * @errors      Reports nothing: a name that is not there leaves the choice as it was
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.fit.shows_the_models_report
     */
    void choose_channel(const std::string& channel);

    /// @brief The polynomial degree currently selected.
    [[nodiscard]] std::size_t chosen_degree() const;

    /// @brief Selects a degree, clamped to `kMaximumFitDegree`.
    void choose_degree(std::size_t degree);

    /**
     * @brief Runs the fit and rebuilds every widget from the result.
     *
     * Runs on the **current** selection, so a caller that has just changed the channel or the degree calls this
     * once. The order is: read the session's report, refuse by name if it is not fittable, otherwise call the fit
     * and render it -- so there is exactly one place where a fit can fail to happen, and it is not a Qt slot that
     * swallows the reason.
     *
     * @ownership   owns
     * @thread      ui
     * @pre         none
     * @post        The table shows the report's coefficients, or the reason there are none
     * @invariant   The displayed text is derived from the report and the fit, never recomputed
     * @errors      Reports nothing: a refusal is a state this panel exists to display
     * @complexity  O(points * degree^2 + degree^3)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.fit.shows_the_models_report,
     *              qt.views.fit.a_refusal_is_shown_by_name,
     *              qt.views.fit.coefficients_come_with_their_uncertainties
     */
    void refresh();

    /// @brief The text of the summary line, as displayed.
    [[nodiscard]] QString summary_text() const;

    /// @brief The lines of the "what was left out" list, in the report's order.
    [[nodiscard]] QStringList exclusion_lines() const;

    /// @brief The coefficient rows currently displayed: `name`, `value`, `uncertainty`.
    ///
    /// Exposed for the tests, which assert that a coefficient is never shown without its uncertainty. A test that
    /// could only read pixels would have to be a Qt rendering test, and this is the property most likely to rot:
    /// a coefficient printed alone is the single most common way a lab report overstates its own precision.
    [[nodiscard]] std::vector<std::vector<QString>> coefficient_rows() const;

    /// @brief What the panel shows when there is no fit to show. Never a zero.
    ///
    /// Static and exposed for the same reason `ConfidencePanel::unavailable_text` is: the distinction between
    /// "not fitted" and "fitted, and the answer is zero" is the one this platform is built on, and a test has to
    /// be able to name it.
    [[nodiscard]] static QString unavailable_text();

    /// @brief Pushes the widget's current selection into the session.
    ///
    /// Called by every control change, by `show_channels`, and by `refresh` -- and public because a caller that
    /// has just changed the **trace**, which is what a run does, has changed the available channels without
    /// touching a widget. The channel list is the caller's to rebuild: the panel does not own the trace and must
    /// not go looking for changes in it.
    void apply_selection();

    /// @brief The channel names the widget is currently offering, in the trace's order.
    ///
    /// Exposed so a case can assert that the list is the **trace's** rather than a constant, and that a rebuild
    /// keeps the user's choice when the channel it named is still there.
    [[nodiscard]] std::vector<std::string> offered_channels() const;

private:
    qp::views::model::FitSession& session_;
    QComboBox* channels_ = nullptr;
    QSpinBox* degree_ = nullptr;
    QLabel* summary_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* excluded_ = nullptr;
    /// How many coefficient rows the table currently has, so a shorter fit clears the rest rather than leaving
    /// the previous degree's rows behind -- which would show two models at once.
    std::size_t shown_rows_ = 0;
};

}  // namespace qp::views
