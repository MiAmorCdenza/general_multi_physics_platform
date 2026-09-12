/**
 * @file fit.hpp
 * @brief Least squares: the first thing in this repository that **produces** a `FitResult`.
 *
 * ## The gap this closes
 *
 * `runtime/store` has had `FitResult` since the store was written -- coefficients, a full covariance matrix,
 * residuals, chi-squared, degrees of freedom, R-squared -- and until this file existed **nothing in the
 * repository produced one and nothing consumed one**. The plan tree recorded it as a gap with a reopening
 * condition, and the condition was this: the first real demand for a fit. A lab course's demand is not
 * hypothetical, because a straight line through a set of measurements is the single most common piece of
 * analysis a first-year student does, and the numbers that come out of it are exactly the ones a report is
 * graded on: the gradient (which is the physical constant), its uncertainty, and whether the line is actually
 * straight.
 *
 * So this module is content, and the interface it fills was written before it -- `FitResult` is unchanged,
 * `store` gained nothing, and the whole module is one header and one source file. That is the evidence for the
 * framework's own claim: adding a category of content did not need a new interface.
 *
 * ## Weighted, and why that is not a refinement
 *
 * A measurement whose uncertainty is unknown is not a measurement with zero uncertainty. If a fit treated every
 * point as equally good, then a reading taken with a metre rule would pull the line exactly as hard as one taken
 * with a caliper -- and the fitted gradient would depend on which points happened to be in the table rather than
 * on how well each was known. So each point carries a **weight** `w = 1/sigma^2`, the normal equations are
 * `A^T W A x = A^T W y`, and a point with `sigma = 0` is refused rather than trusted: an infinite weight would
 * make one reading the whole answer.
 *
 * ## What the covariance is, and what it is not
 *
 * The covariance matrix is `(A^T W A)^-1` -- the coefficients' own covariance under the standard linear model,
 * in which the weights are taken as **known** and the scatter of the points about the line is what the weights
 * already describe. This is the convention a lab report uses and the one `FitResult::coefficient_uncertainty`
 * documents.
 *
 * When the weights are *not* trustworthy -- a student who wrote the same resolution in every `sigma` box
 * without thinking -- the honest answer is different and this file provides it under its own name rather than
 * silently switching: `scaled_covariance` reports `(A^T W A)^-1 * chi_squared / dof`, the estimate that lets the
 * data say how scattered it really is. Two conventions, two names, one number apart, and a caller that has to
 * pick one has to know which question it is asking.
 *
 * ## What is deliberately absent
 *
 * No model that is not a polynomial in its parameters, and no solver other than the normal equations. Both are
 * decisions with a reason:
 *
 *   - a **nonlinear** fit (an exponential decay, a resonance curve) needs an iteration and an initial guess, and
 *     an initial guess is a thing a student has to choose and defend. That is a different user interface and a
 *     different set of numerical questions, and guessing at it here would make this file's contract vague;
 *   - the **normal equations square the condition number**. For the degrees a lab exercise uses -- a line, a
 *     quadratic, occasionally a cubic -- that is not observable, and `fit.analysis.a_line_through_nearly_equal_x`
 *     measures it rather than assuming it. A degree high enough to matter is a degree no first-year experiment
 *     asks for, and the reopening condition is written down rather than hidden.
 *
 * @ownership   owns
 * @thread      main (no globals, so it is also safe from several threads on distinct data)
 * @pre         none
 * @post        none
 * @invariant   A returned `FitResult` has `residuals.size() == x.size()` and a square covariance
 * @errors      See each declaration
 * @frozen      no
 * @tests       fit.a_line_through_exact_points_is_exact,
 *              fit.analysis.weights_decide_the_line,
 *              fit.analysis.an_unknown_uncertainty_is_refused,
 *              fit.analysis.nearly_equal_abscissae_still_fit,
 *              fit.analysis.a_quadratic_recovers_its_own_coefficients,
 *              fit.analysis.chi_squared_measures_the_scatter,
 *              fit.analysis.r_squared_is_absent_when_it_is_undefined,
 *              fit.analysis.scaling_the_covariance_answers_a_different_question,
 *              fit.analysis.a_broken_input_is_refused
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/runtime/store/store.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace qp::plugins::analysis {

/**
 * @brief One point of a fit: an abscissa, an ordinate, and how well the ordinate is known.
 *
 * @ownership   owns
 * @thread      main
 * @pre         `sigma > 0` for a point that is to be used
 * @post        none
 * @invariant   The weight the fit uses is `1 / sigma^2`
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.a_line_through_exact_points_is_exact
 */
struct Point final {
    /// The independent variable. Exact and chosen, not measured -- which is why it carries no uncertainty.
    double x = 0.0;
    /// The dependent variable, as measured.
    double y = 0.0;
    /// The standard uncertainty of `y`. Must be positive: an unknown uncertainty is not a zero one.
    double sigma = 1.0;
};

/**
 * @brief How many points and parameters a fit had, for a report.
 *
 * Separate from `FitResult` because `FitResult` is frozen and belongs to `store`, and because these are facts
 * about the **request** rather than about the answer: a caller printing "12 points, 2 parameters, 10 degrees of
 * freedom" is describing what it asked for.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `parameters == coefficients_expected`
 * @errors      noexcept
 * @frozen      no
 * @tests       fit.analysis.chi_squared_measures_the_scatter
 */
struct FitSummary final {
    /// Points that entered the fit.
    std::size_t points = 0;
    /// Free parameters the model fitted.
    std::size_t parameters = 0;
    /// `points - parameters`, the divisor in a reduced chi-squared.
    std::size_t degrees_of_freedom = 0;
};

/**
 * @brief Fits `y = sum_k c_k * x^k` to `points` by weighted least squares.
 *
 * The workhorse of a first-year lab report, and the reason it is a named function rather than a matrix the
 * caller assembles: the caller has a column of readings from a ruler, not a design matrix.
 *
 * @param points      The measurements, in any order. The residuals come back in this order.
 * @param degree      The highest power fitted. `1` is a straight line, `0` a weighted mean.
 * @param scale_covariance When true the covariance is scaled by `chi_squared / dof` -- the estimate for the
 *                   case where the stated uncertainties are not to be trusted. See the file comment.
 *
 * @ownership   owns the returned result
 * @thread      main
 * @pre         `degree <= 12`, so a Vandermonde column cannot overflow
 * @post        On success `coefficients.size() == degree + 1`, `residuals.size() == points.size()`, the
 *              covariance is square with that side, and `degrees_of_freedom == points - parameters`
 * @invariant   The fitted line passes through the weighted centroid of the data
 * @errors      `invalid_argument` for fewer points than parameters, a non-positive or non-finite `sigma`, a
 *              non-finite value anywhere, or a degree out of range; `fit_failed` when the abscissae do not
 *              determine the coefficients -- every `x` equal, for a line
 * @complexity  O(n * degree^2 + degree^3)
 * @nondet      none
 * @frozen      no
 * @tests       fit.a_line_through_exact_points_is_exact,
 *              fit.analysis.weights_decide_the_line,
 *              fit.analysis.an_unknown_uncertainty_is_refused,
 *              fit.analysis.nearly_equal_abscissae_still_fit,
 *              fit.analysis.a_quadratic_recovers_its_own_coefficients,
 *              fit.analysis.chi_squared_measures_the_scatter,
 *              fit.analysis.r_squared_is_absent_when_it_is_undefined,
 *              fit.analysis.scaling_the_covariance_answers_a_different_question,
 *              fit.analysis.a_broken_input_is_refused
 */
[[nodiscard]] diag::Result<runtime::FitResult> fit_polynomial(const std::vector<Point>& points,
                                                              std::size_t degree,
                                                              bool scale_covariance = false);

/**
 * @brief Fits `y = sum_k c_k * basis_k(x)` for a caller-supplied design matrix.
 *
 * The general form, exposed because not every model is a polynomial in `x`: a linear fit against `sin(x)` or
 * `1/x` is still a **linear** least-squares problem in its coefficients, and refusing it because the basis is
 * not a power of `x` would push callers to reimplement the linear algebra. `fit_polynomial` is this function
 * with `basis_k(x) = x^k` and is defined in terms of it, so there is one solver and not two.
 *
 * @param design_rows One row per point, `parameters` entries each. Row `i` must be in the same order as
 *                    `points[i]`, because the residuals are reported per input point.
 * @param points      The measurements. `points.size()` must equal `design_rows.size()`.
 * @param model       The name to record in the result, e.g. "linear". Copied.
 * @param scale_covariance See `fit_polynomial`.
 *
 * @ownership   owns the returned result
 * @thread      main
 * @pre         `design_rows[i].size()` equals `design_rows[j].size()` for every `i`, `j`
 * @post        On success the result has one coefficient per column and one residual per point
 * @invariant   A row of zeros contributes nothing to the fit and is still reported a residual of `-y`
 * @errors      `invalid_argument` for an empty design, a ragged matrix, a row/point count mismatch, a
 *              non-positive `sigma` or a non-finite value; `fit_failed` when the columns are linearly dependent
 *              or the system is underdetermined
 * @complexity  O(n * m^2 + m^3) for `n` points and `m` parameters
 * @nondet      none
 * @frozen      no
 * @tests       fit.analysis.a_fit_against_a_chosen_basis
 */
[[nodiscard]] diag::Result<runtime::FitResult> fit_linear_model(    const std::vector<std::vector<double>>& design_rows, const std::vector<Point>& points,
    std::string model, bool scale_covariance = false);

/**
 * @brief The reduced chi-squared of a fit: `chi_squared / dof`, or nothing when `dof` is zero.
 *
 * The number a student compares against 1. Well below 1 means the stated uncertainties are larger than the
 * scatter -- the error bars are overstated, which is a real finding about the instrument and not a failure of
 * the fit. Well above 1 means the opposite: either the model is wrong or the uncertainties are optimistic, and
 * telling those two apart is what the residuals are for.
 *
 * Nothing rather than zero when there are no degrees of freedom, for the reason this whole platform exists:
 * a reduced chi-squared of zero reads as a perfect fit, and a fit with as many parameters as points has said
 * nothing at all.
 *
 * @param fit The result to summarise.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        Nothing exactly when `degrees_of_freedom <= 0`
 * @invariant   Non-negative when it has a value, for a fit whose chi-squared is non-negative
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       fit.analysis.chi_squared_measures_the_scatter
 */
[[nodiscard]] std::optional<double> reduced_chi_squared(const runtime::FitResult& fit) noexcept;

}  // namespace qp::plugins::analysis
