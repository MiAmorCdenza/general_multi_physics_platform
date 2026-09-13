/**
 * @file fit_session.hpp
 * @brief A trace channel as a set of fit points: which readings may enter a fit, and why the rest may not.
 *
 * ## What this file is, and what it deliberately is not
 *
 * The loop this platform exists for ends in a **report**, and the single most common piece of analysis in a
 * first-year lab report is a straight line through a set of measurements: the gradient is the physical constant
 * and its uncertainty is what the report is graded on. `plugins/analysis` performs that fit, and this file
 * prepares it -- turning a recorded trace into the `(x, y, sigma)` triples a weighted least-squares fit needs.
 *
 * It does **not** perform the fit, and that is a dependency decision rather than a layering preference.
 * `views/model` is built **unconditionally** (`views/CMakeLists.txt` says why: it contains no Qt, so gating it
 * behind `QP_BUILD_VIEWS` would be a lie), while every plugin library exists only under `QP_BUILD_PLUGINS`. A
 * `views_model -> plugins_analysis` link would therefore make the Qt-free half of the view layer fail to
 * configure in a plugins-off build -- a real configuration, and the one `check_core_standalone.py` guards. So
 * `views/qt/fit_panel.cpp` calls the fit, under `QP_HAS_ANALYSIS_PLUGIN`, and the decision of **what may be
 * fitted** stays here where both compilers assert it.
 *
 * ## The rule this file enforces, and the number it refuses to invent
 *
 * A weighted fit needs a weight per point, and the weight is `1/sigma^2`. Three states of a reading therefore
 * have three different answers, and conflating them is the defect this file exists to prevent:
 *
 *   - `standard` and `sigma > 0`: a point, with the weight the instrument's own uncertainty implies;
 *   - `unknown`: the reading is real and nobody quantified its error. There is no honest `sigma` to use.
 *     Substituting `1.0` would make the point's weight a number about the **scale** of the plotted axis rather
 *     than about the measurement: a trace of metres would be fitted as though every sample were uncertain by a
 *     metre, and the slope would come out wrong in a way nothing on screen reports;
 *   - `exact`, and `standard` with `sigma == 0`: the reading claims to know itself perfectly. That is a true
 *     statement about a counted quantity -- twelve periods, three trials -- and it is **not** a usable weight:
 *     `1/0^2` is an infinite weight, and one such point would become the whole answer.
 *
 * So the two latter cases are **excluded and named**, each with its own reason, rather than averaged into a
 * default. A fit that silently decides an uncertainty is a fit whose gradient nobody can defend, which is
 * precisely the artefact a spreadsheet produces and this platform replaces.
 *
 * ## Where the abscissa comes from
 *
 * From the trace's own time column: sample `t` is the abscissa and the channel's value is the ordinate. That is
 * the fit an experiment actually produces -- a signal sampled against time -- and using the sample **index**
 * instead would make a fit over a non-uniform sampling silently wrong, because a least-squares line assumes the
 * abscissae are what they say they are.
 *
 * The consequence is worth stating: a linear fit's coefficients carry **different dimensions**, the intercept
 * the channel's and the slope the channel's per second, and the covariance matrix mixes them. That is why
 * `FitReport::dim` is reported rather than a formatted answer -- a caller printing "1.234" without saying
 * whether that is metres or metres per second has produced the number and lost the measurement.
 *
 * ## The covariance convention is not chosen here
 *
 * `plugins/analysis` offers two: `(A^T W A)^-1`, which takes the stated uncertainties as known, and the same
 * scaled by `chi_squared / dof`, for the case where they are not to be trusted. Which one a report wants is a
 * question about the data, not about this layer, so `FitRequest::scale_covariance` carries the caller's answer
 * through rather than this file picking one. Two conventions one number apart, and a caller that has to choose
 * has to know which question it is asking.
 *
 * @ownership   observes (holds a reference to the trace; owns no samples)
 * @thread      ui
 * @pre         The referenced trace outlives the session
 * @post        none
 * @invariant   `report()` is a pure function of the trace and the request
 * @errors      See each declaration
 * @frozen      no
 * @tests       fit.session.points_come_from_a_named_channel,
 *              fit.session.an_unknown_uncertainty_is_excluded,
 *              fit.session.an_exact_reading_has_no_weight,
 *              fit.session.no_channel_is_refused_by_name,
 *              fit.session.degrees_of_freedom_decide_eligibility
 */
#pragma once

#include <qp/ports/value.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/runtime/trace/trace.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qp::views::model {

/**
 * @brief The highest polynomial degree this session will prepare.
 *
 * Twelve, and the number is not arbitrary: a Vandermonde column is `t^degree`, and beyond that the column
 * overflows a double for any plausible `t` -- so the limit is where the arithmetic stops being meaningful
 * rather than where a user interface stops offering buttons. `plugins/analysis` refuses the same bound for the
 * same reason, and stating it in both places is deliberate: this one is about what may be **prepared**.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Constant
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.session.degrees_of_freedom_decide_eligibility
 */
inline constexpr std::size_t kMaximumFitDegree = 12;

/**
 * @brief Whether a polynomial of `degree` could be fitted to `points` samples at all.
 *
 * `degree + 1` parameters need at least that many points, and a fit with exactly as many points as parameters
 * has **zero** degrees of freedom: it passes through every point and has said nothing. That is not a failure of
 * the arithmetic -- it is a real answer to a question nobody should ask, and a report showing it as a perfect
 * fit is the failure mode this whole platform is about. So this predicate requires at least one degree of
 * freedom, not merely a solvable system.
 *
 * A free function rather than a member, because the caller asking "would a quadratic work here" has not built a
 * session yet, and a caller that had to construct one to ask would then have to keep it alive to justify the
 * answer.
 *
 * @param points How many readings would enter the fit.
 * @param degree The highest power to fit.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        False exactly when `degree` is out of range, or `points <= degree + 1`
 * @invariant   Monotone in `degree`: a higher degree never becomes eligible against fewer points
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       fit.session.degrees_of_freedom_decide_eligibility
 */
[[nodiscard]] constexpr bool degree_is_fittable(std::size_t points, std::size_t degree) noexcept {
    return degree <= kMaximumFitDegree && points > degree + 1;
}

/**
 * @brief Why a reading did not become a fit point.
 *
 * Named reasons rather than a count, because the three states want three different sentences in a report and a
 * caller that only had a total could not tell a student what to do about it. "Three readings have no stated
 * uncertainty, so measure again or enter one" is actionable; "three readings were skipped" is not.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One enumerator per distinct situation
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.session.an_unknown_uncertainty_is_excluded
 */
enum class FitExclusion : std::uint8_t {
    /// `UncertaintyKind::unknown`: nobody quantified this reading's error.
    uncertainty_unknown = 0,
    /// A quantified uncertainty of zero: `1/0^2` is an infinite weight, so the point cannot be used.
    uncertainty_zero = 1,
    /// The reading itself, or its time, is not a finite number.
    value_not_finite = 2,
};

/**
 * @brief Stable short name of an exclusion, for a message or a log line.
 *
 * @param reason The exclusion to name.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        One of the names `to_string` writes, never null
 * @invariant   Total: every enumerator has a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       fit.session.an_unknown_uncertainty_is_excluded
 */
[[nodiscard]] const char* to_string(FitExclusion reason) noexcept;

/**
 * @brief Why a channel cannot be fitted at all.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One enumerator per distinct situation
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.session.no_channel_is_refused_by_name
 */
enum class FitRefusal : std::uint8_t {
    /// Nothing is being asked for: the caller has not chosen a channel yet.
    no_channel = 0,
    /// The trace has no channel under that name.
    channel_not_found = 1,
    /// The trace has no samples, so there is nothing to fit.
    no_samples = 2,
    /// The degree is out of range, or the eligible points do not leave a degree of freedom.
    not_enough_points = 3,
};

/**
 * @brief Stable short name of a refusal, for a message or a log line.
 *
 * @param reason The refusal to name.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        One of the names in this declaration's enumerator list, never null
 * @invariant   Total: every enumerator has a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       fit.session.no_channel_is_refused_by_name
 */
[[nodiscard]] const char* to_string(FitRefusal reason) noexcept;

/**
 * @brief What to fit: a channel, a degree, and the covariance convention.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   `degree <= kMaximumFitDegree` for a request that can be satisfied
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.session.points_come_from_a_named_channel
 */
struct FitRequest final {
    /// The trace channel to fit, by name. Empty means nothing has been chosen.
    std::string channel{};
    /// The highest power of `t` to fit. `1` is a straight line, `0` a weighted mean.
    std::size_t degree = 1;
    /// Whether the covariance should be scaled by `chi_squared / dof`. See the file comment: the convention is
    /// the caller's decision, and it is carried rather than assumed.
    bool scale_covariance = false;

    [[nodiscard]] friend bool operator==(const FitRequest& a, const FitRequest& b) noexcept {
        return a.channel == b.channel && a.degree == b.degree &&
               a.scale_covariance == b.scale_covariance;
    }
    [[nodiscard]] friend bool operator!=(const FitRequest& a, const FitRequest& b) noexcept {
        return !(a == b);
    }
};

/**
 * @brief One reading, ready for a weighted least-squares fit.
 *
 * A separate type from `plugins::analysis::Point` even though the three fields are identical, and the reason is
 * the dependency rule above: this header may not name anything from a plugin, or the Qt-free half of the view
 * layer stops building in a plugins-off configuration. The copy is three doubles, once per fit.
 *
 * @ownership   pure
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   `sigma > 0` for a point in `FitReport::points`
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.session.points_come_from_a_named_channel
 */
struct FitPoint final {
    /// The sample's time, in seconds.
    double x = 0.0;
    /// The channel's value at that sample.
    double y = 0.0;
    /// The reading's own standard uncertainty. Positive by construction here.
    double sigma = 1.0;
};

/**
 * @brief What a fit would be run on: the points, the parameters, and everything left out.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   `points.size() + excluded == sample_count`, and `dim` is the fitted channel's
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.session.points_come_from_a_named_channel,
 *              fit.session.an_exact_reading_has_no_weight
 */
struct FitReport final {
    /// The request this report answers.
    FitRequest request{};
    /// The dimension of the fitted channel -- of the **ordinate**. The abscissa is seconds, so a linear fit's
    /// slope is this dimension per second and its intercept is this dimension. Reported rather than assumed
    /// because a coefficient without its unit is a number, not a measurement.
    qp::units::Dim dim{};
    /// How many samples the trace holds.
    std::size_t sample_count = 0;
    /// The readings that will enter the fit, in trace order.
    std::vector<FitPoint> points{};
    /// How many readings were left out, and why. Counts, in `FitExclusion` order.
    std::vector<std::pair<FitExclusion, std::size_t>> excluded{};
    /// Nothing when the fit can run; the reason it cannot otherwise.
    std::optional<FitRefusal> refusal{};

    /**
     * @brief How many readings were left out, of every reason.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        The sum of every count in `excluded`
     * @invariant   `points.size() + excluded_count() == sample_count` for a report over a consistent trace
     * @errors      noexcept
     * @complexity  O(exclusions)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.an_unknown_uncertainty_is_excluded
     */
    [[nodiscard]] std::size_t excluded_count() const noexcept;

    /**
     * @brief The number of readings left out for one reason.
     *
     * @param reason The exclusion to count. A reason that does not apply counts zero, which is a real answer.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        Zero when no reading was excluded for that reason
     * @invariant   Never larger than `excluded_count()`
     * @errors      noexcept
     * @complexity  O(exclusions)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.an_unknown_uncertainty_is_excluded
     */
    [[nodiscard]] std::size_t excluded_count(FitExclusion reason) const noexcept;

    /**
     * @brief Whether a fit can be run on this report.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        True exactly when `refusal` is absent
     * @invariant   A fittable report has more points than parameters
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.degrees_of_freedom_decide_eligibility
     */
    [[nodiscard]] bool fittable() const noexcept { return !refusal.has_value(); }

    /**
     * @brief `points - (degree + 1)`, the divisor of a reduced chi-squared.
     *
     * Zero rather than a negative number when the points do not exceed the parameters, and the reader is
     * expected to check `fittable()` first: a negative degree of freedom is not a quantity, and a caller that
     * divided by it would produce a number that looks like a fit quality and is not one.
     *
     * @ownership   pure
     * @thread      ui
     * @pre         none
     * @post        `points.size() - (request.degree + 1)`, or zero when that would be negative
     * @invariant   Zero exactly when a fit has no freedom left
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.degrees_of_freedom_decide_eligibility
     */
    [[nodiscard]] std::size_t degrees_of_freedom() const noexcept;
};

/**
 * @brief Reads one trace channel as a set of fit points.
 *
 * @ownership   observes `trace`
 * @thread      ui
 * @pre         `trace` outlives this object
 * @post        none
 * @invariant   The same trace and request give the same report
 * @errors      See each declaration
 * @frozen      no
 * @tests       fit.session.points_come_from_a_named_channel
 */
class FitSession final {
public:
    /**
     * @brief A session over one trace.
     *
     * @param trace The run's samples, **borrowed**. Borrowed rather than copied for the reason the confidence
     *              model gives: a fit of a copy of the data is a fit of something else, and a student comparing
     *              the fitted gradient against the trace on screen would be comparing two different runs.
     *
     * @ownership   observes `trace`
     * @thread      ui
     * @pre         `trace` outlives this session
     * @post        `request().channel` is empty and `request().degree` is `1`
     * @invariant   The trace reference is never stored past this object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.points_come_from_a_named_channel
     */
    explicit FitSession(const qp::runtime::Trace& trace) noexcept : trace_(&trace) {}

    /**
     * @brief The trace this session reads.
     *
     * @ownership   borrows
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   The same object passed to the constructor
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.points_come_from_a_named_channel
     */
    [[nodiscard]] const qp::runtime::Trace& trace() const noexcept { return *trace_; }

    /**
     * @brief What the session has been asked to fit.
     *
     * @ownership   borrows from this object
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   Stable until the next mutation
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.points_come_from_a_named_channel
     */
    [[nodiscard]] const FitRequest& request() const noexcept { return request_; }

    /**
     * @brief Chooses a channel to fit, and how.
     *
     * The degree is clamped to `kMaximumFitDegree` rather than refused: a caller asking for a degree that cannot
     * exist has made an arithmetic mistake, and the honest response is to fit the highest degree that can be
     * fitted and report the degree that was used -- which `report().request.degree` does. A refusal would leave
     * the caller to guess a valid value.
     *
     * @param request The channel, degree and covariance convention.
     *
     * @ownership   owns (the request is copied into this object)
     * @thread      ui
     * @pre         none
     * @post        `request().channel == request.channel` and `request().degree <= kMaximumFitDegree`
     * @invariant   Choosing a request changes nothing in the trace
     * @errors      noexcept
     * @complexity  O(channel name)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.no_channel_is_refused_by_name
     */
    void choose(FitRequest request) noexcept;

    /**
     * @brief Reads the chosen channel as fit points, and says what it left out.
     *
     * @ownership   owns the result
     * @thread      ui
     * @pre         none
     * @post        On a fittable report, `points.size()` exceeds `request().degree + 1`
     * @invariant   Every sample is either a point or counted in exactly one exclusion
     * @errors      May allocate; allocation failure terminates, as elsewhere in this project
     * @complexity  O(samples)
     * @nondet      none
     * @frozen      no
     * @tests       fit.session.points_come_from_a_named_channel,
     *              fit.session.an_unknown_uncertainty_is_excluded,
     *              fit.session.an_exact_reading_has_no_weight,
     *              fit.session.no_channel_is_refused_by_name,
     *              fit.session.degrees_of_freedom_decide_eligibility
     */
    [[nodiscard]] FitReport report() const;

private:
    /// @brief The index of the channel named `name`, or nothing.
    [[nodiscard]] std::optional<std::size_t> channel_index(const std::string& name) const noexcept;

    const qp::runtime::Trace* trace_;
    FitRequest request_{};
};

/**
 * @brief The name of a fitted coefficient: `a` for a line's intercept, then `b`, `c`, ...
 *
 * **In the model layer because two things need it now.** The fit panel has named its rows this way since it was
 * written, and the export of a fit has to name them the same way -- a table whose `parameter` column says `k0` where
 * the window says `a` is a table a reader cannot line up with what they saw. One rule, one place.
 *
 * Past `z` the names become `c26`, `c27`, ...: never `{`, and never a name that depends on how many coefficients
 * happen to exist.
 *
 * @param index Zero-based coefficient index.
 *
 * @ownership   owns the result
 * @thread      any
 * @pre         none
 * @post        A short stable name, distinct for every index
 * @invariant   The same index gives the same name in every build and every session
 * @errors      May allocate; allocation failure terminates
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes -- a saved report quotes these names
 * @tests       fit.session.coefficients_are_named_the_same_way_everywhere
 */
[[nodiscard]] std::string fit_coefficient_name(std::size_t index);

}  // namespace qp::views::model
