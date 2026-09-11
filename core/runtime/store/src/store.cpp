/**
 * @file store.cpp
 * @brief Implementation of the measured-data structures.
 */
#include <qp/runtime/store/store.hpp>

#include <cmath>

namespace qp::runtime {
namespace {

/// @brief Whether `v` is a value the statistics below can use.
[[nodiscard]] bool is_finite(double v) noexcept { return std::isfinite(v); }

}  // namespace

std::optional<double> UncertainValue::relative_uncertainty() const noexcept {
    // Three refusals, each for its own reason:
    //   unknown  -- there is no u to divide;
    //   zero     -- the ratio is undefined, not infinite;
    //   non-finite -- a NaN or inf would propagate into every derived quantity.
    if (kind != UncertaintyKind::standard) return std::nullopt;
    if (value == 0.0) return std::nullopt;
    if (!is_finite(u) || !is_finite(value)) return std::nullopt;
    return u / std::abs(value);
}

void Dataset::add(UncertainValue reading) noexcept {
    // The dataset's dimension wins. A caller that mixed dimensions would
    // otherwise build a series whose mean is meaningless, and the error would
    // surface as a wrong number rather than as a rejection.
    reading.dim = dim_;
    Measurement m;
    m.reading = reading;
    items_.push_back(m);
}

void Dataset::add(double value, double uncertainty) noexcept {
    add(UncertainValue::measured(value, uncertainty, dim_));
}

diag::Result<void> Dataset::reject(std::size_t index) noexcept {
    if (index >= items_.size()) return diag::ErrorCode::out_of_range;
    items_[index].valid = false;
    return {};
}

diag::Result<void> Dataset::restore(std::size_t index) noexcept {
    if (index >= items_.size()) return diag::ErrorCode::out_of_range;
    items_[index].valid = true;
    return {};
}

std::size_t Dataset::valid_count() const noexcept {
    std::size_t n = 0;
    for (const Measurement& m : items_) {
        if (m.valid) ++n;
    }
    return n;
}

std::size_t Dataset::quantified_count() const noexcept {
    std::size_t n = 0;
    for (const Measurement& m : items_) {
        if (!m.valid) continue;
        if (m.reading.kind == UncertaintyKind::standard && is_finite(m.reading.u)) ++n;
    }
    return n;
}

std::optional<double> Dataset::mean() const noexcept {
    if (valid_count() == 0) return std::nullopt;
    double sum = 0.0;
    std::size_t n = 0;
    for (const Measurement& m : items_) {
        if (!m.valid) continue;
        sum += m.reading.value;
        ++n;
    }
    return sum / static_cast<double>(n);
}

std::optional<double> Dataset::sample_stddev() const noexcept {
    // Welford's online algorithm. The textbook form `sum(x^2) - n*mean^2`
    // subtracts two large nearly equal numbers whenever the values are far from
    // zero and their spread is small -- which is the ordinary case for a lab
    // reading -- and can even return a small negative variance, whose square root
    // is NaN. This form has no such cancellation.
    double mean_w = 0.0;
    double m2 = 0.0;
    std::size_t n = 0;
    for (const Measurement& m : items_) {
        if (!m.valid) continue;
        ++n;
        const double delta = m.reading.value - mean_w;
        mean_w += delta / static_cast<double>(n);
        m2 += delta * (m.reading.value - mean_w);
    }
    if (n < 2) return std::nullopt;

    // m2 is a sum of squares, so it cannot be negative in exact arithmetic; a
    // negative value here would mean the inputs were not finite. Guard rather
    // than take a square root of a negative number.
    if (m2 < 0.0) return std::nullopt;
    return std::sqrt(m2 / static_cast<double>(n - 1));
}

std::optional<double> Dataset::standard_error() const noexcept {
    const std::optional<double> s = sample_stddev();
    if (!s.has_value()) return std::nullopt;
    const std::size_t n = valid_count();
    if (n == 0) return std::nullopt;
    return *s / std::sqrt(static_cast<double>(n));
}

std::optional<double> Dataset::weighted_mean() const noexcept {
    double weight_sum = 0.0;
    double weighted = 0.0;
    for (const Measurement& m : items_) {
        if (!m.valid) continue;
        if (m.reading.kind != UncertaintyKind::standard) continue;
        if (!is_finite(m.reading.u) || m.reading.u <= 0.0) continue;
        if (!is_finite(m.reading.value)) continue;
        const double w = 1.0 / (m.reading.u * m.reading.u);
        weight_sum += w;
        weighted += w * m.reading.value;
    }
    // A zero-uncertainty reading carries infinite weight, so it is skipped rather
    // than allowed to produce inf. An "exact" reading inside a weighted mean is a
    // modelling mistake, and refusing to answer is better than answering inf.
    if (weight_sum <= 0.0) return std::nullopt;
    return weighted / weight_sum;
}

std::optional<double> Dataset::weighted_mean_uncertainty() const noexcept {
    double weight_sum = 0.0;
    for (const Measurement& m : items_) {
        if (!m.valid) continue;
        if (m.reading.kind != UncertaintyKind::standard) continue;
        if (!is_finite(m.reading.u) || m.reading.u <= 0.0) continue;
        if (!is_finite(m.reading.value)) continue;
        weight_sum += 1.0 / (m.reading.u * m.reading.u);
    }
    if (weight_sum <= 0.0) return std::nullopt;
    return 1.0 / std::sqrt(weight_sum);
}

bool FitResult::has_covariance() const noexcept {
    if (covariance.empty()) return false;
    const std::size_t n = coefficients.size();
    return covariance.size() == n * n;
}

std::optional<double> FitResult::coefficient_uncertainty(std::size_t i) const noexcept {
    if (!has_covariance()) return std::nullopt;
    if (i >= coefficients.size()) return std::nullopt;
    const double variance = covariance[i * coefficients.size() + i];
    // A negative variance is not a small uncertainty, it is a malformed fit.
    // Returning NaN here would spread silently through every derived quantity in
    // the report, so the answer is "no answer".
    if (!is_finite(variance) || variance < 0.0) return std::nullopt;
    return std::sqrt(variance);
}

std::optional<double> FitResult::covariance_of(std::size_t i, std::size_t j) const noexcept {
    if (!has_covariance()) return std::nullopt;
    const std::size_t n = coefficients.size();
    if (i >= n || j >= n) return std::nullopt;
    const double c = covariance[i * n + j];
    if (!is_finite(c)) return std::nullopt;
    return c;
}

}  // namespace qp::runtime
