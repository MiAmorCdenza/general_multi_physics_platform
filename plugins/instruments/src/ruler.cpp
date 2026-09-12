/**
 * @file ruler.cpp
 * @brief The device every other one derives from: hold a resolution, refuse one you cannot honour, quantise.
 *
 * The definitions live here rather than in the header for the reason the header's file comment gives: a contract
 * block is found by the lines immediately after it, so a declaration that starts with `[[nodiscard]]` pushes the
 * declaration one line further down than the checker looks. One-line declarations with their contracts in
 * `ruler.hpp`, bodies here -- one place to read each promise, one place to read each implementation.
 */
#include <qp/plugins/instruments/ruler.hpp>

#include <utility>

namespace qp::plugins::instruments {

GraduatedInstrument::GraduatedInstrument(qp::runtime::InstrumentDesc desc, double coarsest_resolution,
                                        double offset) noexcept
    : desc_(std::move(desc)),
      coarsest_(coarsest_resolution),
      offset_(offset),
      resolution_(desc_.finest_resolution) {}

GraduatedInstrument::~GraduatedInstrument() = default;

const qp::runtime::InstrumentDesc& GraduatedInstrument::describe() const noexcept { return desc_; }

double GraduatedInstrument::resolution() const noexcept { return resolution_; }

double GraduatedInstrument::offset() const noexcept { return offset_; }

qp::diag::Result<void> GraduatedInstrument::set_resolution(double resolution) {
    if (!desc_.adjustable) return qp::diag::ErrorCode::not_implemented;
    if (!std::isfinite(resolution) || resolution < desc_.finest_resolution || resolution > coarsest_) {
        return qp::diag::ErrorCode::invalid_argument;
    }
    // Snapped to a whole tick, and the **nearest** one: a request of 1.4 ticks is honoured as 1 tick, because
    // honouring 1.4 would put a number in the uncertainty that the graduations cannot produce. Rounding up would
    // claim a finer resolution than the device has, which is the direction that produces a confident wrong
    // error bar.
    const double ticks = std::floor(resolution / desc_.finest_resolution + 0.5);
    const double snapped = ticks * desc_.finest_resolution;
    resolution_ = snapped > coarsest_ ? coarsest_ : snapped;
    return {};
}

qp::diag::Result<qp::runtime::UncertainValue> GraduatedInstrument::measure(
    double truth, const qp::runtime::MeasureContext& ctx) {
    (void)ctx;
    if (!std::isfinite(truth)) return qp::diag::ErrorCode::invalid_argument;

    // What the device displays: the truth plus this unit's uncorrected error, snapped to the nearest mark.
    const double shown = truth + offset_;
    const double ticks = std::floor(shown / resolution_ + 0.5);
    const double value = ticks * resolution_;

    return qp::runtime::UncertainValue::measured(
        value, qp::runtime::resolution_uncertainty(resolution_), desc_.dim);
}

}  // namespace qp::plugins::instruments
