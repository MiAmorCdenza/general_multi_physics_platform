/**
 * @file propagate.hpp
 * @brief Uncertainty propagation: turning "ten periods took 19.98 s" into a number a report can defend.
 *
 * ## The lesson this file exists for
 *
 * The standard trick of a first-year mechanics lab is to time **ten** periods and divide by ten, and the reason
 * it works is not that the stopwatch gets better. It is that the uncertainty divides with the reading: a
 * stopwatch reading to 10 ms carries `10 ms / sqrt(12)` of standard uncertainty however many periods it times,
 * and the mean of ten periods carries a tenth of that. The reading gets coarser -- 0.2 s of resolution is a
 * hundredth of a period, not a thousandth -- and the **uncertainty** gets ten times smaller. A student who
 * writes down "T = 1.998 s +/- 0.003 s" after timing ten periods has done exactly the right thing; the one who
 * writes "T = 1.9980 s +/- 0.0003 s" has divided the uncertainty twice.
 *
 * So the arithmetic here is the one that makes that visible, stated in terms of **independent** measurements and
 * the coefficients a caller puts on them. It is deliberately the smallest possible toolkit:
 *
 *   - a sum of independent values with coefficients, which covers the mean (`1/n` each), the difference of two
 *     readings (a before and an after), a sum of masses, and a scaled reading;
 *   - a **ratio** and a **product** of two independent values, by the first-order expansion every lab manual
 *     teaches, with the correlations that a general `f(x, y)` does not have;
 *   - a generic first-order propagation for one-variable functions with a caller-supplied derivative, so a
 *     student who knows d(sin x)/dx does not have to invent a second mechanism;
 *   - and a **correlated** sum, because a general propagation that assumed independence would be silently wrong
 *     the first time two derived quantities shared a fitted coefficient -- which is exactly what
 *     `FitResult::covariance` exists to describe, and the reason it is a full matrix rather than a diagonal.
 *
 * ## Why first order, and when that is not enough
 *
 * Every function here is the linear approximation: `sigma_f^2 = sum (df/dx_i)^2 sigma_i^2`. It is what a lab
 * manual means by "propagation of errors", it is exact for linear `f`, and it is the convention against which
 * the student's own hand calculation is marked -- a platform that used a Monte Carlo method would produce a
 * number that does not match the one they are taught to produce, and the difference would look like a bug.
 *
 * Its limit is real and stated rather than hidden: for a strongly nonlinear function, or a relative uncertainty
 * large enough that the expansion is not small, the answer is approximate. `propagate.analysis.a_large_relative_uncertainty_is_still_first_order`
 * measures the error rather than pretending it is absent, which is where the reopening condition lives: a
 * demand for a genuine nonlinear treatment (a Monte Carlo with the run's seed, which this platform already has)
 * is a new function under a new name, not a change to these.
 *
 * ## Sign and dimension
 *
 * Nothing here carries a dimension. `units::Dim` is available and a derived quantity has one, but attaching it
 * to every intermediate would mean this module owning a unit algebra, and the platform already has one -- the
 * node graph. The division of labour is deliberate: this file answers "how well is this known", and the caller
 * answers "what is it".
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every returned uncertainty is non-negative and finite
 * @errors      See each declaration
 * @frozen      no
 * @tests       propagate.analysis.a_mean_divides_the_uncertainty_by_root_n
 */
#pragma once

#include <qp/diag/result.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace qp::plugins::analysis {

/**
 * @brief One independent input to a derived quantity: a value, how well it is known, and its coefficient.
 *
 * @ownership   owns
 * @thread      main
 * @pre         `sigma >= 0`
 * @post        none
 * @invariant   `sigma == 0` means "exact by construction", which is a claim and not an absence
 * @errors      noexcept
 * @frozen      no
 * @tests       propagate.analysis.a_mean_divides_the_uncertainty_by_root_n
 */
struct Term final {
    /// The measured value.
    double value = 0.0;
    /// Its standard uncertainty. Zero is the exact case -- a counted quantity, or a defined constant.
    double sigma = 0.0;
    /// How many of it, or what fraction. The coefficient in the linear combination.
    double coefficient = 1.0;
};

/// @brief A value and its standard uncertainty: what a propagation produces.
///
/// @ownership   owns
/// @thread      main
/// @pre         none
/// @post        none
/// @invariant   `sigma >= 0`
/// @errors      noexcept
/// @frozen      no
/// @tests       propagate.analysis.a_mean_divides_the_uncertainty_by_root_n
struct Uncertain final {
    double value = 0.0;
    double sigma = 0.0;
};

/**
 * @brief The standard error of the **mean** of `n` independent readings, each with uncertainty `sigma`.
 *
 * `sigma / sqrt(n)`, and it is a named function rather than a line of arithmetic in a caller because of the
 * mistake it prevents: averaging repeated readings is the one place where a student is most likely to divide
 * the uncertainty by `n` (the same way the value is divided) rather than by `sqrt(n)`. The two answers differ
 * by `sqrt(n)` -- a factor of twenty for a four-hundred-reading series -- and only one of them is defensible.
 *
 * Nothing for an empty series or a negative uncertainty, because "the mean of nothing" is not a quantity and a
 * caller that receives `0` would print it.
 *
 * @param sigma The uncertainty of one reading. Zero is legitimate: the mean of exact values is exact.
 * @param n     How many readings.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        Nothing when `n == 0` or `sigma < 0` or either is not finite
 * @invariant   `<= sigma` for any `n >= 1`
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       propagate.analysis.a_mean_divides_the_uncertainty_by_root_n
 */
[[nodiscard]] std::optional<double> standard_error_of_mean(double sigma, std::size_t n) noexcept;

/**
 * @brief The uncertainty of a linear combination of **independent** inputs.
 *
 * `sigma_f = sqrt(sum (c_i * sigma_i)^2)`. Contributions add in **quadrature**, not linearly, and that is the
 * point a lab course spends a week on: two independent errors of 1 mm do not make 2 mm, they make 1.41 mm,
 * because they are as likely to cancel as to add.
 *
 * The value is the same sum, so a caller gets both halves of the answer and cannot compute one without the
 * other -- which is how a report ends up quoting a mean whose uncertainty was computed for something else.
 *
 * @param terms The inputs, each with its own coefficient.
 *
 * @ownership   owns the result
 * @thread      main
 * @pre         none
 * @post        `sigma` is non-negative and finite for finite inputs
 * @invariant   Scaling every coefficient by `k` scales both the value and the uncertainty by `k`
 * @errors      `invalid_argument` for a non-finite value, coefficient or uncertainty, and for a negative
 *              uncertainty (which is a typo, not a measurement)
 * @complexity  O(terms)
 * @nondet      none
 * @frozen      no
 * @tests       propagate.analysis.independent_errors_add_in_quadrature,
 *              propagate.analysis.a_negative_uncertainty_is_refused
 */
[[nodiscard]] diag::Result<Uncertain> combine(const std::vector<Term>& terms);

/**
 * @brief The uncertainty of a linear combination whose inputs are **correlated**.
 *
 * The general form: `sigma_f^2 = sum_ij c_i c_j cov_ij`. The independent version above is the special case
 * where the covariance matrix is diagonal, and it is kept as its own function because a caller who has no
 * covariance should not have to write one out.
 *
 * This is the function that makes `FitResult`'s full covariance matrix usable. Two quantities derived from the
 * same fitted line -- an intercept and a gradient, or the same gradient evaluated at two temperatures -- share
 * the coefficients' errors, and treating them as independent **understates** the uncertainty of their difference
 * by an amount that grows with the correlation. Every lab course catches this for a straight-line fit through
 * the origin, where the intercept and the gradient are anticorrelated and the extrapolated value is much better
 * known than either.
 *
 * @param values      The inputs' values, in the order the covariance uses.
 * @param coefficients What each contributes.
 * @param covariance  Row-major, square, side `values.size()`. An empty matrix means "independent", which is
 *                    handled by treating every off-diagonal entry as zero rather than by refusing.
 *
 * @ownership   owns the result
 * @thread      main
 * @pre         none
 * @post        `sigma` is non-negative for a positive semi-definite covariance
 * @invariant   Agrees with `combine` whenever the covariance is diagonal
 * @errors      `invalid_argument` for a size mismatch, a ragged covariance, a non-finite entry, or a negative
 *              diagonal; a negative variance that survives the sum is `invalid_argument` too, because a
 *              negative variance is a claim about a covariance matrix that is not one
 * @complexity  O(n^2)
 * @nondet      none
 * @frozen      no
 * @tests       propagate.analysis.correlated_inputs_do_not_add_in_quadrature
 */
[[nodiscard]] diag::Result<Uncertain> combine_correlated(const std::vector<double>& values,
                                                         const std::vector<double>& coefficients,
                                                         const std::vector<double>& covariance);

/**
 * @brief The uncertainty of `f(x)` for one measured `x`, given `df/dx`.
 *
 * `sigma_f = |f'(x)| * sigma_x`. First order, and exact for a linear `f`.
 *
 * The derivative is a **caller-supplied function** rather than an approximation this file computes by finite
 * differences, and the reason is a lesson rather than a shortcut: a student who has worked out d(sin x)/dx =
 * cos x has done the physics, and a platform that differentiated numerically would accept a wrong derivative
 * silently and disagree with their hand calculation in the fourth digit. Supplying it means the derivative is
 * something they wrote down, which is what a lab report asks for.
 *
 * A **zero** derivative gives zero uncertainty, and that is correct rather than a degenerate case: a quantity at
 * a stationary point of `f` is known much better than the input, which is precisely the anticorrelation effect
 * the correlated form above describes. It is also why this function does not add a "second-order" term: doing so
 * would be right and would disagree with every lab manual.
 *
 * @param x         The measured input.
 * @param sigma     Its standard uncertainty.
 * @param f         The function, for the value. Called once.
 * @param derivative `df/dx` at `x`. Called once.
 *
 * @ownership   owns the result
 * @thread      main
 * @pre         `derivative` is the derivative of `f`
 * @post        `sigma` of the result is `|f'(x)| * sigma`
 * @invariant   A zero derivative yields zero uncertainty whatever the input's is
 * @errors      `invalid_argument` for a negative or non-finite input uncertainty, or a non-finite value or
 *              derivative; an empty `f` or `derivative` is `invalid_argument` as well, rather than a crash
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       propagate.analysis.a_derivative_is_the_caller_s,
 *              propagate.analysis.a_missing_derivative_is_refused
 */
[[nodiscard]] diag::Result<Uncertain> propagate_function(double x, double sigma,
                                                         const std::function<double(double)>& f,
                                                         const std::function<double(double)>& derivative);

/**
 * @brief The uncertainty of `a / b` for two independent measurements.
 *
 * `|a/b| * sqrt((sigma_a/a)^2 + (sigma_b/b)^2)`: **relative** uncertainties add in quadrature, which is why a
 * ratio of two well-known quantities is usually fine and a ratio involving a small one is not.
 *
 * A dedicated function rather than a call to `propagate_function` twice, for two reasons: the derivative of a
 * quotient is the step a student most often gets wrong (it is `1/b` and `-a/b^2`, and the second one is what
 * makes the answer a relative sum), and the refusal a division needs is specific -- a **zero divisor** is
 * `invalid_argument` here, where a generic propagation would divide by it and return an infinity that then
 * propagates through everything downstream as a NaN.
 *
 * @param a      The numerator's value.
 * @param sigma_a Its standard uncertainty.
 * @param b      The denominator's value.
 * @param sigma_b Its standard uncertainty.
 *
 * @ownership   owns the result
 * @thread      main
 * @pre         none
 * @post        On success the uncertainty is the relative sum in quadrature, scaled by the ratio
 * @invariant   Symmetric under swapping numerator and denominator together with their uncertainties
 * @errors      `invalid_argument` for a zero denominator, a negative or non-finite uncertainty, or a non-finite
 *              value
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       propagate.analysis.a_ratio_is_not_a_difference
 */
[[nodiscard]] diag::Result<Uncertain> ratio(double a, double sigma_a, double b, double sigma_b);

}  // namespace qp::plugins::analysis
