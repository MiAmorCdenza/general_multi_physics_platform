/**
 * @file propagate.cpp
 * @brief The first-order propagation formulae every lab manual teaches, written once.
 */
#include <qp/plugins/analysis/propagate.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

namespace qp::plugins::analysis {
namespace {

/// @brief Whether an uncertainty a caller supplied is usable.
///
/// Negative and non-finite are refused together and separately from the value, because they are a different kind
/// of mistake: a non-finite number came out of a computation that failed, while a negative uncertainty is a typo
/// in a table. Both would produce a nonsense answer downstream, and neither would be noticed.
[[nodiscard]] bool usable_sigma(double sigma) noexcept {
    return std::isfinite(sigma) && sigma >= 0.0;
}

}  // namespace

std::optional<double> standard_error_of_mean(double sigma, std::size_t n) noexcept {
    if (n == 0 || !usable_sigma(sigma)) return std::nullopt;
    return sigma / std::sqrt(static_cast<double>(n));
}

diag::Result<Uncertain> combine(const std::vector<Term>& terms) {
    Uncertain out;
    double variance = 0.0;
    for (const Term& term : terms) {
        if (!std::isfinite(term.value) || !std::isfinite(term.coefficient)) {
            return diag::ErrorCode::invalid_argument;
        }
        if (!usable_sigma(term.sigma)) return diag::ErrorCode::invalid_argument;
        out.value += term.coefficient * term.value;
        const double contribution = term.coefficient * term.sigma;
        variance += contribution * contribution;
    }
    out.sigma = std::sqrt(variance);
    if (!std::isfinite(out.value) || !std::isfinite(out.sigma)) return diag::ErrorCode::invalid_argument;
    return out;
}

diag::Result<Uncertain> combine_correlated(const std::vector<double>& values,
                                           const std::vector<double>& coefficients,
                                           const std::vector<double>& covariance) {
    const std::size_t n = values.size();
    if (coefficients.size() != n) return diag::ErrorCode::invalid_argument;
    // An empty covariance is the independent case rather than an error: a caller that has no covariance has
    // exactly the information a diagonal matrix would carry, and making it build one would be ceremony.
    if (!covariance.empty() && covariance.size() != n * n) return diag::ErrorCode::invalid_argument;

    Uncertain out;
    double variance = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(values[i]) || !std::isfinite(coefficients[i])) {
            return diag::ErrorCode::invalid_argument;
        }
        out.value += coefficients[i] * values[i];
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            const double cov = covariance.empty() ? (i == j ? 0.0 : 0.0) : covariance[i * n + j];
            if (!std::isfinite(cov)) return diag::ErrorCode::invalid_argument;
            if (i == j && cov < 0.0) return diag::ErrorCode::invalid_argument;
            variance += coefficients[i] * coefficients[j] * cov;
        }
    }
    // A negative variance can survive the sum even when every diagonal entry is non-negative: that is a matrix
    // which is not positive semi-definite, and reporting `sqrt` of it would be a NaN that spreads through every
    // number derived from this one. Refused here, where the cause is still nameable.
    if (variance < 0.0) return diag::ErrorCode::invalid_argument;
    if (!std::isfinite(out.value)) return diag::ErrorCode::invalid_argument;
    out.sigma = std::sqrt(variance);
    return out;
}

diag::Result<Uncertain> propagate_function(double x, double sigma,
                                           const std::function<double(double)>& f,
                                           const std::function<double(double)>& derivative) {
    if (!f || !derivative) return diag::ErrorCode::invalid_argument;
    if (!std::isfinite(x) || !usable_sigma(sigma)) return diag::ErrorCode::invalid_argument;

    const double value = f(x);
    const double slope = derivative(x);
    if (!std::isfinite(value) || !std::isfinite(slope)) return diag::ErrorCode::invalid_argument;

    // The absolute value, because an uncertainty is a magnitude: a decreasing function has a negative slope and
    // a positive uncertainty, and a caller that forgot the sign would otherwise get a negative sigma that the
    // next combination would square back into a plausible answer. The `.value()` at the end would then be right
    // and the `.sigma` silently right too -- which is how a sign error survives.
    return Uncertain{value, std::abs(slope) * sigma};
}

diag::Result<Uncertain> ratio(double a, double sigma_a, double b, double sigma_b) {
    if (!std::isfinite(a) || !std::isfinite(b)) return diag::ErrorCode::invalid_argument;
    if (!usable_sigma(sigma_a) || !usable_sigma(sigma_b)) return diag::ErrorCode::invalid_argument;
    // A zero divisor is refused rather than divided by. The generic form would return an infinity, and the
    // first thing downstream to touch it would produce a NaN with nothing left to say where it came from.
    if (b == 0.0) return diag::ErrorCode::invalid_argument;

    const double value = a / b;
    if (!std::isfinite(value)) return diag::ErrorCode::invalid_argument;

    const double relative_a = sigma_a / a;
    const double relative_b = sigma_b / b;
    // `a == 0` is not refused: a numerator of zero with a known uncertainty is a legitimate measurement, and its
    // relative uncertainty is formally infinite -- but the product below is `value * ...`, and `value` is zero,
    // so the answer is zero rather than a NaN. That is the correct first-order result: `a/b` near zero does not
    // care how badly `a` is known, in absolute terms.
    double relative = 0.0;
    if (std::isfinite(relative_a) && std::isfinite(relative_b)) {
        relative = std::sqrt(relative_a * relative_a + relative_b * relative_b);
    }
    const double sigma = std::abs(value) * relative;
    if (!std::isfinite(sigma)) return diag::ErrorCode::invalid_argument;
    return Uncertain{value, sigma};
}

}  // namespace qp::plugins::analysis
