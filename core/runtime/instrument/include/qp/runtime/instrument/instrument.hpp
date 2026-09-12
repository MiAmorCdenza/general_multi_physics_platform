/**
 * @file instrument.hpp
 * @brief Instruments as first-class citizens: the contract a measuring device answers.
 *
 * ## Why this module exists before any instrument does
 *
 * Charter C6 says `IInstrument` and the simulation-model interface must be established **together**, "not
 * bolted on later". The reason is the shape of the whole platform: its differentiator is the closed loop
 * measure -> record -> uncertainty -> report, and an instrument is where that loop starts. A framework that treated
 * instruments as a kind of model -- something that produces numbers -- could not express the one thing that
 * makes a number a measurement, and adding the distinction afterwards would touch every consumer of a value.
 *
 * The interface here is deliberately small: what an instrument is asked is *one reading of one truth*, with
 * the precision it is set to and a seed. Everything else (a dial to turn, a calibration file, a manufacturer's
 * error model) is a plugin's business.
 *
 * ## R1 in the type system
 *
 * The taxonomy's first requirement for an instrument is that it **produces readings, not truth**. That is
 * enforced by the signature rather than by a rule:
 *
 *   - the argument is a plain `double` -- the truth the model computed, which the instrument may perturb;
 *   - the result is an `UncertainValue` -- a value, its standard uncertainty and its dimension.
 *
 * An instrument therefore **cannot** return a bare number, because a measurement without an uncertainty is
 * not a measurement, and an instrument able to return one could publish the truth it was handed as though it
 * had measured it. The separation is not a convention an author is asked to honour; it is the only shape the
 * compiler accepts.
 *
 * ## Promise 3 in one function
 *
 * Charter promise 3 is "changing an instrument's precision must actually change the final uncertainty". The
 * framework's part of that is `resolution_uncertainty`: the standard uncertainty a resolution implies under a
 * rectangular distribution, which is what a first-year lab is taught and what an instrument with no better
 * model should report. A plugin may override it with its own error model -- a calibration certificate, a
 * temperature coefficient -- but it starts from a defined answer rather than from a constant it invented.
 *
 * ## C4 applies here too
 *
 * An instrument is plugin code called by the host, so it is called through the same fault barrier as an
 * evaluator or an operator: `measure_guarded` is the only sanctioned way in, and a device that raises is
 * caught, recorded and eventually quarantined rather than taking the process with it.
 *
 * ## What an instrument is not
 *
 * It is not a generator of the truth, and it cannot move the state a model integrates. `measure` is
 * `const`-ish in spirit: a reading may be noisy and may depend on the seed, and it never writes back into
 * the experiment. An instrument that could change what it measures would make every recorded run
 * unreproducible in a way no seed could fix -- which is why the signature takes the truth **by value**.
 *
 * @ownership   observes (the registry borrows the instruments)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A reading carries a dimension, and an instrument may not publish the truth as measured
 * @errors      Reports through `diag::Result` and `MeasureRefusal`
 * @frozen      no
 * @tests       instrument.a_reading_is_never_a_bare_value
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/plugin/guard.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/units/dim.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qp::runtime {

/**
 * @brief The standard uncertainty a resolution implies, under a rectangular distribution.
 *
 * An instrument that reports to the nearest `resolution` is saying the true value lies somewhere inside that
 * increment, with no reason to prefer one part of it over another. The standard uncertainty of a uniform
 * distribution of width `w` is `w / sqrt(12)` -- the textbook Type-B evaluation, and the reason a finer
 * setting really does make a smaller uncertainty rather than merely a longer number.
 *
 * A non-positive or non-finite resolution contributes **zero**, which is the honest answer for an instrument
 * that claims infinite resolution: the framework cannot invent an error model for a device that declares it
 * has none, and returning a made-up constant would put a number in a report that nobody measured.
 *
 * @param resolution The increment the instrument reports to, in SI units of the quantity.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns 0 for a resolution that is not finite and positive, and `resolution / sqrt(12)`
 *              otherwise
 * @invariant   Strictly increasing in `resolution`: a finer setting never reports a larger uncertainty
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       instrument.resolution_becomes_an_uncertainty
 */
[[nodiscard]] inline double resolution_uncertainty(double resolution) noexcept {
    if (!std::isfinite(resolution) || resolution <= 0.0) return 0.0;
    return resolution / std::sqrt(12.0);
}

/**
 * @brief What an instrument measures, declared rather than guessed.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `id`, `quantity` and a positive `finest_resolution` for a usable instrument
 * @errors      noexcept
 * @frozen      no
 * @tests       instrument.a_reading_is_never_a_bare_value
 */
struct InstrumentDesc final {
    /// Stable identifier, e.g. "demo.ruler". Used in a run's record of what measured it.
    std::string id{};
    /// What a user sees in a device list.
    std::string label{};
    /// The quantity it measures, e.g. "length".
    std::string quantity{};
    /// The dimension of every reading it produces. Part of the declaration because a reading without one is a
    /// number, and this platform exists to keep them apart.
    units::Dim dim{};
    /// The finest increment it can report to, in SI units. A reading finer than this is a lie the instrument
    /// cannot tell.
    double finest_resolution = 0.0;
    /// Whether `set_resolution` may be called. False for a device whose precision is fixed by construction.
    bool adjustable = false;
};

/**
 * @brief Where a reading was taken from, so two readings can be compared honestly.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The same seed and index give the same reading from the same instrument
 * @errors      noexcept
 * @frozen      no
 * @tests       instrument.the_same_seed_gives_the_same_reading
 */
struct MeasureContext final {
    /// The run's seed, already mixed with the instrument's slot. An instrument that reads without a seed is
    /// not reproducible, and a run whose measurements cannot be repeated is an anecdote.
    std::uint64_t seed = 0;
    /// Which sample of the run this is, so an instrument can model drift over a series if it wants to.
    std::uint64_t sample = 0;
};

/**
 * @brief Why an instrument refused to take a reading.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One code per distinct reason; `ok` is zero
 * @errors      noexcept
 * @frozen      yes -- tags frozen, the set may gain a reason
 * @tests       instrument.a_non_finite_truth_is_not_a_measurement
 */
enum class MeasureRefusal : std::uint8_t {
    ok = 0,
    /// The requested resolution is finer than the device can report, or coarser than it can use.
    resolution_out_of_range = 1,
    /// The instrument's precision is fixed and `set_resolution` is not available.
    fixed_precision = 2,
    /// The truth it was handed is not a number: a model that produced an infinity or a NaN must not be
    /// reported as a measurement with an error bar.
    value_not_finite = 3,
};

/// @brief Stable short name of a refusal, for a message or a log line.
[[nodiscard]] constexpr const char* to_string(MeasureRefusal r) noexcept {
    switch (r) {
        case MeasureRefusal::ok: return "ok";
        case MeasureRefusal::resolution_out_of_range: return "resolution_out_of_range";
        case MeasureRefusal::fixed_precision: return "fixed_precision";
        case MeasureRefusal::value_not_finite: return "value_not_finite";
    }
    return "unknown";
}

/**
 * @brief A measuring device. Implementations are plugins.
 *
 * @ownership   observes (the plugin owns itself)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `describe()` is stable for the object's lifetime, except `resolution` as set below
 * @errors      Reports through `diag::Result` rather than throwing
 * @frozen      no
 * @tests       instrument.a_reading_is_never_a_bare_value
 */
class IInstrument {
public:
    IInstrument() = default;
    virtual ~IInstrument() = default;
    IInstrument(const IInstrument&) = delete;
    IInstrument& operator=(const IInstrument&) = delete;

    /**
     * @brief What this device is and what it measures.
     *
     * @ownership   borrows from this object
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   The reference stays valid for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.a_reading_is_never_a_bare_value
     */
    [[nodiscard]] virtual const InstrumentDesc& describe() const noexcept = 0;

    /**
     * @brief The increment this instrument is currently set to, in SI units.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        A positive value, at most `describe().finest_resolution`... at least as coarse as
     *              `finest_resolution` and never finer
     * @invariant   Unchanged by `measure`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.precision_changes_the_uncertainty
     */
    [[nodiscard]] virtual double resolution() const noexcept = 0;

    /**
     * @brief Sets the resolution, refusing one the device cannot honour.
     *
     * @param resolution The requested increment, in SI units.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success `resolution()` returns the new value and later readings report the uncertainty
     *              it implies
     * @invariant   On failure neither the resolution nor anything else changes
     * @errors      `fixed_precision` when the description says the precision cannot be changed, and
     *              `resolution_out_of_range` for a value the device cannot honour
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.precision_changes_the_uncertainty,
     *              instrument.a_fixed_precision_device_refuses
     */
    [[nodiscard]] virtual diag::Result<void> set_resolution(double resolution) = 0;

    /**
     * @brief Takes one reading of `truth`.
     *
     * @param truth The true value the model computed. By **value**, because an instrument must not be able to
     *              change what it measures: a device that wrote back into the experiment would make every
     *              recorded run unreproducible in a way no seed could fix.
     * @param ctx   The seed and sample index, so a reading is repeatable.
     *
     * @ownership   owns the returned reading
     * @thread      main
     * @pre         none
     * @post        On success the reading carries this instrument's dimension and a standard uncertainty that
     *              depends on `resolution()`
     * @invariant   The same instrument, resolution, truth, seed and sample give the same reading -- asserted
     *              by `instrument.the_same_seed_gives_the_same_reading` rather than assumed, because the noise
     *              model is the plugin's and a plugin can get it wrong
     * @errors      `MeasureRefusal::value_not_finite` for a truth that is not finite, and whatever the device
     *              refuses beyond that
     * @complexity  implementation-defined
     * @nondet      the seed -- nothing else, and never the clock
     * @frozen      no
     * @tests       instrument.a_reading_is_never_a_bare_value,
     *              instrument.precision_changes_the_uncertainty,
     *              instrument.the_same_seed_gives_the_same_reading,
     *              instrument.a_non_finite_truth_is_not_a_measurement
     */
    [[nodiscard]] virtual diag::Result<UncertainValue> measure(double truth,
                                                              const MeasureContext& ctx) = 0;
};

/**
 * @brief Calls an instrument through the plugin fault barrier.
 *
 * The only sanctioned way for the host to take a reading, for the reason charter C4 exists: an instrument is
 * plugin code, and a device that raises must not take the process with it. The label is the instrument's id,
 * because that is what a device list and a run's record name.
 *
 * @param instrument The device. Must outlive the call.
 * @param truth      The true value to measure.
 * @param ctx        The seed and sample index.
 * @param label      What to attribute a fault to; pass the instrument's id.
 * @param faults     Where a fault is recorded, or null for the barrier without the counting.
 *
 * @ownership   observes `instrument`
 * @thread      main
 * @pre         `instrument` is not null
 * @post        A reading, or a failure: the instrument's own refusal, or `plugin_fault` when it misbehaved
 * @invariant   Never throws, whatever the instrument does
 * @errors      noexcept
 * @complexity  O(1) plus the reading
 * @nondet      none
 * @frozen      no
 * @tests       instrument.the_host_calls_through_the_fault_barrier
 */
[[nodiscard]] inline diag::Result<UncertainValue> measure_guarded(IInstrument& instrument, double truth,
                                                                 const MeasureContext& ctx,
                                                                 std::string_view label,
                                                                 plugin::FaultLog* faults) noexcept {
    return plugin::call_guarded(label, faults,
                                [&instrument, truth, &ctx] { return instrument.measure(truth, ctx); });
}

/**
 * @brief The instruments the host knows about.
 *
 * @ownership   observes (never owns an instrument)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   An id appears at most once
 * @errors      Reports through `diag::Result`
 * @frozen      no
 * @tests       instrument.registry.one_id_one_instrument
 */
class InstrumentRegistry final {
public:
    InstrumentRegistry() = default;
    InstrumentRegistry(const InstrumentRegistry&) = delete;
    InstrumentRegistry& operator=(const InstrumentRegistry&) = delete;

    /**
     * @brief Registers a device.
     *
     * @ownership   observes (`instrument` outlives the registration)
     * @thread      main
     * @pre         `instrument` is not null and its description has an id
     * @post        On success `find(id)` returns it
     * @invariant   On failure the registry is unchanged
     * @errors      noexcept; `invalid_argument` for a null instrument or an empty id; `duplicate_connection` for an id
     *              already registered, because two devices under one id would make "what measured this"
     *              depend on registration order
     * @complexity  O(devices)
     * @nondet      none
     * @frozen      no
     * @tests       instrument.registry.one_id_one_instrument
     */
    [[nodiscard]] diag::Result<void> add(IInstrument* instrument) noexcept;

    /// @brief Removes a device, for a plugin that is unloading.
    [[nodiscard]] diag::Result<void> remove(std::string_view id) noexcept;

    /// @brief The device registered under `id`, or null.
    [[nodiscard]] IInstrument* find(std::string_view id) const noexcept;

    /// @brief Every registered device, in registration order.
    [[nodiscard]] std::vector<IInstrument*> all() const noexcept;

    /// @brief Every device whose precision can be changed, in registration order.
    ///
    /// The list a "set your instrument" dialog should offer, computed here so the property can be asserted
    /// without a dialog.
    [[nodiscard]] std::vector<IInstrument*> adjustable() const noexcept;

    /// @brief Number of registered devices.
    [[nodiscard]] std::size_t size() const noexcept { return instruments_.size(); }

private:
    std::vector<IInstrument*> instruments_{};
};

}  // namespace qp::runtime
