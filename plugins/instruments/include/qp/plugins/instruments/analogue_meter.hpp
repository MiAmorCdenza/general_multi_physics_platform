/**
 * @file analogue_meter.hpp
 * @brief A device whose error is **not** quantisation: a relative term plus a fixed floor.
 *
 * ## Why this file exists, and what it proves
 *
 * `ruler.hpp`'s file comment ends with a reopening condition rather than a guess: the graduated device is the
 * right shape for an instrument whose error comes from rounding to a graduation, and "the reopening condition
 * would be a device whose error model is **not** quantisation -- a noisy photodetector, say -- and that device
 * would derive from the interface directly rather than from this class".
 *
 * This is that device, and it is one of the most common instruments in any lab: a meter whose specification
 * reads `+/- (p% of reading + c)`. A moving-coil ammeter, a bench multimeter on a poor range, a photodiode with a
 * transimpedance amplifier -- all of them have an error whose **size depends on what is being measured**, which
 * the graduated model cannot express at all. So this class derives straight from `IInstrument`, and the fact
 * that it fits without touching the interface is the answer to the question the reopening condition posed.
 *
 * ## The model, and why it is two terms
 *
 *     sigma = (p * |reading| + c) / sqrt(3)
 *
 * The two terms answer two different physical causes and a course needs both:
 *
 *   - `p * |reading|` is **proportional**. It comes from gain error, from a calibration curve's slope being
 *     slightly wrong, from anything that scales with the quantity. It is why a percentage specification becomes
 *     useless near zero: 3% of 1 mV is 0.03 mV, and no instrument can resolve that, so the manufacturer's
 *     specification is not the limit there;
 *   - `c` is a **floor**. It comes from offset error, from noise, from the last digit's resolution -- anything
 *     that does not care what the reading is. It is why a percentage specification becomes useless at full
 *     scale, where 3% of 10 V is 300 mV and the floor is irrelevant.
 *
 * The `1/sqrt(3)` is the rectangular distribution, the same convention `GraduatedInstrument` uses: a
 * specification gives a **bound**, and a bound is not a standard uncertainty. A specification that says `+/- x`
 * is saying the error lies within `x`, and the standard deviation of a symmetric distribution of half-width `x`
 * is `x/sqrt(3)`. Dividing by it is what makes this device's numbers comparable with the graduated devices',
 * which matters because a lab report that adds a half-width to a standard deviation is wrong by 73%.
 *
 * ## What it deliberately does not do
 *
 * No quantisation. A real meter displays digits, and the display's last digit is a real source of error -- but it
 * is already in `c` for most instruments, because the manufacturer's specification includes it. Rounding the
 * reading **as well** would count it twice, and double-counting an error source is the same mistake as ignoring
 * one: the number comes out wrong and nothing says so.
 *
 * No noise. Same argument as the graduated device: a reading carries an uncertainty, and a device that also
 * jittered would report a spread its own uncertainty does not cover. The same truth must give the same reading.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `measure` returns the truth and the uncertainty this device's specification implies
 * @errors      See each declaration
 * @frozen      no
 * @tests       instrument.analogue.the_uncertainty_grows_with_the_reading
 */
#pragma once

#include <qp/runtime/instrument/instrument.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace qp::plugins::instruments {

/**
 * @brief A meter specified as `+/- (relative * reading + floor)`.
 *
 * @ownership   owns
 * @thread      main
 * @pre         `desc.dim` is the dimension of every reading, and both error coefficients are non-negative
 * @post        none
 * @invariant   The same truth gives the same reading and the same uncertainty
 * @errors      Reports through `diag::Result` rather than throwing
 * @frozen      no
 * @tests       instrument.analogue.the_uncertainty_grows_with_the_reading
 */
class AnalogueMeter final : public qp::runtime::IInstrument {
public:
    /**
     * @brief Builds a meter from its declaration and its two error coefficients.
     *
     * @param desc     The declaration: id, label, quantity, dimension, finest resolution, adjustable.
     * @param relative The proportional error, as a fraction of the reading. `0.03` for a 3% specification.
     * @param floor    The fixed part of the error, in SI units. Not "the smallest digit": it is everything that
     *                 does not scale with the reading, which for a real specification includes the last digit.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `relative >= 0`, `floor >= 0`, `desc.finest_resolution > 0`
     * @post        `relative_error()` and `floor_error()` return what was passed, and `resolution()` is
     *              `desc.finest_resolution` -- the setting the device is on when it is switched on
     * @invariant   The description is returned by reference and never copied per call
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.analogue.the_uncertainty_grows_with_the_reading
     */
    AnalogueMeter(qp::runtime::InstrumentDesc desc, double relative, double floor) noexcept
        : desc_(std::move(desc)),
          relative_(relative),
          floor_(floor),
          // From the description, not a literal. The first version defaulted the member to `1.0` and left the
          // constructor to mean "start at the finest setting"; every reading was then reported at whatever the
          // default was, and `set_resolution(1e-6)` was refused as out of range on a device whose finest
          // resolution **is** 1e-6. A default that no device wants is a default that hides the omission.
          resolution_(desc_.finest_resolution) {}

    /// @brief Not copyable and not movable, for the reason the graduated device gives: a device's address is its
    /// identity to the registry that borrows it.
    AnalogueMeter(AnalogueMeter&&) noexcept = delete;
    AnalogueMeter& operator=(AnalogueMeter&&) noexcept = delete;
    AnalogueMeter(const AnalogueMeter&) noexcept = delete;
    AnalogueMeter& operator=(const AnalogueMeter&) noexcept = delete;
    ~AnalogueMeter() override = default;

    /**
     * @brief What this device is and what it measures.
     *
     * @ownership   borrows from this object
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Stable for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.analogue.the_uncertainty_grows_with_the_reading
     */
    [[nodiscard]] const qp::runtime::InstrumentDesc& describe() const noexcept override { return desc_; }

    /**
     * @brief The increment the display can show, in SI units.
     *
     * The **display's** increment, not the specification's: this device ranges over decades and its resolution
     * follows the range it is on, so `set_resolution` is a range change rather than a precision claim.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        A positive value, never finer than `describe().finest_resolution`
     * @invariant   Unchanged by `measure`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.analogue.an_analogue_meter_changes_range
     */
    [[nodiscard]] double resolution() const noexcept override { return resolution_; }

    /**
     * @brief Changes the range, refusing one the display cannot show.
     *
     * @param resolution The requested increment, in SI units.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success `resolution()` returns the new value
     * @invariant   On failure nothing changed
     * @errors      `fixed_precision` when the description says the precision cannot be changed, and
     *              `resolution_out_of_range` for a non-finite, non-positive or out-of-range request
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.analogue.an_analogue_meter_changes_range
     */
    [[nodiscard]] qp::diag::Result<void> set_resolution(double resolution) override;

    /**
     * @brief Reads `truth`, and reports the uncertainty this device's specification implies **at that reading**.
     *
     * The reading is the truth: this device does not quantise, and rounding it as well as reporting the
     * specification's uncertainty would count the display's digit twice -- see the file comment.
     *
     * @param truth The true value the model computed, in SI units.
     * @param ctx   The seed and sample index. Unused, and see the file comment for why that is the honest model.
     *
     * @ownership   owns the reading
     * @thread      main
     * @pre         none
     * @post        The reading's value is `truth` and its uncertainty is `(relative*|truth| + floor)/sqrt(3)`
     * @invariant   The uncertainty is non-zero even at zero reading, because a floor is a floor
     * @errors      `value_not_finite` for a truth that is not a number
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.analogue.the_uncertainty_grows_with_the_reading,
     *              instrument.analogue.a_zero_reading_still_has_an_uncertainty
     */
    [[nodiscard]] qp::diag::Result<qp::runtime::UncertainValue> measure(
        double truth, const qp::runtime::MeasureContext& ctx) override;

    /// @brief The proportional part of the specification, as a fraction of the reading.
    [[nodiscard]] double relative_error() const noexcept { return relative_; }

    /// @brief The fixed part of the specification, in SI units.
    [[nodiscard]] double floor_error() const noexcept { return floor_; }

private:
    qp::runtime::InstrumentDesc desc_;
    double relative_ = 0.0;
    double floor_ = 0.0;
    double resolution_ = 1.0;
};

}  // namespace qp::plugins::instruments
