/**
 * @file analogue_meter.cpp
 * @brief The two error coefficients, and the one division that makes a specification a standard uncertainty.
 */
#include <qp/plugins/instruments/analogue_meter.hpp>

#include <cmath>

namespace qp::plugins::instruments {

qp::diag::Result<void> AnalogueMeter::set_resolution(double resolution) {
    if (!desc_.adjustable) return qp::diag::ErrorCode::not_implemented;
    if (!std::isfinite(resolution) || resolution <= 0.0) return qp::diag::ErrorCode::invalid_argument;
    // A **range** change, so the bound is a ratio rather than an absolute limit: a meter's display has a fixed
    // number of digits, so the increment it can show is a fixed fraction of the range it is on and spans several
    // decades across the instrument's settings. The bound here is that span, expressed against the finest
    // resolution the description names.
    constexpr double kDecades = 1.0e4;
    if (resolution < desc_.finest_resolution || resolution > desc_.finest_resolution * kDecades) {
        return qp::diag::ErrorCode::invalid_argument;
    }
    resolution_ = resolution;
    return {};
}

qp::diag::Result<qp::runtime::UncertainValue> AnalogueMeter::measure(
    double truth, const qp::runtime::MeasureContext& ctx) {
    (void)ctx;
    if (!std::isfinite(truth)) return qp::diag::ErrorCode::invalid_argument;
    if (!std::isfinite(relative_) || !std::isfinite(floor_) || relative_ < 0.0 || floor_ < 0.0) {
        return qp::diag::ErrorCode::invalid_argument;
    }

    // The specification is a **bound**, and a bound is not a standard uncertainty: `+/- x` says the error lies
    // within `x` with no statement about its distribution, and the conventional reading of a manufacturer's
    // limit-of-error is a rectangular one. The standard deviation of a symmetric rectangular distribution of
    // half-width `x` is `x/sqrt(3)` -- the same conversion `runtime/instrument`'s `resolution_uncertainty` makes
    // for a graduation, so a report that adds one device's number to another's is adding like to like.
    constexpr double kRectangular = 1.7320508075688772935;  // sqrt(3)
    const double half_width = relative_ * std::abs(truth) + floor_;
    const double sigma = half_width / kRectangular;

    // The truth, unchanged. This device does not quantise: its display's last digit is already inside `floor_`,
    // because a manufacturer's specification is measured from the same display, and rounding the value as well
    // would count that digit twice.
    return qp::runtime::UncertainValue::measured(truth, sigma, desc_.dim);
}

}  // namespace qp::plugins::instruments
