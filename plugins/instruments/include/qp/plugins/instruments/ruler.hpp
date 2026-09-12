/**
 * @file ruler.hpp
 * @brief A graduated scale: the simplest honest instrument, and the base every other one shares.
 *
 * ## The one decision this file makes
 *
 * A real device cannot report finer than its graduations, and the number it reports is a **rounded** value --
 * so what it hands back is not the truth with a small error added, it is the truth snapped to the nearest tick.
 * That is the whole error model of a ruler, and it is worth stating because the alternative is the mistake this
 * platform exists to prevent: inventing a reading with more digits than the device can resolve, or attaching an
 * error bar that has nothing to do with how the number was produced.
 *
 * So `measure` does two things and neither is optional:
 *
 *   1. round `truth` to the nearest multiple of the resolution -- the reading a user would write down;
 *   2. report the standard uncertainty that rounding implies, `resolution / sqrt(12)`.
 *
 * Step 2 is the rectangular-distribution result and not a guess: a value rounded to the nearest tick is
 * uniformly distributed within half a tick of the true value, and the standard deviation of a uniform
 * distribution of width `w` is `w / sqrt(12)`. The claim "this device knows the value to within 1 mm" becomes a
 * number a student can propagate, and `resolution_uncertainty` in `runtime/instrument` is where that formula
 * lives so every device in this directory reports it the same way.
 *
 * ## What is deliberately absent
 *
 * No random noise. It is tempting -- a device that returns the same number twice looks fake -- and it is wrong
 * here for two reasons. A reading carries an uncertainty, and if the device also jittered, the jitter would be
 * an error the reported uncertainty does not cover: the number would be **inconsistent with its own error
 * bar**, which is exactly the failure mode this platform is built to catch. And a repeated measurement is
 * supposed to repeat: in a real lab the spread comes from the experimenter and the setup, not from a device
 * that invents a different answer each time you ask. A student who wants a spread takes several readings.
 *
 * ## Why the devices share one base class
 *
 * Every one of them has the same three jobs: hold a resolution, refuse a resolution it cannot honour, and
 * quantise. Only the numbers differ -- the increments a rule and a caliper can report, and the quantity they
 * measure. Writing that logic once means a device cannot get the uncertainty formula subtly wrong, and the
 * subclasses in `instruments.hpp` are then a description and two bounds each. `IInstrument` is not frozen, so
 * this is a decision that can be revisited: the reopening condition would be a device whose error model is
 * **not** quantisation -- a noisy photodetector, say -- and that device would derive from the interface directly
 * rather than from this class.
 *
 * ## Why the definitions are not here
 *
 * Every function in this class is declared and defined in `ruler.cpp`. That is the project's normal split, and
 * here it is also what makes the contracts **readable by the gate**: the gate finds a contract block by looking
 * at the lines immediately after it, and a declaration starting with `[[nodiscard]]` puts the attribute on the
 * line the block ends against -- so a two-line declaration is a contract no checker can see. A one-line
 * declaration in the header, contract attached, definition in the source: one place to read the promise, one
 * place to read what it does.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `measure` returns a value on a tick and the uncertainty that tick implies
 * @errors      See each declaration
 * @frozen      no
 * @tests       instrument.ruler.reports_the_tick_it_can_see
 */
#pragma once

#include <qp/runtime/instrument/instrument.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace qp::plugins::instruments {

/**
 * @brief A device that reads to a graduation, with an optional calibration offset.
 *
 * @ownership   owns
 * @thread      main
 * @pre         `desc.dim` is the dimension of every reading, and `desc.finest_resolution > 0`
 * @post        none
 * @invariant   `resolution()` is in `[desc.finest_resolution, coarsest_resolution]` and on a tick of the finest
 * @errors      Reports through `diag::Result` rather than throwing
 * @frozen      no
 * @tests       instrument.ruler.reports_the_tick_it_can_see,
 *              instrument.ruler.an_adjustable_scale_snaps_to_its_ticks
 */
class GraduatedInstrument final : public qp::runtime::IInstrument {
public:
    /**
     * @brief Builds a device from its description, its coarsest setting and its offset.
     *
     * @param desc                The declaration: id, label, quantity, dimension, finest increment, adjustable.
     * @param coarsest_resolution The largest increment this device can be set to. Pass `desc.finest_resolution`
     *                            for a device with one setting.
     * @param offset              The **uncorrected** error of this particular unit: what the device reads minus
     *                            what is there, in SI units. Zero is a calibrated instrument, which is what a
     *                            teaching lab's kit is supposed to be, and it is the default because a nonzero
     *                            default would silently bias every reading in the platform.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `desc.finest_resolution > 0`, `coarsest_resolution >= desc.finest_resolution`, `desc.dim` set
     * @post        `resolution() == desc.finest_resolution`, the setting a device is at when switched on
     * @invariant   The description is returned by reference and never copied per call
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     */
    GraduatedInstrument(qp::runtime::InstrumentDesc desc, double coarsest_resolution,
                        double offset = 0.0) noexcept;

    /**
     * @brief Not copyable and not movable, and both deletions are deliberate.
     *
     * `IInstrument` deletes its copy operations -- two devices claiming one id is two answers to "which device
     * took this reading" -- and that deletion is inherited as a **deleted move**, which is not what it looks
     * like from the outside.
     *
     * The move is left deleted rather than restored, and that is the interesting half. A device's **identity**
     * is its address: `InstrumentRegistry` holds non-owning pointers, so a registered device that moved would
     * leave the registry pointing at the moved-from husk -- a device that answers with a default resolution and
     * an empty id, with nothing to say it went wrong. The cost of forbidding the move is that a rack cannot be
     * a `std::vector`, which is what the first version of `ruler.cpp` tried and what the compiler's
     * `construct_at` error was the type refusing. The rack is an `std::array` built in place instead: fixed
     * size, nothing relocated, and a size that is a compile-time fact.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   The address of a device is stable for its lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     */
    GraduatedInstrument(GraduatedInstrument&&) noexcept = delete;

    /// @brief See the move constructor's contract: this type has one address for its whole life.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Nothing is assigned to a device
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    GraduatedInstrument& operator=(GraduatedInstrument&&) noexcept = delete;

    /// @brief See the move constructor's contract: two devices claiming one id is two answers to one question.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Nothing is copied into a device
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    GraduatedInstrument(const GraduatedInstrument&) noexcept = delete;

    /// @brief See the copy constructor's contract.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Nothing is assigned to a device
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    GraduatedInstrument& operator=(const GraduatedInstrument&) noexcept = delete;

    ~GraduatedInstrument() override;

    /// @brief What this device is and what it measures.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   The reference stays valid for the object's lifetime
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       instrument.ruler.reports_the_tick_it_can_see
    [[nodiscard]] const qp::runtime::InstrumentDesc& describe() const noexcept override;

    /// @brief The increment this instrument is currently set to, in SI units.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        A positive value, never finer than `describe().finest_resolution`
    /// @invariant   Unchanged by `measure`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       instrument.ruler.an_adjustable_scale_snaps_to_its_ticks
    [[nodiscard]] double resolution() const noexcept override;

    /**
     * @brief Sets the increment, refusing one this device cannot report to.
     *
     * Two refusals, and they are different sentences to a user. A device whose precision is **fixed by
     * construction** -- a plastic rule moulded with 1 mm marks -- cannot be adjusted at all, and asking is a
     * category error the description already answers. A device that is adjustable but asked for an increment
     * outside its range is a request the user can fix by asking for something else.
     *
     * The requested increment is snapped to a whole number of the finest ticks. A ruler asked for 2.5 mm
     * graduations reports 3 mm, because a device can only be as fine as its marks: honouring 2.5 would put a
     * number in the uncertainty that the hardware cannot produce.
     *
     * @param resolution The requested increment, in SI units.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success `resolution()` is within `[finest, coarsest]` and a whole number of ticks
     * @invariant   On failure nothing changed
     * @errors      Reports through `diag::Result` rather than throwing: `fixed_precision` when
     *              `describe().adjustable` is false, `resolution_out_of_range` for a non-finite, non-positive or
     *              out-of-range request
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.ruler.a_fixed_scale_refuses_to_change,
     *              instrument.ruler.an_adjustable_scale_snaps_to_its_ticks
     */
    [[nodiscard]] qp::diag::Result<void> set_resolution(double resolution) override;

    /**
     * @brief Reads `truth` to the nearest graduation and reports what that rounding implies.
     *
     * @param truth The true value the model computed, in SI units.
     * @param ctx   The seed and sample index. Unused by this device, and see the file comment for why that is
     *              the honest model rather than an omission.
     *
     * @ownership   owns the reading
     * @thread      main
     * @pre         none
     * @post        The reading is on a tick of `resolution()`, and its uncertainty is `resolution/sqrt(12)`
     * @invariant   The same instrument, resolution and truth give the same reading: this device has no noise
     * @errors      `value_not_finite` for a truth that is not a number, because a model that produced an
     *              infinity must not be reported as a measurement with an error bar
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.ruler.reports_the_tick_it_can_see,
     *              instrument.ruler.a_reading_repeats_exactly,
     *              instrument.ruler.a_broken_model_is_not_a_reading
     */
    [[nodiscard]] qp::diag::Result<qp::runtime::UncertainValue> measure(
        double truth, const qp::runtime::MeasureContext& ctx) override;

    /**
     * @brief This unit's uncorrected error, in SI units.
     *
     * Exposed so a calibration exercise can ask, and so a test can hold the constructor to what it was given
     * rather than inferring it from a reading.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The value passed at construction
     * @invariant   Constant for the object's lifetime: a device does not drift inside a session
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.a_calibration_offset_shows_up_in_the_reading
     */
    [[nodiscard]] double offset() const noexcept;

private:
    qp::runtime::InstrumentDesc desc_;
    double coarsest_ = 1.0;
    double offset_ = 0.0;
    double resolution_ = 1.0;
};

}  // namespace qp::plugins::instruments
