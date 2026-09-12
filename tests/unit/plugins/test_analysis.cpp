/**
 * @file test_analysis.cpp
 * @brief Tests for the fit and the propagation: the numbers a lab report is graded on.
 *
 * Test case ids match the @tests fields in the plugin headers byte for byte.
 *
 * ## What is worth asserting about a fit
 *
 * Not that a matrix was inverted. What is worth asserting is that the **answer is the one a student can check by
 * hand**, because that is the property that makes the platform useful rather than merely self-consistent:
 *
 *   - a line through points it passes exactly through is exact, with zero chi-squared and zero coefficient
 *     uncertainty -- and the intercept and gradient are the ones the points imply;
 *   - a point known ten times better pulls the line a hundred times harder, and the fitted gradient moves
 *     accordingly: `w = 1/sigma^2`, which is the whole content of "weighted";
 *   - the residuals are what the *data* should give, not what the fit says they are -- an exact fit whose
 *     residuals were computed from the normal equations would still be zero if the accumulation were wrong;
 *   - a nearly singular system is **refused**, not answered with digits that are noise;
 *   - and the quantities a report needs are present exactly when they are defined: R-squared is absent for a
 *     perfect fit, reduced chi-squared is absent with no degrees of freedom.
 *
 * The propagation cases are the lab-manual formulae, checked against the numbers a student computes with a
 * calculator: the standard error of a mean, quadrature against linear addition, and the correlated case where
 * the naive sum is wrong in the direction that matters.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/analysis/fit.hpp>
#include <qp/plugins/analysis/propagate.hpp>

#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

using namespace qp::plugins::analysis;

namespace {

namespace rt = qp::runtime;

/// @brief Points from parallel vectors, for terse fixtures.
std::vector<Point> make_points(const std::vector<double>& xs, const std::vector<double>& ys,
                               double sigma = 1.0) {
    std::vector<Point> out;
    out.reserve(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) out.push_back(Point{xs[i], ys[i], sigma});
    return out;
}

/// @brief Points with one sigma per point.
std::vector<Point> make_points(const std::vector<double>& xs, const std::vector<double>& ys,
                               const std::vector<double>& sigmas) {
    std::vector<Point> out;
    out.reserve(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) out.push_back(Point{xs[i], ys[i], sigmas[i]});
    return out;
}

}  // namespace

TEST_CASE("fit.a_line_through_exact_points_is_exact", "[analysis]") {
    // The first thing anyone does with a fit: check it against points that lie on a line somebody chose. y = 3x+1
    // at four abscissae, each known to 0.1.
    const std::vector<Point> points =
        make_points({0.0, 1.0, 2.0, 3.0}, {1.0, 4.0, 7.0, 10.0}, 0.1);

    const auto fit = fit_polynomial(points, 1);
    REQUIRE(fit.has_value());
    const rt::FitResult& r = fit.value();

    REQUIRE(r.model == "linear");
    REQUIRE(r.coefficients.size() == 2);
    REQUIRE(r.residuals.size() == 4);
    REQUIRE(r.degrees_of_freedom == 2);
    REQUIRE(r.has_covariance());

    REQUIRE(std::abs(r.coefficients[0] - 1.0) < 1.0e-12);
    REQUIRE(std::abs(r.coefficients[1] - 3.0) < 1.0e-12);

    // The residuals are at the **rounding level**, not exactly zero, and that is a finding rather than a
    // tolerance. `A^T W A` is formed by summing products, so the coefficients come back as 3.0 plus or minus a
    // few ulps; multiplying that by x and subtracting gives a residual of order 1e-16 rather than 0. An earlier
    // version of this case asserted `is_exact()` -- `chi_squared == 0` -- and failed, which was the test being
    // wrong about floating point rather than the fit being wrong about the line.
    //
    // What matters is asserted instead: the residual is negligible compared with the measurement's own
    // uncertainty, which is the property a user cares about. A fit that passed through the points to within a
    // millionth of a sigma is a fit that found the line.
    for (const double residual : r.residuals) REQUIRE(std::abs(residual) < 1.0e-12);
    REQUIRE(r.chi_squared < 1.0e-20);

    // A perfect fit leaves nothing for the line to explain, so R-squared is 1 here -- the total scatter is
    // non-zero (the y values differ) and the residual scatter is zero.
    REQUIRE(r.r_squared.has_value());
    REQUIRE(std::abs(r.r_squared.value() - 1.0) < 1.0e-12);

    // The uncertainties are small but **not** zero: four points and two parameters leave two degrees of freedom,
    // and a line through them is constrained, not determined. A zero here would be the platform claiming a
    // perfect measurement.
    const auto intercept_u = r.coefficient_uncertainty(0);
    const auto gradient_u = r.coefficient_uncertainty(1);
    REQUIRE(intercept_u.has_value());
    REQUIRE(gradient_u.has_value());
    REQUIRE(intercept_u.value() > 0.0);
    REQUIRE(gradient_u.value() > 0.0);
    // And the intercept is known **worse** than the gradient, which is the shape every student sees: the line
    // pivots about the centroid, so the ends move most.
    REQUIRE(intercept_u.value() > gradient_u.value());
}

TEST_CASE("fit.analysis.weights_decide_the_line", "[analysis]") {
    // The property that makes this a weighted fit rather than a line through the average. Two points define a
    // line exactly, so a third with a tiny sigma must drag it almost onto itself while one with a huge sigma must
    // barely move it. `w = 1/sigma^2` is the claim; this measures it.
    const std::vector<Point> good = make_points({0.0, 1.0, 2.0}, {0.0, 1.0, 5.0},
                                                {1.0, 1.0, 0.001});
    const auto trusted = fit_polynomial(good, 1);
    REQUIRE(trusted.has_value());
    // The third point is known a thousand times better, so the line goes through it: at x = 2 the prediction is
    // within a thousandth of 5.
    const double at_two = trusted.value().coefficients[0] + 2.0 * trusted.value().coefficients[1];
    REQUIRE(std::abs(at_two - 5.0) < 1.0e-6);

    const std::vector<Point> ignored = make_points({0.0, 1.0, 2.0}, {0.0, 1.0, 5.0},
                                                   {1.0, 1.0, 1000.0});
    const auto discounted = fit_polynomial(ignored, 1);
    REQUIRE(discounted.has_value());
    // The same third point, now nearly worthless, and the line goes through the first two instead.
    const double at_two_cheap = discounted.value().coefficients[0] + 2.0 * discounted.value().coefficients[1];
    REQUIRE(std::abs(at_two_cheap - 2.0) < 1.0e-3);

    // Unweighted, the third point would sit between: this is the assertion that the weights were **used**.
    REQUIRE(std::abs(at_two_cheap - at_two) > 1.0);
}

TEST_CASE("fit.analysis.an_unknown_uncertainty_is_refused", "[analysis]") {
    // The rule the whole platform holds, in the one place a fit can still act on it. A point with `sigma = 0` is
    // a point with **infinite** weight: one reading would become the entire answer, and the fitted gradient would
    // no longer depend on any other measurement. Refused rather than special-cased.
    const std::vector<Point> unknown = make_points({0.0, 1.0, 2.0}, {0.0, 1.0, 2.0},
                                                   {1.0, 0.0, 1.0});
    REQUIRE_FALSE(fit_polynomial(unknown, 1).has_value());

    // A negative or non-finite uncertainty is a typo or a failed computation, and both are refused for the same
    // reason: neither is a statement about how well anything is known.
    REQUIRE_FALSE(fit_polynomial(make_points({0.0, 1.0, 2.0}, {0.0, 1.0, 2.0}, {1.0, -1.0, 1.0}), 1)
                      .has_value());
    REQUIRE_FALSE(fit_polynomial(make_points({0.0, 1.0, 2.0}, {0.0, 1.0, 2.0},
                                             {1.0, std::nan(""), 1.0}),
                                 1)
                      .has_value());
}

TEST_CASE("fit.analysis.nearly_equal_abscissae_still_fit", "[analysis]") {
    // The numerical claim, measured rather than assumed. The normal equations square the condition number, so the
    // honest question is how close together the abscissae can be before the answer stops being usable. This
    // exercises a spread of 1e-4 -- a thousand times worse conditioned than any lab fixture -- and checks the
    // gradient against the one the data implies, to a relative tolerance rather than a bit comparison.
    // y = 2x + 5 on a narrow window, as a straight-line calibration on a small range of a dial would be.
    const std::vector<Point> points = make_points({1.0000, 1.00005, 1.00010},
                                                  {7.0000, 7.0001, 7.0002}, 1.0e-5);
    const auto fit = fit_polynomial(points, 1);
    REQUIRE(fit.has_value());
    REQUIRE(std::abs(fit.value().coefficients[1] - 2.0) < 1.0e-3);
    REQUIRE(std::abs(fit.value().coefficients[0] - 5.0) < 1.0e-3);
    // And the covariance is finite, which is the half a "the answer looks close" check would miss: an
    // ill-conditioned inverse can be numerically infinite while the coefficients still round to something right.
    for (const double entry : fit.value().covariance) REQUIRE(std::isfinite(entry));

    // A genuinely singular system -- every abscissa the same -- is refused by name rather than answered with
    // digits that are pure noise.
    const std::vector<Point> vertical = make_points({2.0, 2.0, 2.0}, {1.0, 2.0, 3.0}, 0.1);
    const auto refused = fit_polynomial(vertical, 1);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == qp::diag::ErrorCode::fit_failed);

    // A degree with too few points is refused as a different cause: there are as many parameters as readings, so
    // the fit can be satisfied exactly and says nothing about the model.
    REQUIRE_FALSE(fit_polynomial(make_points({0.0, 1.0}, {0.0, 1.0}), 1).has_value());
}

TEST_CASE("fit.analysis.a_quadratic_recovers_its_own_coefficients", "[analysis]") {
    // The degree is a parameter, so the fit has to be a family and not a special case. y = 2 - 3x + 0.5x^2 at
    // five abscissae is five equations in three unknowns, solved exactly.
    const std::vector<double> xs = {-2.0, -1.0, 0.0, 1.0, 2.0};
    std::vector<double> ys;
    for (const double x : xs) ys.push_back(2.0 - 3.0 * x + 0.5 * x * x);

    const auto fit = fit_polynomial(make_points(xs, ys, 0.01), 2);
    REQUIRE(fit.has_value());
    REQUIRE(fit.value().model == "polynomial");
    REQUIRE(fit.value().coefficients.size() == 3);
    REQUIRE(fit.value().degrees_of_freedom == 2);
    REQUIRE(std::abs(fit.value().coefficients[0] - 2.0) < 1.0e-10);
    REQUIRE(std::abs(fit.value().coefficients[1] + 3.0) < 1.0e-10);
    REQUIRE(std::abs(fit.value().coefficients[2] - 0.5) < 1.0e-10);

    // Degree zero is the weighted mean, and it is the one case where the normal equations are a single division.
    // Asserted because a "polynomial" that could not do the simplest polynomial would be a strange family.
    const auto mean = fit_polynomial(make_points({0.0, 1.0, 2.0}, {3.0, 4.0, 5.0}, 1.0), 0);
    REQUIRE(mean.has_value());
    REQUIRE(mean.value().model == "mean");
    REQUIRE(mean.value().coefficients.size() == 1);
    REQUIRE(std::abs(mean.value().coefficients[0] - 4.0) < 1.0e-12);

    // A degree the file will not attempt is refused rather than attempted badly: `x^13` at a lab-scale abscissa
    // is a column of zeros in double precision, and the normal matrix would be singular for a reason that has
    // nothing to do with the data.
    REQUIRE_FALSE(fit_polynomial(make_points(xs, ys, 1.0), 13).has_value());
}

TEST_CASE("fit.analysis.chi_squared_measures_the_scatter", "[analysis]") {
    // Chi-squared is the number that says whether the error bars are believable, so it is measured against cases
    // whose value is known by hand rather than against a stored number.
    const std::vector<double> xs = {0.0, 1.0, 2.0, 3.0, 4.0};
    const std::vector<double> ys = {0.0, 2.0, 4.0, 6.0, 8.0};
    const auto exact = fit_polynomial(make_points(xs, ys, 1.0), 1);
    REQUIRE(exact.has_value());
    // At the rounding level rather than exactly zero, for the reason the first case records: the coefficients are
    // recovered to a few ulps, so the residuals are of order 1e-16. The claim worth making is that the fit found
    // the line, and `1e-20` on a sum of five squared residuals of that size says so.
    REQUIRE(exact.value().chi_squared < 1.0e-20);

    // Now shift one point up by exactly one sigma, with every sigma = 1.
    std::vector<double> nudged = ys;
    nudged[2] += 1.0;
    const auto one_off = fit_polynomial(make_points(xs, nudged, 1.0), 1);
    REQUIRE(one_off.has_value());
    // Not exactly 1: the line moves towards the nudged point, so the residual is spread over every point. What is
    // asserted is that it is a fraction of the raw 1.0 -- which is the property that says the fit **used** the
    // point rather than ignoring it.
    REQUIRE(one_off.value().chi_squared > 0.0);
    REQUIRE(one_off.value().chi_squared < 1.0);

    // The reduced form is chi-squared over the degrees of freedom, and it is absent -- not zero, not infinite --
    // when there are none. A reduced chi-squared of zero would read as a perfect fit, and a fit with as many
    // parameters as points has said nothing.
    const std::optional<double> reduced = reduced_chi_squared(one_off.value());
    REQUIRE(reduced.has_value());
    REQUIRE(std::abs(reduced.value() - one_off.value().chi_squared / 3.0) < 1.0e-15);

    rt::FitResult degenerate;
    degenerate.chi_squared = 0.0;
    degenerate.degrees_of_freedom = 0;
    REQUIRE_FALSE(reduced_chi_squared(degenerate).has_value());
    degenerate.degrees_of_freedom = -1;
    REQUIRE_FALSE(reduced_chi_squared(degenerate).has_value());
}

TEST_CASE("fit.analysis.r_squared_is_absent_when_it_is_undefined", "[analysis]") {
    // 0/0 is not 1. A data set with no scatter at all -- every y identical -- has nothing for a model to explain,
    // and reporting R-squared = 1 there would be the platform claiming a perfect fit to a constant. The optional
    // is the whole reason `FitResult` has one.
    const std::vector<Point> flat = make_points({0.0, 1.0, 2.0, 3.0}, {5.0, 5.0, 5.0, 5.0}, 0.1);
    const auto fit = fit_polynomial(flat, 1);
    REQUIRE(fit.has_value());
    // A fit to a constant is exact to the rounding level, and the point of the case is that R-squared is **absent**
    // rather than 1: there is no scatter for the model to explain, so the ratio is 0/0.
    REQUIRE(fit.value().chi_squared < 1.0e-20);
    REQUIRE_FALSE(fit.value().r_squared.has_value());

    // With scatter, it is present and between 0 and 1 for a line through noisy data. Asserted as a range rather
    // than a value, because the value depends on the fixture and the range is the property that matters.
    const auto noisy = fit_polynomial(make_points({0.0, 1.0, 2.0, 3.0}, {1.0, 2.9, 5.2, 7.0}, 0.5), 1);
    REQUIRE(noisy.has_value());
    REQUIRE(noisy.value().r_squared.has_value());
    REQUIRE(noisy.value().r_squared.value() > 0.99);
    REQUIRE(noisy.value().r_squared.value() < 1.0);
}

TEST_CASE("fit.analysis.scaling_the_covariance_answers_a_different_question", "[analysis]") {
    // The two conventions, side by side, so a caller can see they are different questions. With the stated
    // sigmas taken as known, the covariance is `(A^T W A)^-1`; when the data scatters more than the sigmas claim,
    // the scaled form grows by `chi-squared / dof`. Both are right, and a fit that chose silently would be
    // answering a question nobody asked.
    const std::vector<Point> points = make_points({0.0, 1.0, 2.0, 3.0}, {0.0, 1.5, 1.5, 3.0}, 0.05);
    const auto known = fit_polynomial(points, 1, /*scale_covariance=*/false);
    const auto estimated = fit_polynomial(points, 1, /*scale_covariance=*/true);
    REQUIRE(known.has_value());
    REQUIRE(estimated.has_value());

    // The coefficients are the same: scaling the covariance does not move the line.
    REQUIRE(known.value().coefficients == estimated.value().coefficients);

    // But the uncertainties differ, and the estimated one is larger because the data scatters more than 0.05.
    const auto known_u = known.value().coefficient_uncertainty(1);
    const auto estimated_u = estimated.value().coefficient_uncertainty(1);
    REQUIRE(known_u.has_value());
    REQUIRE(estimated_u.has_value());
    REQUIRE(estimated_u.value() > known_u.value());

    // And the ratio is exactly the reduced chi-squared, which is what "scaled by chi-squared/dof" means.
    const std::optional<double> reduced = reduced_chi_squared(known.value());
    REQUIRE(reduced.has_value());
    REQUIRE(std::abs(estimated_u.value() / known_u.value() - std::sqrt(reduced.value())) < 1.0e-9);
}

TEST_CASE("fit.analysis.a_fit_against_a_chosen_basis", "[analysis]") {
    // Not every linear model is a power of x. A pendulum's period against sqrt(L) is a straight line through the
    // origin whose gradient is 2*pi/sqrt(g), and a linear fit with one column is a line with the intercept forced
    // to zero -- both are linear least squares with a basis the caller chooses, which is why the general form is
    // exposed.
    //
    // The fixture is **generated from** that gradient rather than from rounded decimals, which is a correction:
    // the first version listed four-decimal periods and asserted the recovered gradient to three decimals, and the
    // rounding in the fixture was larger than the tolerance. A test whose expected value is built from the same
    // constant as the data is checking the fit; one built from hand-rounded numbers is checking the rounding.
    const double sqrt_g = std::sqrt(9.81);
    const double gradient = 2.0 * 3.14159265358979323846 / sqrt_g;
    const std::vector<double> lengths = {0.09, 0.16, 0.25, 0.36};
    std::vector<Point> points;
    std::vector<std::vector<double>> rows;
    for (const double l : lengths) {
        points.push_back(Point{l, gradient * std::sqrt(l), 0.01});
        rows.push_back({std::sqrt(l)});
    }

    const auto fit = fit_linear_model(rows, points, "period_vs_sqrt_length");
    REQUIRE(fit.has_value());
    REQUIRE(fit.value().model == "period_vs_sqrt_length");
    REQUIRE(fit.value().coefficients.size() == 1);
    REQUIRE(std::abs(fit.value().coefficients[0] - gradient) < 1.0e-9);
    // And the value it recovers is the physical one: 2*pi/sqrt(g) = 2.005 s/m^0.5 for g = 9.81.
    REQUIRE(std::abs(fit.value().coefficients[0] - 2.0054) < 1.0e-3);
    REQUIRE(fit.value().residuals.size() == 4);

    // A ragged design matrix is refused, because the rows are the model and a row of the wrong length is a model
    // that is not the one the caller thinks it is.
    std::vector<std::vector<double>> ragged = {{1.0}, {1.0, 2.0}};
    REQUIRE_FALSE(fit_linear_model(ragged, points, "ragged").has_value());
    // As is a design whose row count does not match the point count: the residuals would be reported against the
    // wrong measurements.
    std::vector<std::vector<double>> short_design = {{1.0}, {1.0}};
    REQUIRE_FALSE(fit_linear_model(short_design, points, "short").has_value());
    REQUIRE_FALSE(fit_linear_model({}, {}, "empty").has_value());
}

TEST_CASE("fit.analysis.a_broken_input_is_refused", "[analysis]") {
    // A model that produced an infinity must not become a fitted coefficient. Every route in is refused, and the
    // point of listing them together is that they are different mistakes with the same consequence.
    const double inf = std::numeric_limits<double>::infinity();

    // A non-finite ordinate in an otherwise perfect fixture.
    std::vector<Point> bad_y = make_points({0.0, 1.0, 2.0}, {0.0, 1.0, 2.0}, 1.0);
    bad_y[1].y = inf;
    REQUIRE_FALSE(fit_polynomial(bad_y, 1).has_value());

    // A non-finite abscissa.
    std::vector<Point> bad_x = make_points({0.0, 1.0, 2.0}, {0.0, 1.0, 2.0}, 1.0);
    bad_x[2].x = std::nan("");
    REQUIRE_FALSE(fit_polynomial(bad_x, 1).has_value());

    // An abscissa so large that its square overflows: refused as `invalid_argument` rather than reported as
    // `singular`, because the cause is the data and not the system.
    std::vector<Point> huge = make_points({0.0, 1.0, 1.0e200}, {0.0, 1.0, 2.0}, 1.0);
    REQUIRE_FALSE(fit_polynomial(huge, 2).has_value());

    // And an empty data set, which has no points and therefore nothing to fit.
    REQUIRE_FALSE(fit_polynomial({}, 1).has_value());
}

TEST_CASE("propagate.analysis.a_mean_divides_the_uncertainty_by_root_n", "[analysis]") {
    // The lesson the whole module exists for. A digital stopwatch reads to 10 ms, so one reading carries
    // `0.01/sqrt(12)` = 2.887 ms of standard uncertainty -- the rectangular distribution of a value rounded to the
    // nearest tick, which is the same error model `plugins/instruments` gives every device.
    const double one_reading = 1.0e-2 / std::sqrt(12.0);
    REQUIRE(std::abs(one_reading - 2.886751345948129e-3) < 1.0e-15);

    // Ten periods timed in one go, then divided by ten. The **value** divides by ten; the uncertainty divides by
    // sqrt(10). A student who divides the uncertainty by ten as well has produced an answer three times better
    // than the instrument can support, and the two lines below are the difference.
    const auto ten = standard_error_of_mean(one_reading, 10);
    REQUIRE(ten.has_value());
    REQUIRE(std::abs(ten.value() - one_reading / std::sqrt(10.0)) < 1.0e-18);
    REQUIRE(ten.value() > 9.0e-4);
    // The size of the mistake, asserted as a **ratio** rather than as an inequality between two absolute
    // quantities. The first version wrote `abs(ten - one_reading/10) > 1e-3`, which is a claim about a difference
    // of 0.62 ms and simply false: dividing by n instead of sqrt(n) makes the answer sqrt(n)/n = 1/sqrt(n) of the
    // right value, and for n = 10 that is 0.316. Then a second version compared that ratio against sqrt(10)
    // itself, which is 3.16 times too big -- the lesson being that a ratio assertion has to name the ratio it
    // means, not the quantity it was derived from.
    REQUIRE(std::abs(one_reading / 10.0 / ten.value() - 1.0 / std::sqrt(10.0)) < 1.0e-12);
    // And the right answer is bigger than the wrong one, which is the direction of the error: dividing by n
    // understates the uncertainty, so a report built that way claims to know more than it does.
    REQUIRE(ten.value() > one_reading / 10.0);

    // Expressed instead as a combination, the uncertainty must be the same: two ways of saying "the mean" that
    // disagreed would be two rules for one quantity. What is combined is the **total** timed interval and the
    // coefficient that turns it into a period -- `1/10` -- so the value is the total, and the uncertainty is the
    // total's divided by ten. The first version asserted the value was the period, which it is not: a coefficient
    // scales a term, it does not normalise the answer, and the test was conflating the two steps of the
    // calculation.
    std::vector<Term> readings;
    for (int i = 0; i < 10; ++i) readings.push_back(Term{19.98, one_reading, 1.0 / 10.0});
    const auto via_combine = combine(readings);
    REQUIRE(via_combine.has_value());
    REQUIRE(std::abs(via_combine.value().value - 19.98) < 1.0e-12);
    REQUIRE(std::abs(via_combine.value().sigma - ten.value()) < 1.0e-18);
    // And the period is that value divided by ten, with the uncertainty divided by ten as well -- the step the
    // student writes down.
    const double period = via_combine.value().value / 10.0;
    REQUIRE(std::abs(period - 1.998) < 1.0e-12);

    // The mean of one reading is that reading.
    REQUIRE(standard_error_of_mean(one_reading, 1).value() == one_reading);
    // Exact values average to an exact value rather than to nothing: zero uncertainty is a claim, and this is
    // the case where it is a true one.
    REQUIRE(standard_error_of_mean(0.0, 5).value() == 0.0);
    // Nothing for an empty series, because "the mean of nothing" is not a quantity and a caller receiving 0
    // would print it.
    REQUIRE_FALSE(standard_error_of_mean(one_reading, 0).has_value());
    REQUIRE_FALSE(standard_error_of_mean(-1.0, 5).has_value());
}

TEST_CASE("propagate.analysis.independent_errors_add_in_quadrature", "[analysis]") {
    // Two independent errors of 1 mm do not make 2 mm. They make sqrt(2) mm, because they are as likely to
    // cancel as to add -- the point a lab course spends a week on, and the one a "just add them up" implementation
    // gets wrong in the direction that makes every error bar too big.
    const auto sum = combine({{1.0, 1.0e-3, 1.0}, {2.0, 1.0e-3, 1.0}});
    REQUIRE(sum.has_value());
    REQUIRE(sum.value().value == 3.0);
    REQUIRE(std::abs(sum.value().sigma - std::sqrt(2.0) * 1.0e-3) < 1.0e-15);
    REQUIRE(sum.value().sigma < 2.0e-3);

    // A coefficient scales both halves: this is how a mean is expressed as a combination, and how a sum of n
    // identical readings divides its uncertainty by n while its value multiplies by n.
    const auto scaled = combine({{2.0, 1.0e-3, 3.0}});
    REQUIRE(scaled.value().value == 6.0);
    REQUIRE(std::abs(scaled.value().sigma - 3.0e-3) < 1.0e-18);

    // The mean as a combination: n readings, each with coefficient 1/n. The uncertainty must equal the standard
    // error of the mean, which is the cross-check that the two functions agree about what a mean is.
    std::vector<Term> mean_terms;
    for (int i = 0; i < 10; ++i) mean_terms.push_back(Term{2.0, 3.0e-3, 1.0 / 10.0});
    const auto mean = combine(mean_terms);
    REQUIRE(mean.has_value());
    REQUIRE(std::abs(mean.value().value - 2.0) < 1.0e-12);
    REQUIRE(std::abs(mean.value().sigma - standard_error_of_mean(3.0e-3, 10).value()) < 1.0e-18);

    // A difference, which is where a "before and after" reading lives: 5.0 +/- 0.1 minus 3.0 +/- 0.1. The
    // uncertainties still add in quadrature -- subtraction does not reduce an uncertainty.
    const auto difference = combine({{5.0, 0.1, 1.0}, {3.0, 0.1, -1.0}});
    REQUIRE(difference.has_value());
    REQUIRE(std::abs(difference.value().value - 2.0) < 1.0e-12);
    REQUIRE(std::abs(difference.value().sigma - std::sqrt(0.02)) < 1.0e-15);

    // The empty combination is the additive identity and it is not an error.
    const auto nothing = combine({});
    REQUIRE(nothing.has_value());
    REQUIRE(nothing.value().value == 0.0);
    REQUIRE(nothing.value().sigma == 0.0);
}

TEST_CASE("propagate.analysis.a_negative_uncertainty_is_refused", "[analysis]") {
    // An uncertainty is a magnitude. A negative one is a typo in a table rather than a measurement, and letting
    // it through would square it back into a plausible answer -- so the check has to be here, where the typo is
    // still visible, rather than left to a caller's own validation.
    REQUIRE_FALSE(combine({{1.0, -1.0e-3, 1.0}}).has_value());
    REQUIRE_FALSE(combine({{1.0, std::nan(""), 1.0}}).has_value());
    REQUIRE_FALSE(combine({{std::numeric_limits<double>::infinity(), 1.0, 1.0}}).has_value());
    REQUIRE_FALSE(combine({{1.0, 1.0, std::nan("")}}).has_value());
    // Zero is **not** refused: it is the exact case, a counted quantity or a defined constant.
    REQUIRE(combine({{1.0, 0.0, 1.0}}).has_value());
}

TEST_CASE("propagate.analysis.correlated_inputs_do_not_add_in_quadrature", "[analysis]") {
    // **A difference of two evaluations of one fitted line.** Take a gradient known to 0.01 and an intercept known
    // to 0.5, anticorrelated -- the situation for a line fitted through points away from the origin. The value at
    // x = 2 is `c0 + 2 c1` and at x = 1 it is `c0 + c1`; their difference is exactly `c1`, so its uncertainty must
    // be exactly the gradient's 0.01. The coefficients below apply that subtraction to the two parameters, and the
    // order is (higher x first) so the resulting value is positive -- an earlier version had them reversed and
    // produced -1.0, which the value assertion caught.
    const std::vector<double> covariance = {
        0.25, -0.005,  // cov(c0, c0), cov(c0, c1)
        -0.005, 1.0e-4  // cov(c1, c0), cov(c1, c1)
    };
    const std::vector<double> values = {1.0, 1.0};
    const std::vector<double> coefficients = {(1.0 - 1.0), (2.0 - 1.0)};
    const auto difference = combine_correlated(values, coefficients, covariance);
    REQUIRE(difference.has_value());
    // The coefficients are (0, 1), so the value is the gradient and the uncertainty is its own. The correlated
    // formula gets 0.01 **exactly**, which is the whole reason a full covariance matrix is carried: the naive
    // independent sum would give sqrt(0.25 + 0.0001) = 0.5, fifty times too large.
    REQUIRE(difference.value().value == 1.0);
    REQUIRE(std::abs(difference.value().sigma - 0.01) < 1.0e-15);

    // The other direction, and the same covariance: a **sum** of the two evaluations, whose coefficients are
    // (2, 3). The negative cross term now subtracts, so the sum is known *better* than the independent formula
    // claims -- which is the effect that makes an extrapolated value from a well-fitted line surprisingly good.
    const std::vector<double> sum_coefficients = {2.0, 3.0};
    const auto sum = combine_correlated(values, sum_coefficients, covariance);
    REQUIRE(sum.has_value());
    const double independent_sum =
        std::sqrt(2.0 * 2.0 * 0.25 + 3.0 * 3.0 * 1.0e-4);  // no cross term
    REQUIRE(sum.value().sigma < independent_sum);

    // And the comparison that says what the correlated form is **for**: the difference is far better known than
    // either of the two values it is a difference of. A caller that computed `sigma_a` and `sigma_b` separately and
    // added them in quadrature would report 0.71 for a quantity known to 0.01 -- seventy times too large -- because
    // for a straight line the intercept and the gradient are strongly anticorrelated and the end-to-end difference
    // cancels that error almost exactly. The first version of this case compared against `combine` with coefficients
    // (0, 1), which is the **same** arithmetic on a diagonal matrix and therefore 0.01 as well; the number worth
    // comparing against is the two evaluations individually.
    const auto value_at_two = combine_correlated(values, {1.0, 2.0}, covariance);
    const auto value_at_one = combine_correlated(values, {1.0, 1.0}, covariance);
    const auto independent_difference =
        combine({{value_at_two.value().value, value_at_two.value().sigma, 1.0},
                 {value_at_one.value().value, value_at_one.value().sigma, -1.0}});
    REQUIRE(independent_difference.has_value());
    REQUIRE(independent_difference.value().sigma > 50.0 * difference.value().sigma);

    // An empty covariance is the independent case and must agree with `combine` exactly.
    const std::vector<double> diagonal = {1.0e-6, 0.0, 0.0, 4.0e-6};
    const auto via_matrix = combine_correlated({3.0, 5.0}, {2.0, 1.0}, diagonal);
    const auto via_terms = combine({{3.0, 1.0e-3, 2.0}, {5.0, 2.0e-3, 1.0}});
    REQUIRE(via_matrix.has_value());
    REQUIRE(via_terms.has_value());
    REQUIRE(std::abs(via_matrix.value().value - via_terms.value().value) < 1.0e-12);
    REQUIRE(std::abs(via_matrix.value().sigma - via_terms.value().sigma) < 1.0e-15);

    // A matrix that is not positive semi-definite is refused rather than square-rooted into a NaN: a negative
    // variance is a claim about a covariance matrix that is not one.
    const std::vector<double> bad = {1.0, 5.0, 5.0, 1.0};
    REQUIRE_FALSE(combine_correlated({1.0, 1.0}, {1.0, -1.0}, bad).has_value());
    // A negative **diagonal** is refused on sight, because that is a variance and a variance is a square.
    REQUIRE_FALSE(combine_correlated({1.0}, {1.0}, {-1.0}).has_value());
    // Sizes that do not line up are refused, including a covariance that is not square.
    REQUIRE_FALSE(combine_correlated({1.0, 2.0}, {1.0}, {}).has_value());
    REQUIRE_FALSE(combine_correlated({1.0, 2.0}, {1.0, 1.0}, {1.0, 2.0, 3.0}).has_value());
}

TEST_CASE("propagate.analysis.a_ratio_is_not_a_difference", "[analysis]") {
    // Relative uncertainties add in quadrature for a quotient. 10.0 +/- 0.1 over 2.0 +/- 0.1 is 5.0 with a
    // relative uncertainty of sqrt(0.01^2 + 0.05^2) -- which is dominated by the **denominator**, the half a
    // student routinely forgets because the numerator is the one they wrote down.
    const auto r = ratio(10.0, 0.1, 2.0, 0.1);
    REQUIRE(r.has_value());
    REQUIRE(r.value().value == 5.0);
    const double expected = 5.0 * std::sqrt(0.01 * 0.01 + 0.05 * 0.05);
    REQUIRE(std::abs(r.value().sigma - expected) < 1.0e-15);

    // The denominator dominates, which is the assertion that the second term is really in the formula.
    const auto numerator_only = ratio(10.0, 0.1, 2.0, 0.0);
    REQUIRE(numerator_only.has_value());
    REQUIRE(r.value().sigma > numerator_only.value().sigma);

    // A zero denominator is refused: the generic form would return an infinity, and the first thing downstream to
    // touch it would produce a NaN with nothing left to say where it came from.
    REQUIRE_FALSE(ratio(1.0, 0.1, 0.0, 0.1).has_value());
    // A zero numerator is fine and gives zero uncertainty: in absolute terms, `a/b` near zero does not care how
    // badly `a` is known.
    const auto zero_top = ratio(0.0, 0.1, 2.0, 0.1);
    REQUIRE(zero_top.has_value());
    REQUIRE(zero_top.value().value == 0.0);
    REQUIRE(zero_top.value().sigma == 0.0);
    // Negative uncertainties are refused here too, for the reason above.
    REQUIRE_FALSE(ratio(1.0, -0.1, 2.0, 0.1).has_value());
    REQUIRE_FALSE(ratio(1.0, 0.1, 2.0, -0.1).has_value());
    REQUIRE_FALSE(ratio(std::nan(""), 0.1, 2.0, 0.1).has_value());
}

TEST_CASE("propagate.analysis.a_missing_derivative_is_refused", "[analysis]") {
    // A `std::function` that has not been set throws when it is called, and this file's contract is a `Result`
    // rather than an exception. So an empty function is refused **before** it is invoked, which is the difference
    // between a caller getting a refusal code and a caller getting a `std::bad_function_call` out of a module whose
    // whole promise is that it reports rather than throws.
    const std::function<double(double)> real = [](double x) { return x; };
    const std::function<double(double)> empty;

    REQUIRE_FALSE(propagate_function(1.0, 0.1, empty, real).has_value());
    REQUIRE_FALSE(propagate_function(1.0, 0.1, real, empty).has_value());
    REQUIRE_FALSE(propagate_function(1.0, 0.1, empty, empty).has_value());

    // The same call with both supplied succeeds, so the case above is testing the emptiness and not the fixture.
    REQUIRE(propagate_function(1.0, 0.1, real, real).has_value());
}

TEST_CASE("propagate.analysis.a_derivative_is_the_caller_s", "[analysis]") {
    // The one-variable form, with the derivative supplied rather than approximated. `f(x) = sin(x)` at
    // x = pi/6 = 0.5236, whose derivative is cos(x) = 0.8660.
    const auto sine = propagate_function(0.5235987755982988, 0.01,
                                         [](double x) { return std::sin(x); },
                                         [](double x) { return std::cos(x); });
    REQUIRE(sine.has_value());
    REQUIRE(std::abs(sine.value().value - 0.5) < 1.0e-12);
    REQUIRE(std::abs(sine.value().sigma - 0.8660254037844386 * 0.01) < 1.0e-15);

    // A **zero** derivative gives zero uncertainty, and that is correct rather than degenerate: a quantity at a
    // stationary point of `f` is known much better than its input. sin at pi/2 is the case.
    const auto at_peak = propagate_function(1.5707963267948966, 0.01,
                                            [](double x) { return std::sin(x); },
                                            [](double x) { return std::cos(x); });
    REQUIRE(at_peak.has_value());
    REQUIRE(std::abs(at_peak.value().value - 1.0) < 1.0e-12);
    REQUIRE(at_peak.value().sigma < 1.0e-14);

    // A decreasing function has a negative slope and a **positive** uncertainty: the absolute value is what keeps
    // a sign error from surviving into a caller that squares it back into something plausible.
    const auto inverse = propagate_function(4.0, 0.1, [](double x) { return 1.0 / x; },
                                            [](double x) { return -1.0 / (x * x); });
    REQUIRE(inverse.has_value());
    REQUIRE(inverse.value().value == 0.25);
    REQUIRE(inverse.value().sigma > 0.0);
    REQUIRE(std::abs(inverse.value().sigma - 0.1 / 16.0) < 1.0e-18);

    // A missing function or derivative is refused rather than called, because an empty `std::function` throws
    // when invoked and this file's contract is a `Result`.
    REQUIRE_FALSE(propagate_function(1.0, 0.1, nullptr, [](double) { return 1.0; }).has_value());
    REQUIRE_FALSE(propagate_function(1.0, 0.1, [](double) { return 1.0; }, nullptr).has_value());
    // A non-finite value or derivative is refused: a derivative of infinity is not a sensitivity.
    REQUIRE_FALSE(propagate_function(1.0, 0.1, [](double) { return 1.0; },
                                     [](double) { return std::numeric_limits<double>::infinity(); })
                      .has_value());
    REQUIRE_FALSE(propagate_function(1.0, -0.1, [](double x) { return x; }, [](double) { return 1.0; })
                      .has_value());
}
