/**
 * @file test_instrument.cpp
 * @brief Tests for the instrument contract: charter C6 and promise 3, as properties rather than prose.
 *
 * ## What is worth testing here
 *
 * The taxonomy in `standards/test-taxonomy.md` names exactly three properties for an instrument, and this
 * file is where they stop being intentions:
 *
 *   - **a reading, never the truth (R1)**: a reading is a value with an uncertainty and a dimension, and the
 *     interface cannot produce anything else. Checked by type as well as by behaviour -- a plugin that tried to
 *     return a bare `double` would not compile, which is the only enforcement that survives an author in a
 *     hurry;
 *   - **the error model follows the precision (promise 3)**: at a finer resolution the reported uncertainty is smaller, and the
 *     direction is asserted rather than the change alone. "It changed" would pass for an instrument whose
 *     uncertainty grew when you asked it to resolve more, which is the opposite of the promise;
 *   - **repeatable**: the same instrument, resolution, truth and seed give the same reading.
 *
 * ## Why the stubs are stubs
 *
 * A concrete instrument is content -- `plugins/instruments/` per the plan tree -- and arrives later. What the
 * **framework** owes is that such a plugin cannot be written in a way that breaks the three properties, and
 * that is what a stub can prove: the stub is a deliberately awkward instrument (fixed precision, a noisy one,
 * a broken one), and each case shows the framework catching what it must.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/runtime/instrument.hpp>

#include <qp/runtime/store/store.hpp>
#include <qp/units/dimensions.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace qp::runtime;

namespace diag = qp::diag;
namespace plugin = qp::plugin;

namespace {

/// @brief An instrument with a settable resolution and a noise model that depends on the seed.
///
/// Deliberately ordinary: what a ruler, a stopwatch or a multimeter has in common is exactly this -- a
/// resolution the user can choose, a reading that scatters, and an uncertainty derived from the resolution.
class Ruler final : public IInstrument {
public:
    explicit Ruler(double finest = 1.0e-3) {
        desc_.id = "test.ruler";
        desc_.label = "Test ruler";
        desc_.quantity = "length";
        desc_.dim = qp::units::dims::length;
        desc_.finest_resolution = finest;
        desc_.adjustable = true;
        resolution_ = finest;
    }

    [[nodiscard]] const InstrumentDesc& describe() const noexcept override { return desc_; }
    [[nodiscard]] double resolution() const noexcept override { return resolution_; }

    [[nodiscard]] diag::Result<void> set_resolution(double resolution) override {
        if (!std::isfinite(resolution) || resolution < desc_.finest_resolution) {
            return diag::ErrorCode::out_of_range;
        }
        resolution_ = resolution;
        return {};
    }

    [[nodiscard]] diag::Result<UncertainValue> measure(double truth,
                                                       const MeasureContext& ctx) override {
        if (!std::isfinite(truth)) return diag::ErrorCode::invalid_argument;

        // The reading is quantised to the resolution and then scattered by a seed-derived offset -- the shape
        // of every real measurement, and the reason the context carries a seed at all.
        const double quantised = std::round(truth / resolution_) * resolution_;
        const double scatter = (static_cast<double>(ctx.seed % 1000) / 1000.0 - 0.5) * resolution_;
        // The uncertainty comes from the resolution, through the framework's own model, so this stub cannot
        // quietly answer with a constant.
        const double u = resolution_uncertainty(resolution_);
        return UncertainValue::measured(quantised + scatter, u, desc_.dim);
    }

private:
    InstrumentDesc desc_{};
    double resolution_ = 0.0;
};

/// @brief An instrument whose precision is fixed by construction, like a digital clock.
class FixedReading final : public IInstrument {
public:
    FixedReading() {
        desc_.id = "test.fixed";
        desc_.label = "Fixed";
        desc_.quantity = "time";
        desc_.dim = qp::units::dims::time;
        desc_.finest_resolution = 1.0e-3;
        desc_.adjustable = false;
    }

    [[nodiscard]] const InstrumentDesc& describe() const noexcept override { return desc_; }
    [[nodiscard]] double resolution() const noexcept override { return desc_.finest_resolution; }
    [[nodiscard]] diag::Result<void> set_resolution(double) override {
        return diag::ErrorCode::not_implemented;
    }
    [[nodiscard]] diag::Result<UncertainValue> measure(double truth, const MeasureContext&) override {
        return UncertainValue::measured(truth, resolution_uncertainty(desc_.finest_resolution),
                                        desc_.dim);
    }

private:
    InstrumentDesc desc_{};
};

/// @brief An instrument that raises, for charter C4 at this call site.
class BrokenInstrument final : public IInstrument {
public:
    BrokenInstrument() {
        desc_.id = "test.broken";
        desc_.label = "Broken";
        desc_.quantity = "length";
        desc_.dim = qp::units::dims::length;
        desc_.finest_resolution = 1.0e-3;
        desc_.adjustable = false;
    }

    [[nodiscard]] const InstrumentDesc& describe() const noexcept override { return desc_; }
    [[nodiscard]] double resolution() const noexcept override { return desc_.finest_resolution; }
    [[nodiscard]] diag::Result<void> set_resolution(double) override {
        return diag::ErrorCode::not_implemented;
    }
    [[nodiscard]] diag::Result<UncertainValue> measure(double, const MeasureContext&) override {
        throw std::runtime_error{"the instrument has a memory bug"};
    }

private:
    InstrumentDesc desc_{};
};

}  // namespace

TEST_CASE("instrument.resolution_becomes_an_uncertainty", "[instrument]") {
    // Promise 3's mechanism, as arithmetic. A rectangular distribution over one increment is what a
    // first-year lab is taught for a Type-B evaluation, and it is the answer an instrument with no better
    // model should report rather than a constant it invented.
    const double coarse = resolution_uncertainty(1.0e-2);
    const double fine = resolution_uncertainty(1.0e-4);

    REQUIRE(coarse > 0.0);
    REQUIRE(fine > 0.0);
    REQUIRE(coarse > fine);
    // `w / sqrt(12)`, asserted against the formula rather than against a decimal: the constant is the physics.
    //
    // Compared against the formula to within a few units in the last place, and the tolerance is a measurement
    // rather than a convenience: this project's MinGW target is 32-bit i686, where `FLT_EVAL_METHOD == 2` lets
    // a constant-folded `w / sqrt(12)` differ from the library's already-rounded result in the final bit --
    // `1e-4` diverges, `1e-2` does not. The golden kernel case records the same divergence instead of hiding
    // it. Four ulps is far tighter than any change of model (which would move the value by a factor), so this
    // still fails if the formula changes and cannot pass by accident.
    const auto close_to_formula = [](double got, double w) {
        const double expected = w / std::sqrt(12.0);
        return std::abs(got - expected) <=
               4.0 * std::numeric_limits<double>::epsilon() * std::abs(expected);
    };
    REQUIRE(close_to_formula(coarse, 1.0e-2));
    REQUIRE(close_to_formula(fine, 1.0e-4));

    // A finer setting never reports a larger uncertainty -- the direction is the promise.
    double previous = std::numeric_limits<double>::infinity();
    for (const double resolution : {1.0, 1.0e-1, 1.0e-2, 1.0e-3, 1.0e-6}) {
        const double u = resolution_uncertainty(resolution);
        REQUIRE(u < previous);
        previous = u;
    }

    // An instrument claiming infinite resolution contributes nothing: the framework cannot invent an error
    // model for a device that declares it has none, and a made-up constant would put a number in a report
    // that nobody measured. Zero is the honest answer, and it is zero rather than absent because the caller
    // asked for the uncertainty *of a resolution*, not for a reading.
    REQUIRE(resolution_uncertainty(0.0) == 0.0);
    REQUIRE(resolution_uncertainty(-1.0) == 0.0);
    REQUIRE(resolution_uncertainty(std::numeric_limits<double>::infinity()) == 0.0);
    REQUIRE(resolution_uncertainty(std::numeric_limits<double>::quiet_NaN()) == 0.0);
}

TEST_CASE("instrument.a_reading_is_never_a_bare_value", "[instrument]") {
    // R1 by type. An instrument is handed the **truth** as a plain double and must return a reading: a value,
    // its standard uncertainty and its dimension. An interface that could return a bare number would let a
    // device publish the truth it was given as though it had measured it -- and every later stage (the record,
    // the report, the propagation) would take it at face value.
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<IInstrument&>().measure(1.0,
                                                                              std::declval<const MeasureContext&>())),
                                 diag::Result<UncertainValue>>);
    // And the reading carries the dimension: a measurement without one is a number, which is the distinction
    // this whole platform exists to keep.
    STATIC_REQUIRE(std::is_same_v<decltype(UncertainValue{}.dim), qp::units::Dim>);

    Ruler ruler;
    const diag::Result<UncertainValue> reading = ruler.measure(0.0123, MeasureContext{7, 0});
    REQUIRE(reading.has_value());
    // The kind is `standard`: the instrument quantified its error, which is what makes this a measurement
    // rather than a number with a guess attached.
    REQUIRE(reading.value().kind == UncertaintyKind::standard);
    REQUIRE(reading.value().u > 0.0);
    REQUIRE(reading.value().dim.is_dimensionless() == false);
    REQUIRE(reading.value().dim == qp::units::dims::length);
    // The declared identity travels with the device, so a record can say what measured it.
    REQUIRE(ruler.describe().id == "test.ruler");
    REQUIRE(ruler.describe().quantity == "length");
}

TEST_CASE("instrument.precision_changes_the_uncertainty", "[instrument]") {
    // Charter promise 3, at the instrument's own edge: "changing an instrument's precision must actually
    // change the final uncertainty". The direction is part of the assertion -- an uncertainty that *grew* when
    // the user asked for more resolution would satisfy "it changed" and contradict the promise.
    Ruler ruler{1.0e-5};
    REQUIRE(ruler.set_resolution(1.0e-2).has_value());
    const diag::Result<UncertainValue> coarse = ruler.measure(0.01234, MeasureContext{1, 0});
    REQUIRE(coarse.has_value());

    REQUIRE(ruler.set_resolution(1.0e-4).has_value());
    const diag::Result<UncertainValue> fine = ruler.measure(0.01234, MeasureContext{1, 0});
    REQUIRE(fine.has_value());

    REQUIRE(fine.value().u < coarse.value().u);
    REQUIRE(fine.value().u == resolution_uncertainty(1.0e-4));
    REQUIRE(ruler.resolution() == 1.0e-4);

    // A resolution finer than the device can report is refused, and nothing changes: a reading finer than the
    // instrument's own limit is a lie it cannot tell, and reporting one would be worse than refusing.
    const auto refused = ruler.set_resolution(1.0e-9);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(ruler.resolution() == 1.0e-4);
    REQUIRE(ruler.measure(0.01234, MeasureContext{1, 0}).value().u ==
            resolution_uncertainty(1.0e-4));
}

TEST_CASE("instrument.a_fixed_precision_device_refuses", "[instrument]") {
    // A device whose precision is fixed by construction says so, rather than pretending to accept a setting it
    // will ignore: a control that appears to work and does nothing is how a student concludes that precision
    // does not matter.
    FixedReading clock;
    REQUIRE_FALSE(clock.describe().adjustable);
    REQUIRE_FALSE(clock.set_resolution(1.0e-6).has_value());
    REQUIRE(clock.resolution() == clock.describe().finest_resolution);

    // The registry can list exactly the devices a "set your instrument" dialog should offer.
    InstrumentRegistry registry;
    Ruler ruler;
    REQUIRE(registry.add(&clock).has_value());
    REQUIRE(registry.add(&ruler).has_value());
    REQUIRE(registry.size() == 2);
    const std::vector<IInstrument*> adjustable = registry.adjustable();
    REQUIRE(adjustable.size() == 1);
    REQUIRE(adjustable.front() == &ruler);
}

TEST_CASE("instrument.the_same_seed_gives_the_same_reading", "[instrument]") {
    // Repeatable. A measurement that cannot be repeated is an anecdote, and the run's record is worthless if the
    // numbers it quotes cannot be produced again. The stub scatters by seed, so this case would fail for any
    // instrument that read the clock.
    Ruler ruler;
    const MeasureContext ctx{20240517, 3};
    const auto first = ruler.measure(0.012345, ctx);
    const auto second = ruler.measure(0.012345, ctx);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first.value().value == second.value().value);
    REQUIRE(first.value().u == second.value().u);

    // A different seed gives a different scattering -- which is what makes the seed worth recording rather
    // than a formality.
    const auto other = ruler.measure(0.012345, MeasureContext{20240518, 3});
    REQUIRE(other.has_value());
    REQUIRE(other.value().value != first.value().value);

    // A non-finite truth is refused rather than turned into a measurement: a diverged model must not appear
    // in the record as a value with an error bar, because an error bar is a claim about precision.
    REQUIRE_FALSE(ruler.measure(std::numeric_limits<double>::infinity(), ctx).has_value());
    REQUIRE_FALSE(ruler.measure(std::numeric_limits<double>::quiet_NaN(), ctx).has_value());
}

TEST_CASE("instrument.a_non_finite_truth_is_not_a_measurement", "[instrument]") {
    // The same rule from the framework's side: `MeasureRefusal::value_not_finite` names it so a report can say
    // *why* the reading is missing instead of showing a blank.
    REQUIRE(std::string{to_string(MeasureRefusal::value_not_finite)} == "value_not_finite");
    REQUIRE(std::string{to_string(MeasureRefusal::fixed_precision)} == "fixed_precision");
    REQUIRE(std::string{to_string(MeasureRefusal::resolution_out_of_range)} ==
            "resolution_out_of_range");
    REQUIRE(std::string{to_string(MeasureRefusal::ok)} == "ok");
    STATIC_REQUIRE(static_cast<std::uint8_t>(MeasureRefusal::ok) == 0);
    REQUIRE(std::string{to_string(static_cast<MeasureRefusal>(200))} == "unknown");
}

TEST_CASE("instrument.registry.one_id_one_instrument", "[instrument]") {
    // An id appears at most once. Two devices under one id would make "what measured this" depend on
    // registration order -- the same class of defect as a record that cannot be looked up -- and a file is
    // evidence.
    InstrumentRegistry registry;
    Ruler first;
    Ruler second;
    REQUIRE(registry.add(&first).has_value());
    REQUIRE(registry.find("test.ruler") == &first);
    REQUIRE(registry.size() == 1);

    // The second one shares the id, so it is refused and the first is untouched.
    REQUIRE_FALSE(registry.add(&second).has_value());
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.find("test.ruler") == &first);

    // A null device, or one with no identity, is refused rather than stored: a registry entry nobody can name
    // is worse than none.
    REQUIRE_FALSE(registry.add(nullptr).has_value());

    // Removal is by id, and removing something that is not there is reported rather than ignored.
    REQUIRE(registry.remove("test.ruler").has_value());
    REQUIRE(registry.size() == 0);
    REQUIRE(registry.find("test.ruler") == nullptr);
    REQUIRE_FALSE(registry.remove("test.ruler").has_value());
}

TEST_CASE("instrument.the_host_calls_through_the_fault_barrier", "[instrument]") {
    // Charter C4 at the instrument boundary: an instrument is plugin code, so the host takes readings through
    // the same barrier as an evaluator or an operator. A device that raises is a fault -- caught, attributed
    // and counted -- rather than an exception travelling out through whatever asked for the measurement.
    BrokenInstrument broken;
    plugin::FaultLog faults;

    const auto reading = measure_guarded(broken, 1.0, MeasureContext{1, 0}, broken.describe().id, &faults);
    REQUIRE_FALSE(reading.has_value());
    REQUIRE(reading.error() == diag::ErrorCode::plugin_fault);

    // Attributed to the device's id, because that is what a device list and a run's record name.
    REQUIRE(faults.faults().size() == 1);
    REQUIRE(faults.faults().front().label == "test.broken");
    REQUIRE(faults.faults().front().what == "the instrument has a memory bug");

    // A healthy instrument goes through the same call and simply works: the barrier is not a tax on the
    // working case, it is a net under the broken one.
    Ruler ruler;
    const auto fine = measure_guarded(ruler, 0.5, MeasureContext{4, 0}, ruler.describe().id, &faults);
    REQUIRE(fine.has_value());
    REQUIRE(fine.value().u > 0.0);
    REQUIRE(faults.faults().size() == 1);

    // And a barrier with no log still protects the host -- the counting is optional, the catching is not.
    const auto unlogged = measure_guarded(broken, 1.0, MeasureContext{1, 0}, "anon", nullptr);
    REQUIRE_FALSE(unlogged.has_value());
    REQUIRE(unlogged.error() == diag::ErrorCode::plugin_fault);
}
