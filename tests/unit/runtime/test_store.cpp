/**
 * @file test_store.cpp
 * @brief Tests for the measured-data structures.
 *
 * Test case ids match the @tests fields in the store headers byte for byte.
 *
 * Three claims carry the weight here, and each one is a place where a plausible
 * implementation is quietly wrong:
 *
 *   - **"unknown" is not "exact".** Turning an unquantified error into zero
 *     produces a confidently wrong uncertainty, which is the worst output this
 *     platform can emit.
 *   - **Welford, not the textbook formula.** `sum(x^2) - n*mean^2` loses
 *     catastrophic precision for readings clustered far from zero, which is what
 *     a lab series looks like.
 *   - **An empty statistic is absent, not zero.** A mean of zero for a series
 *     nobody measured is a number a report would happily print.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/runtime/store.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace qp::runtime;
using qp::units::Dim;

namespace {

/// @brief A dimension with one axis exponent set, for readable fixtures.
constexpr Dim dim_of(std::int8_t e0 = 0, std::int8_t e1 = 0, std::int8_t e2 = 0) {
    return Dim{e0, e1, e2, 0, 0, 0, 0};
}

/// @brief The sample standard deviation computed the naive way, for comparison.
double naive_stddev(const std::vector<double>& xs) {
    double sum = 0.0;
    for (const double x : xs) sum += x;
    const double mean = sum / static_cast<double>(xs.size());
    double sq = 0.0;
    for (const double x : xs) sq += x * x;
    const double variance =
        (sq - static_cast<double>(xs.size()) * mean * mean) /
        static_cast<double>(xs.size() - 1);
    return variance < 0.0 ? -1.0 : std::sqrt(variance);
}

}  // namespace

// ===========================================================================
// Uncertainty states
// ===========================================================================

TEST_CASE("store.uncertainty.states", "[store]") {
    SECTION("the three states are distinct") {
        const UncertainValue unknown = UncertainValue::unquantified(1.5, dim_of(0, 1, -2));
        const UncertainValue exact = UncertainValue::exact(100.0);
        const UncertainValue measured = UncertainValue::measured(1.5, 0.05, dim_of(0, 1, -2));

        REQUIRE(unknown.kind == UncertaintyKind::unknown);
        REQUIRE(exact.kind == UncertaintyKind::exact);
        REQUIRE(measured.kind == UncertaintyKind::standard);

        // The distinction that matters: an unquantified error must not be usable
        // in a propagation, while an exact value legitimately is. If these two
        // collapsed, "we did not measure the error" would silently become "there
        // is no error".
        REQUIRE_FALSE(unknown.is_usable());
        REQUIRE(exact.is_usable());
        REQUIRE(measured.is_usable());
    }

    SECTION("a negative uncertainty is clamped, not stored") {
        // A negative standard uncertainty is not a small uncertainty, it is a
        // malformed one. Storing it would make every downstream quadrature sum
        // still come out positive while the record was wrong.
        const UncertainValue v = UncertainValue::measured(1.0, -0.5);
        REQUIRE(v.u == 0.0);
        REQUIRE(v.kind == UncertaintyKind::standard);
    }

    SECTION("relative uncertainty refuses the three undefined cases") {
        REQUIRE(UncertainValue::measured(2.0, 0.5).relative_uncertainty().has_value());
        REQUIRE(*UncertainValue::measured(2.0, 0.5).relative_uncertainty() == 0.25);

        // Unknown: there is no u to divide.
        REQUIRE_FALSE(UncertainValue::unquantified(2.0).relative_uncertainty().has_value());
        // Zero value: the ratio is undefined, not infinite.
        REQUIRE_FALSE(UncertainValue::measured(0.0, 0.5).relative_uncertainty().has_value());
        // Exact: a zero denominator, and the ratio would be infinite.
        REQUIRE_FALSE(UncertainValue::exact(3.0).relative_uncertainty().has_value());

        // A negative value still has a meaningful relative uncertainty: it is a
        // signed quantity whose magnitude is uncertain, which happens whenever a
        // zero crossing is being located.
        REQUIRE(UncertainValue::measured(-2.0, 0.5).relative_uncertainty().has_value());
        REQUIRE(*UncertainValue::measured(-2.0, 0.5).relative_uncertainty() == 0.25);

        // Non-finite inputs produce no answer rather than a NaN that spreads.
        REQUIRE_FALSE(UncertainValue::measured(1.0, std::nan("")).relative_uncertainty()
                          .has_value());
        REQUIRE_FALSE(UncertainValue::measured(std::nan(""), 0.1).relative_uncertainty()
                          .has_value());
    }

    SECTION("kind names are stable") {
        STATIC_REQUIRE(std::string_view(to_string(UncertaintyKind::unknown)) == "unknown");
        STATIC_REQUIRE(std::string_view(to_string(UncertaintyKind::exact)) == "exact");
        STATIC_REQUIRE(std::string_view(to_string(UncertaintyKind::standard)) == "standard");
        STATIC_REQUIRE(static_cast<std::uint8_t>(UncertaintyKind::unknown) == 0);
    }
}

// ===========================================================================
// Dataset statistics
// ===========================================================================

TEST_CASE("store.dataset.statistics", "[store]") {
    SECTION("an empty dataset has no statistics at all") {
        const Dataset d{"empty", dim_of(0, 0, 1)};
        REQUIRE(d.empty());
        REQUIRE(d.size() == 0);
        REQUIRE(d.valid_count() == 0);

        // Nothing, not zero. A mean of 0.0 for a series nobody measured is a
        // number a report would print without complaint.
        REQUIRE_FALSE(d.mean().has_value());
        REQUIRE_FALSE(d.sample_stddev().has_value());
        REQUIRE_FALSE(d.standard_error().has_value());
        REQUIRE_FALSE(d.weighted_mean().has_value());
    }

    SECTION("a single reading has a mean and no spread") {
        Dataset d{"one", dim_of(0, 0, 1)};
        d.add(1.5, 0.01);
        REQUIRE(d.mean().has_value());
        REQUIRE(*d.mean() == 1.5);

        // The sample standard deviation needs n >= 2. Reporting 0 for one reading
        // would claim a spread was measured when none can be.
        REQUIRE_FALSE(d.sample_stddev().has_value());
        REQUIRE_FALSE(d.standard_error().has_value());
        REQUIRE_FALSE(d.mean().has_value() == false);
    }

    SECTION("mean, spread and the uncertainty of the mean") {
        Dataset d{"period", dim_of(0, 0, 1)};
        for (const double x : {1.50, 1.52, 1.49, 1.51}) d.add(x, 0.01);

        REQUIRE(d.size() == 4);
        REQUIRE(d.valid_count() == 4);
        REQUIRE(d.mean().has_value());
        REQUIRE(std::abs(*d.mean() - 1.505) < 1e-12);

        // The n-1 denominator: with n == 4 the difference from n is 15%, and it
        // matters most exactly where student labs live -- small n.
        const double s = *d.sample_stddev();
        REQUIRE(std::abs(s - 0.012909944487358056) < 1e-12);

        // The uncertainty of the mean is s/sqrt(n), which is NOT s. Reporting the
        // spread of the readings where the uncertainty of the mean was meant is
        // the classic error this accessor exists to separate.
        const double se = *d.standard_error();
        REQUIRE(std::abs(se - s / 2.0) < 1e-12);
        REQUIRE(se < s);
    }

    SECTION("invalid readings stay in the record and out of the statistics") {
        Dataset d{"with_reject", dim_of(0, 0, 1)};
        d.add(1.00, 0.01);
        d.add(1.10, 0.01);
        d.add(1.01, 0.01);

        // The analyst rejects the middle point. It stays visible in the record --
        // deleting it would make the decision invisible and the record dishonest.
        REQUIRE(d.reject(1).has_value());

        REQUIRE(d.size() == 3);
        REQUIRE(d.valid_count() == 2);
        REQUIRE(d.mean().has_value());
        REQUIRE(std::abs(*d.mean() - 1.005) < 1e-12);
    }

    SECTION("non-finite readings do not poison the result silently") {
        Dataset d{"nan", dim_of(0, 0, 1)};
        d.add(1.0, 0.01);
        d.add(std::nan(""), 0.01);
        // A NaN in the mean is at least visible, unlike a NaN hidden in a
        // variance. What must not happen is a plausible finite number.
        const std::optional<double> m = d.mean();
        REQUIRE(m.has_value());
        REQUIRE(std::isnan(*m));
    }
}

TEST_CASE("store.dataset.welford_precision", "[store]") {
    // The case that separates the two algorithms. These readings are clustered
    // near 1e8 with a spread of order 1: the textbook form subtracts two numbers
    // of order 1e16 whose difference is of order 1, losing every significant
    // digit, and can even produce a negative variance for the same data.
    const std::vector<double> xs = {1.0e8 + 1.0, 1.0e8 + 2.0, 1.0e8 + 3.0,
                                    1.0e8 + 4.0, 1.0e8 + 5.0};
    Dataset d{"clustered", dim_of(0, 0, 1)};
    for (const double x : xs) d.add(x, 1.0);

    const double exact = 1.5811388300841898;   // sqrt(2.5) for 1..5
    const double welford = *d.sample_stddev();
    const double naive = naive_stddev(xs);

    REQUIRE(std::abs(welford - exact) < 1e-9);

    // And the naive formula is demonstrably worse on this data -- which is the
    // reason the implementation uses Welford rather than the shorter expression.
    // If a future refactor "simplified" it, this assertion is what would catch it.
    const double welford_error = std::abs(welford - exact);
    const double naive_error = naive < 0.0 ? 1.0 : std::abs(naive - exact);
    REQUIRE(naive_error > welford_error);
}

TEST_CASE("store.dataset.weighted_mean", "[store]") {
    SECTION("a precise reading counts for more than a coarse one") {
        Dataset d{"weighted", dim_of(0, 0, 1)};
        d.add(10.0, 1.0);   // weight 1
        d.add(12.0, 0.1);   // weight 100

        const double w = *d.weighted_mean();
        // (10/1 + 12/0.01) / (1/1 + 1/0.01) = (10 + 1200) / 101
        REQUIRE(std::abs(w - (1210.0 / 101.0)) < 1e-12);
        REQUIRE(w > 11.9);                 // pulled towards the precise reading
        REQUIRE(*d.mean() == 11.0);        // the plain mean is not

        // The uncertainty of the weighted mean is smaller than either input's,
        // which is the entire point of combining measurements.
        const double uw = *d.weighted_mean_uncertainty();
        REQUIRE(uw < 0.1);
        REQUIRE(std::abs(uw - 1.0 / std::sqrt(101.0)) < 1e-12);
    }

    SECTION("unquantified readings cannot produce a weighted mean") {
        // There is no weight without an uncertainty, so the answer is absent
        // rather than a plain mean wearing a weighted label.
        Dataset d{"unquantified", dim_of(0, 0, 1)};
        d.add(UncertainValue::unquantified(10.0));
        d.add(UncertainValue::unquantified(12.0));
        REQUIRE_FALSE(d.weighted_mean().has_value());
        REQUIRE(d.weighted_mean_uncertainty().has_value() == false);
        REQUIRE(d.quantified_count() == 0);

        // The plain mean is still available: the readings carry information even
        // when their errors were never quantified.
        REQUIRE(*d.mean() == 11.0);
    }

    SECTION("an exact reading does not produce an infinite weight") {
        // An "exact" reading inside a weighted mean would carry infinite weight,
        // and 1/u^2 would be inf, so the mean would be inf or NaN. Refusing to
        // count it is the honest answer: mixing a definitional value into a
        // weighted mean is a modelling mistake.
        Dataset d{"exact_mixed", dim_of(0, 0, 1)};
        d.add(UncertainValue::exact(100.0));
        d.add(10.0, 0.1);
        const std::optional<double> w = d.weighted_mean();
        REQUIRE(w.has_value());
        REQUIRE(std::isfinite(*w));
        REQUIRE(std::abs(*w - 10.0) < 1e-12);
        REQUIRE(d.quantified_count() == 1);
    }

    SECTION("invalid readings are excluded from the weighted mean too") {
        Dataset d{"weighted_reject", dim_of(0, 0, 1)};
        d.add(10.0, 0.1);
        d.add(1.0e6, 0.1);   // a wild point
        REQUIRE(d.reject(1).has_value());
        REQUIRE(d.quantified_count() == 1);
        REQUIRE(std::abs(*d.weighted_mean() - 10.0) < 1e-12);
    }
}

TEST_CASE("store.dataset.valid_flags", "[store]") {
    Dataset d{"flags", dim_of(0, 0, 1)};
    UncertainValue v = UncertainValue::measured(1.0, 0.1);
    v.dim = dim_of(1, 0, 0);   // deliberately the wrong dimension
    d.add(v);

    // The dataset's dimension wins, so a caller cannot build a series whose mean
    // mixes metres and seconds by accident.
    REQUIRE(d.readings()[0].reading.dim == dim_of(0, 0, 1));
    REQUIRE(d.dim() == dim_of(0, 0, 1));
    REQUIRE(d.name() == "flags");

    d.set_name("renamed");
    REQUIRE(d.name() == "renamed");

    // A reading starts valid, and a timestamp is absent unless someone set one.
    REQUIRE(d.readings()[0].valid);
    REQUIRE_FALSE(d.readings()[0].t.has_value());

    // Rejection keeps the reading and flips the flag. Deleting it would make the
    // decision invisible: a reader could not tell an outlier that was rejected
    // from one that was never taken.
    REQUIRE(d.reject(0).has_value());
    REQUIRE_FALSE(d.readings()[0].valid);
    REQUIRE(d.size() == 1);
    REQUIRE(d.valid_count() == 0);
    REQUIRE_FALSE(d.mean().has_value());

    // And it is recoverable, because a mistaken rejection is easy to make.
    REQUIRE(d.restore(0).has_value());
    REQUIRE(d.readings()[0].valid);
    REQUIRE(d.valid_count() == 1);

    // An out-of-range index is reported rather than silently ignored.
    REQUIRE_FALSE(d.reject(9).has_value());
    REQUIRE_FALSE(d.restore(9).has_value());
}

// ===========================================================================
// Fit results
// ===========================================================================

TEST_CASE("store.fit.result_shape", "[store]") {
    SECTION("a fit with covariance reports coefficient uncertainties") {
        FitResult fit;
        fit.model = "linear";
        fit.coefficients = {2.0, 3.0};
        // Row-major 2x2: variances 0.04 and 0.09, covariance 0.01.
        fit.covariance = {0.04, 0.01, 0.01, 0.09};
        fit.residuals = {0.1, -0.1};
        fit.chi_squared = 1.25;
        fit.degrees_of_freedom = 3;
        fit.r_squared = 0.998;

        REQUIRE(fit.has_covariance());
        REQUIRE(std::abs(*fit.coefficient_uncertainty(0) - 0.2) < 1e-12);
        REQUIRE(std::abs(*fit.coefficient_uncertainty(1) - 0.3) < 1e-12);
        REQUIRE(std::abs(*fit.covariance_of(0, 1) - 0.01) < 1e-12);
        REQUIRE(std::abs(*fit.covariance_of(1, 0) - 0.01) < 1e-12);   // symmetric

        // The off-diagonal is kept because it is what lets a caller report a
        // correlated uncertainty later; a fit that stored only variances would
        // make that unobtainable.
        REQUIRE(*fit.covariance_of(0, 1) != 0.0);
        REQUIRE(fit.r_squared.has_value());
        REQUIRE(fit.degrees_of_freedom == 3);
        REQUIRE_FALSE(fit.is_exact());
    }

    SECTION("no covariance means no coefficient uncertainty") {
        FitResult fit;
        fit.coefficients = {2.0, 3.0};
        REQUIRE_FALSE(fit.has_covariance());
        REQUIRE_FALSE(fit.coefficient_uncertainty(0).has_value());
        REQUIRE_FALSE(fit.covariance_of(0, 1).has_value());
    }

    SECTION("a malformed covariance is refused rather than producing NaN") {
        FitResult fit;
        fit.coefficients = {1.0, 2.0, 3.0};   // needs a 3x3
        fit.covariance = {0.01, 0.0, 0.0, 0.01};   // only 2x2
        REQUIRE_FALSE(fit.has_covariance());
        REQUIRE_FALSE(fit.coefficient_uncertainty(0).has_value());
    }

    SECTION("a negative variance is not a small uncertainty") {
        // A malformed fit can produce a negative diagonal entry. Taking its
        // square root would be NaN, which then spreads silently through every
        // derived quantity in a report.
        FitResult fit;
        fit.coefficients = {1.0};
        fit.covariance = {-0.01};
        REQUIRE(fit.has_covariance());
        REQUIRE_FALSE(fit.coefficient_uncertainty(0).has_value());

        FitResult nan_fit;
        nan_fit.coefficients = {1.0};
        nan_fit.covariance = {std::nan("")};
        REQUIRE_FALSE(nan_fit.coefficient_uncertainty(0).has_value());
    }

    SECTION("an exact fit is recognised") {
        FitResult fit;
        fit.coefficients = {1.0};
        fit.chi_squared = 0.0;
        REQUIRE(fit.is_exact());
    }

    SECTION("an out-of-range index has no answer") {
        FitResult fit;
        fit.coefficients = {1.0};
        fit.covariance = {0.01};
        REQUIRE_FALSE(fit.coefficient_uncertainty(5).has_value());
        REQUIRE_FALSE(fit.covariance_of(0, 5).has_value());
        REQUIRE_FALSE(fit.covariance_of(5, 0).has_value());
    }
}
