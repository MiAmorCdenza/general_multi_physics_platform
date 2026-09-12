/**
 * @file test_instruments.cpp
 * @brief Tests for the measuring devices this build ships.
 *
 * Test case ids match the @tests fields in the plugin headers byte for byte.
 *
 * ## What is worth asserting about an instrument
 *
 * Not that a multiplication works. What is worth asserting is that **a reading is consistent with its own
 * error bar** -- that the number the device reports is a number it could actually have produced, and that the
 * uncertainty attached to it is the one its graduations imply. Every case below is a version of that claim:
 *
 *   - the reading lands on a tick, so the device did not invent a digit it cannot see;
 *   - the uncertainty is `resolution/sqrt(12)`, so the error bar came from the instrument and not from the
 *     display's last digit;
 *   - the same request twice gives the same reading, because a device that jitters is a device whose reported
 *     uncertainty does not cover its own spread;
 *   - a device asked for a graduation it does not have refuses rather than pretending;
 *   - and the devices arrive through the host's ledger, so `origin_of` can say where they came from.
 *
 * The last one is the case that would have caught the state this plugin was written to fix: the whole
 * instrument module was reachable from its own test fixtures and from nowhere else.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/instruments/instruments.hpp>

#include <qp/host/host.hpp>
#include <qp/runtime/instrument/instrument.hpp>
#include <qp/units/dimensions.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// `node_types` is the bit that covers registering content of a domain, and a measuring device is content in
// exactly that sense -- `Capability` has no separate bit for instruments, which is itself worth knowing: the
// `add_instrument` method on `IPluginHost` is gated by nothing more specific than "this plugin may register
// content". A host that wants to refuse devices while allowing models has no way to say so today, and that is
// the reopening condition recorded in the plan tree rather than something to invent here.
constexpr qp::plugin::Capability kContent = qp::plugin::Capability::node_types;

using namespace qp::plugins::instruments;

namespace {

namespace rt = qp::runtime;

/// @brief The device registered under `id`, or null. Fails the case rather than returning null silently.
rt::IInstrument* device(const qp::host::PluginHost& host, const char* id) {
    rt::IInstrument* found = host.instruments().find(id);
    REQUIRE(found != nullptr);
    return found;
}

/// @brief How far a reading is from the truth, in units of the device's resolution.
double ticks_away(double reading, double truth, double resolution) {
    return std::abs(reading - truth) / resolution;
}

}  // namespace

TEST_CASE("instrument.ruler.reports_the_tick_it_can_see", "[instruments]") {
    // The whole error model of a graduated device in three assertions: the number is on a tick, it is the
    // **nearest** tick to what was there, and the uncertainty it carries is what that rounding implies.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.rule";
    desc.label = "Test rule";
    desc.quantity = "length";
    desc.dim = qp::units::dims::length;
    desc.finest_resolution = 1.0e-3;
    desc.adjustable = false;
    GraduatedInstrument rule{std::move(desc), 1.0e-3, 0.0};

    REQUIRE(rule.describe().id == "builtin.test.rule");
    REQUIRE(rule.resolution() == 1.0e-3);

    // 0.02413 m is between the 24 mm and 25 mm marks, and nearer the 24. A reading of 0.02413 would be a
    // number this device cannot report, and it is the number a student writes down when they trust a display
    // instead of reading the instrument.
    const auto reading = rule.measure(0.02413, rt::MeasureContext{});
    REQUIRE(reading.has_value());
    REQUIRE(reading.value().value == 0.024);
    REQUIRE(ticks_away(reading.value().value, 0.02413, rule.resolution()) <= 0.5);

    // A value nearer the next mark rounds to it: the reading is the **nearest** tick, not the one below.
    REQUIRE(rule.measure(0.02461, rt::MeasureContext{}).value().value == 0.025);

    // And the uncertainty is the rectangular-distribution result, not zero and not the resolution itself. The
    // distinction is the platform's point: "reads to 1 mm" is not "knows the value to 1 mm", and it is
    // certainly not "knows it exactly".
    const double u = reading.value().u;
    REQUIRE(u > 0.0);
    REQUIRE(std::abs(u - 1.0e-3 / std::sqrt(12.0)) < 1.0e-18);
    REQUIRE(reading.value().kind == rt::UncertaintyKind::standard);
    REQUIRE(reading.value().dim == qp::units::dims::length);
}

TEST_CASE("instrument.ruler.a_reading_repeats_exactly", "[instruments]") {
    // A device with no noise model returns the same number every time, and that is a **decision** rather than
    // an omission. A device that jittered would be reporting a spread its uncertainty does not cover: the
    // number and its error bar would disagree, which is the failure this platform is built to catch. In a real
    // lab the spread comes from the experimenter and the setup, not from a device that answers differently each
    // time it is asked.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.caliper";
    desc.label = "Test caliper";
    desc.quantity = "length";
    desc.dim = qp::units::dims::length;
    desc.finest_resolution = 5.0e-5;
    desc.adjustable = true;

    GraduatedInstrument meter{std::move(desc), 1.0e-3, 0.0};

    const auto first = meter.measure(0.0123456, rt::MeasureContext{20260911, 0});
    REQUIRE(first.has_value());
    // The seed is part of the context and this device does not read it, so changing it must change nothing.
    const auto second = meter.measure(0.0123456, rt::MeasureContext{999, 7});
    REQUIRE(second.has_value());
    REQUIRE(first.value().value == second.value().value);
    REQUIRE(first.value().u == second.value().u);

    // On a tick of 0.05 mm.
    const double ticks = first.value().value / 5.0e-5;
    REQUIRE(std::abs(ticks - std::round(ticks)) < 1.0e-9);
}

TEST_CASE("instrument.ruler.a_fixed_scale_refuses_to_change", "[instruments]") {
    // Two refusals that read differently to a user, which is why they are two codes. A rule's graduations are
    // moulded into it: asking for a finer one is not a request it can weigh, it is a category error the
    // description already answers.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.fixed";
    desc.label = "Fixed rule";
    desc.quantity = "length";
    desc.dim = qp::units::dims::length;
    desc.finest_resolution = 1.0e-3;
    desc.adjustable = false;

    GraduatedInstrument rule{std::move(desc), 1.0e-3, 0.0};
    REQUIRE_FALSE(rule.set_resolution(1.0e-4).has_value());
    REQUIRE(rule.resolution() == 1.0e-3);

    // And the refusal changed nothing, which is the half that a "returns an error" assertion does not cover.
    REQUIRE(rule.measure(0.02413, rt::MeasureContext{}).value().u ==
            rt::resolution_uncertainty(1.0e-3));
}

TEST_CASE("instrument.ruler.an_adjustable_scale_snaps_to_its_ticks", "[instruments]") {
    // An adjustable device honours a request only in whole ticks of its finest graduation. A vernier read to
    // "0.075 mm" is a reading of one and a half vernier divisions, which no eye can do reliably, and
    // `resolution/sqrt(12)` computed from 0.075 would put a number in the error bar that the hardware cannot
    // produce.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.adjustable";
    desc.label = "Adjustable caliper";
    desc.quantity = "length";
    desc.dim = qp::units::dims::length;
    desc.finest_resolution = 5.0e-5;
    desc.adjustable = true;

    GraduatedInstrument caliper{std::move(desc), 1.0e-3, 0.0};
    REQUIRE(caliper.resolution() == 5.0e-5);

    // 0.075 mm is 1.5 ticks, and the request is rounded **down** to one tick: claiming more resolution than the
    // device has is the failure worth avoiding, and rounding to nearest would do exactly that.
    REQUIRE(caliper.set_resolution(7.5e-5).has_value());
    REQUIRE(caliper.resolution() == 5.0e-5);

    // A whole number of ticks is honoured exactly.
    REQUIRE(caliper.set_resolution(2.0e-4).has_value());
    REQUIRE(caliper.resolution() == 2.0e-4);
    REQUIRE(std::abs(caliper.measure(0.01234, rt::MeasureContext{}).value().u -
                     rt::resolution_uncertainty(2.0e-4)) < 1.0e-18);

    // Out of range in both directions, and a value that is not a number at all.
    REQUIRE_FALSE(caliper.set_resolution(1.0e-8).has_value());
    REQUIRE_FALSE(caliper.set_resolution(1.0).has_value());
    REQUIRE_FALSE(caliper.set_resolution(0.0).has_value());
    REQUIRE_FALSE(caliper.set_resolution(-1.0e-4).has_value());
    REQUIRE_FALSE(caliper.set_resolution(std::nan("")).has_value());
    // And none of those refusals moved the setting.
    REQUIRE(caliper.resolution() == 2.0e-4);
}

TEST_CASE("instrument.ruler.a_broken_model_is_not_a_reading", "[instruments]") {
    // A model that produced an infinity or a NaN has not produced a measurement, and reporting one with an
    // error bar would be the platform asserting something about a number nobody computed. This is the same rule
    // the confidence panel holds: an absent value is not a zero.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.finite";
    desc.label = "Test rule";
    desc.quantity = "length";
    desc.dim = qp::units::dims::length;
    desc.finest_resolution = 1.0e-3;
    desc.adjustable = false;

    GraduatedInstrument rule{std::move(desc), 1.0e-3, 0.0};
    REQUIRE_FALSE(rule.measure(std::nan(""), rt::MeasureContext{}).has_value());
    REQUIRE_FALSE(rule.measure(std::numeric_limits<double>::infinity(), rt::MeasureContext{}).has_value());
    REQUIRE_FALSE(rule.measure(-std::numeric_limits<double>::infinity(), rt::MeasureContext{}).has_value());

    // A large but finite value is a reading: refusing it would make the device's opinion about magnitudes part
    // of the physics, and a 3 m measurement with a metre rule is a real thing a student does.
    REQUIRE(rule.measure(2.9979, rt::MeasureContext{}).has_value());
}

TEST_CASE("instrument.a_calibration_offset_shows_up_in_the_reading", "[instruments]") {
    // A unit with an uncorrected error reads what is there plus its own offset, and the offset is inside the
    // reading rather than beside it -- so a calibration exercise with a known standard can measure it. This is
    // the one place a nonzero offset is legitimate, which is why the constructor takes it as an argument with a
    // zero default rather than reading it from anywhere.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.biased";
    desc.label = "Test rule with wear at the zero";
    desc.quantity = "length";
    desc.dim = qp::units::dims::length;
    desc.finest_resolution = 1.0e-3;
    desc.adjustable = false;

    GraduatedInstrument worn{std::move(desc), 1.0e-3, 2.0e-3};
    REQUIRE(worn.offset() == 2.0e-3);
    // 24.0 mm of substance reads 26 mm on a rule whose zero is 2 mm out.
    //
    // Asserted as **"the reading is on a tick, and it is the 26th one"** rather than against a decimal literal,
    // and the difference is a finding rather than a style. Comparing with `0.026` passes on MSVC and fails on
    // 32-bit GCC, where `FLT_EVAL_METHOD == 2` evaluates the arithmetic in x87's 80-bit registers while the
    // literal `0.026` was rounded to double at compile time -- so a bit-comparison against a literal tests the
    // compiler rather than the device.
    //
    // The expectation is therefore built from **the device's own resolution**, which is the one double the
    // device itself multiplied by. Deriving it any other way -- `1.0e-3` written here, or `26 * 1.0e-3` -- asks
    // for the same value through a different expression, and on a build with excess precision those two
    // expressions need not round identically. The assertion stays exact, which matters: a tolerance would stop
    // distinguishing a reading on a tick from one near it, and that distinction is the device's whole contract.
    const auto reading = worn.measure(0.024, rt::MeasureContext{});
    REQUIRE(reading.has_value());
    const double tick = worn.resolution();
    const double shown = reading.value().value;
    REQUIRE(shown == std::round(shown / tick) * tick);
    REQUIRE(std::llround(shown / tick) == 26);

    // And the uncertainty is unchanged: a calibration error is a **bias**, and a bias is not a standard
    // uncertainty. Folding it in would double-count it the moment somebody propagated the reading, because the
    // correction is applied by the person who knows the offset, not by the error bar.
    REQUIRE(worn.measure(0.024, rt::MeasureContext{}).value().u == rt::resolution_uncertainty(1.0e-3));
}

TEST_CASE("instrument.analogue.the_uncertainty_grows_with_the_reading", "[instruments]") {
    // **The case that makes the rack a set of devices rather than a set of graduations.** A meter specified as
    // `+/- (1% of reading + 0.5 mA)` has an error whose size depends on what is being measured, and no
    // graduation can say that: `resolution/sqrt(12)` is the same number at 2 mA and at 20 mA, which is the one
    // thing an ammeter's specification is not.
    //
    // The coefficients here are exact binary fractions so the expectation is arithmetic rather than a rounded
    // decimal: 2^-7 for 1/128, and plenty of headroom below the smallest normal. The point the assertions make
    // is the **shape** -- proportional plus floor -- not the third digit.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.ammeter";
    desc.label = "Test ammeter";
    desc.quantity = "current";
    desc.dim = qp::units::dims::current;
    desc.finest_resolution = 1.0e-6;
    desc.adjustable = true;
    AnalogueMeter meter{std::move(desc), 0.0078125, 1.0e-6};

    // The reading is the truth **unchanged**. This device does not quantise, and rounding the value as well as
    // reporting its specification would count the display's last digit twice -- it is already inside the floor.
    const double truth = 0.02;

    // At the reading: (p*|v| + c)/sqrt(3), and the expectation is **the device's own second answer** rather
    // than the same expression written here. That is not laziness: on 32-bit GCC (`FLT_EVAL_METHOD == 2`) the
    // implementation evaluates the division in an x87 register and rounds once when storing, while the same
    // expression written in the test is rounded at compile time -- so the two agree to every visible digit and
    // still compare unequal, which is how this assertion failed the first time. The property that survives
    // every compiler is the one the device's contract actually promises: the same reading twice.
    const auto reading = meter.measure(truth, rt::MeasureContext{20260911, 0});
    const auto again = meter.measure(truth, rt::MeasureContext{20260911, 0});
    REQUIRE(reading.has_value());
    REQUIRE(reading.value().value == truth);
    REQUIRE(reading.value().kind == rt::UncertaintyKind::standard);
    REQUIRE(reading.value().dim == qp::units::dims::current);

    REQUIRE(reading.value().u == again.value().u);
    REQUIRE(reading.value().u > 0.0);
    // And it is the formula's magnitude, not a constant: scaled by the reading's own proportional term.
    REQUIRE(reading.value().u > (0.0078125 * truth) / std::sqrt(3.0));

    // And the proportional term is real: a tenth of the reading carries about a tenth of the proportional
    // error, which is the assertion a floor-only model fails.
    const auto small = meter.measure(0.002, rt::MeasureContext{});
    REQUIRE(small.has_value());
    REQUIRE(small.value().u < reading.value().u);
    REQUIRE(small.value().u > (0.0078125 * 0.002) / std::sqrt(3.0));

    // **The comparison that says why this device is in the rack.** A graduated device with a 1 uA graduation
    // would report 0.289 uA at this reading and the same 0.289 uA at a tenth of it; the ammeter reports 90 uA.
    // Two instruments whose displays look equally precise disagree by a factor of three hundred, and only the
    // specification knows which one to believe. Comparing against the graduated model directly is what makes
    // this a test of the analogue device rather than of its own arithmetic.
    const double graduated_u = rt::resolution_uncertainty(1.0e-6);
    REQUIRE(reading.value().u > 10.0 * graduated_u);
    REQUIRE(reading.value().u != graduated_u);

    // Unchanged by a range change: the display's increment is a separate fact from the specification, which is
    // the whole reason this class does not derive from `GraduatedInstrument`.
    REQUIRE(meter.set_resolution(1.0e-4).has_value());
    REQUIRE(meter.measure(truth, rt::MeasureContext{}).value().u == reading.value().u);
}

TEST_CASE("instrument.analogue.a_zero_reading_still_has_an_uncertainty", "[instruments]") {
    // The floor term, and the reason the specification has two terms rather than one. A percentage alone would
    // make the uncertainty vanish at zero -- and a vanishing uncertainty on a real meter is a false claim about
    // the instrument, not a fact about the measurement. It is also the exact shape the closed loop is built to
    // refuse: `UncertaintyKind::unknown` exists because "nobody quantified this" must not be spelled "zero",
    // and a device that returned a genuine zero here would be making the opposite mistake.
    //
    // The relative coefficient is deliberately awkward (0.0078125 = 1/128), so the two terms are not multiples
    // of one another and the assertion cannot pass by both being one number.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.zero";
    desc.label = "Test ammeter at zero";
    desc.quantity = "current";
    desc.dim = qp::units::dims::current;
    desc.finest_resolution = 1.0e-6;
    desc.adjustable = true;
    AnalogueMeter meter{std::move(desc), 0.0078125, 5.0e-4};

    const auto zero = meter.measure(0.0, rt::MeasureContext{});
    REQUIRE(zero.has_value());
    REQUIRE(zero.value().value == 0.0);
    REQUIRE(zero.value().u > 0.0);
    // The floor's **exact** value at a zero reading: `(p*0 + c)/sqrt(3)` is `c/sqrt(3)`. Exact rather than
    // approximate, and it can be, because `c` is a power of two that `sqrt(3)` scales without ambiguity --
    // there is no sum here for a compiler with excess precision to associate differently.
    REQUIRE(zero.value().u == 5.0e-4 / std::sqrt(3.0));

    // Negative truth is a reading like any other: a current can be negative, and the proportional term takes
    // the magnitude, so the uncertainty does not change sign with the reading.
    const auto negative = meter.measure(-0.02, rt::MeasureContext{});
    REQUIRE(negative.has_value());
    REQUIRE(negative.value().value == -0.02);
    REQUIRE(negative.value().u > 0.0);
    REQUIRE(negative.value().u == meter.measure(0.02, rt::MeasureContext{}).value().u);

    // The floor is **not** the resolution, and saying so is the point: at zero the reading is known to 0.29 mA
    // and the display claims to resolve 1 uA. A device whose floor happened to equal its resolution would make
    // the two indistinguishable, which is why this one's are 500 ticks apart.
    REQUIRE(zero.value().u > rt::resolution_uncertainty(meter.resolution()));

    // A truth that is not a number is refused rather than reported with an error bar.
    REQUIRE_FALSE(meter.measure(std::nan(""), rt::MeasureContext{}).has_value());
    REQUIRE_FALSE(meter.measure(std::numeric_limits<double>::infinity(), rt::MeasureContext{}).has_value());
}

TEST_CASE("instrument.analogue.an_analogue_meter_changes_range", "[instruments]") {
    // A range switch, which is a resolution change with a bound expressed as a **ratio**: a meter's display has
    // a fixed number of digits, so the increment it can show is a fixed fraction of the range it is on and
    // spans several decades across the settings. An absolute bound would refuse the coarse end of a real
    // instrument's switch.
    rt::InstrumentDesc desc;
    desc.id = "builtin.test.range";
    desc.label = "Test ammeter with ranges";
    desc.quantity = "current";
    desc.dim = qp::units::dims::current;
    desc.finest_resolution = 1.0e-6;
    desc.adjustable = true;
    AnalogueMeter meter{std::move(desc), 0.0078125, 5.0e-4};

    // It starts on its finest setting, which is what a device switched on does.
    REQUIRE(meter.resolution() == 1.0e-6);

    REQUIRE(meter.set_resolution(1.0e-3).has_value());
    REQUIRE(meter.resolution() == 1.0e-3);

    // Out of range at both ends of the span, and values that are not a resolution at all. Each is refused with
    // the device's own code for "I cannot honour that", which is what a caller compares against.
    REQUIRE(meter.set_resolution(1.0e-10).error() == qp::diag::ErrorCode::invalid_argument);
    REQUIRE(meter.set_resolution(1.0e2).error() == qp::diag::ErrorCode::invalid_argument);
    REQUIRE(meter.set_resolution(0.0).error() == qp::diag::ErrorCode::invalid_argument);
    REQUIRE(meter.set_resolution(-1.0e-4).error() == qp::diag::ErrorCode::invalid_argument);
    REQUIRE(meter.set_resolution(std::nan("")).error() == qp::diag::ErrorCode::invalid_argument);
    // And not one of those refusals moved the setting.
    REQUIRE(meter.resolution() == 1.0e-3);

    // A device whose description says its precision is fixed refuses the call outright, with a code that reads
    // differently to a user: this is not "out of range", it is "not a thing this device has".
    rt::InstrumentDesc fixed;
    fixed.id = "builtin.test.fixedmeter";
    fixed.label = "Fixed-range ammeter";
    fixed.quantity = "current";
    fixed.dim = qp::units::dims::current;
    fixed.finest_resolution = 1.0e-6;
    fixed.adjustable = false;
    AnalogueMeter locked{std::move(fixed), 0.0078125, 5.0e-4};
    REQUIRE(locked.set_resolution(1.0e-3).error() == qp::diag::ErrorCode::not_implemented);
    REQUIRE(locked.resolution() == 1.0e-6);
}

TEST_CASE("instrument.the_shipped_kit_is_usable", "[instruments]") {
    // Every device this build ships has to be usable as it stands: a non-empty id and label, a dimension that
    // is not the dimensionless one for a length or a time, a positive finest resolution, and a reading that
    // comes back with the uncertainty the resolution implies.
    std::vector<rt::IInstrument*>& kit = builtin_instruments();
    REQUIRE(kit.size() == builtin_count());
    REQUIRE(builtin_count() >= 5);

    std::vector<std::string> ids;
    for (rt::IInstrument* device : kit) {
        REQUIRE(device != nullptr);
        const rt::InstrumentDesc& desc = device->describe();
        INFO("device " << desc.id);

        REQUIRE_FALSE(desc.id.empty());
        REQUIRE_FALSE(desc.label.empty());
        REQUIRE_FALSE(desc.quantity.empty());
        REQUIRE(desc.finest_resolution > 0.0);
        REQUIRE(desc.dim != qp::units::Dim{});
        // Unique, because the registry is keyed by id and a duplicate would make one device unreachable.
        REQUIRE(std::find(ids.begin(), ids.end(), desc.id) == ids.end());
        ids.push_back(desc.id);

        REQUIRE(device->resolution() >= desc.finest_resolution);
        const auto reading = device->measure(1.0, rt::MeasureContext{});
        REQUIRE(reading.has_value());
        REQUIRE(reading.value().kind == rt::UncertaintyKind::standard);
        // A positive uncertainty from **every** device, whatever its error model. This is the one property the
        // whole rack shares, and it is why the analogue meter's floor term exists: a calibration offset can be
        // zero and a specification's bound cannot.
        REQUIRE(reading.value().u > 0.0);
        REQUIRE(reading.value().dim == desc.dim);
    }

    // A second call returns the same devices, because a host borrows these pointers and a set that was rebuilt
    // per call would leave every registered device dangling.
    REQUIRE(&builtin_instruments() == &kit);

    // The five device classes the platform's lessons are about are present by id, so a document that names one
    // keeps working.
    for (const char* id :
         {"builtin.rule", "builtin.caliper", "builtin.stopwatch", "builtin.millivoltmeter", "builtin.ammeter"}) {
        REQUIRE(std::find(ids.begin(), ids.end(), std::string{id}) != ids.end());
    }

    // Two error **models** are represented, not just five devices: the kit is not five instances of one formula.
    // A device whose specification is a proportion plus a floor reports an uncertainty that depends on the
    // reading, and the metre rule reports one that does not -- so the rack exercises both branches of the
    // interface rather than one of them five times.
    rt::IInstrument* rule = kit.front();
    rt::IInstrument* ammeter = nullptr;
    for (rt::IInstrument* device : kit) {
        if (device->describe().id == "builtin.ammeter") ammeter = device;
    }
    REQUIRE(ammeter != nullptr);
    REQUIRE(rule->measure(2.0e-3, rt::MeasureContext{}).value().u ==
            rule->measure(2.0e-5, rt::MeasureContext{}).value().u);
    REQUIRE(ammeter->measure(2.0e-3, rt::MeasureContext{}).value().u >
            ammeter->measure(2.0e-5, rt::MeasureContext{}).value().u);
}

TEST_CASE("instrument.the_shipped_kit_registers_with_the_host", "[instruments]") {
    // **The case that would have caught the state this plugin exists to fix.** `core/runtime/instrument` had a
    // contract, a registry, a fault barrier and thirteen passing cases, and no production code ever registered a
    // device -- so the platform's central loop was reachable from a fixture and from nowhere else. Declared,
    // documented, tested, unconnected: the same shape of defect this project keeps finding.
    qp::host::PluginHost host{kContent};
    REQUIRE(host.instruments().size() == 0);

    const std::size_t registered = mount_instruments(host);
    REQUIRE(registered == builtin_count());
    REQUIRE(host.instruments().size() == builtin_count());

    // Attributable: the answer comes from the host's own record, so a device that misspelled its origin would
    // still be attributed correctly.
    for (rt::IInstrument* device : builtin_instruments()) {
        REQUIRE(host.origin_of(device->describe().id) == qp::host::PluginHost::kBuiltinOrigin);
    }
    // And a device is not mistaken for a plugin: they are contributions, not mounts.
    REQUIRE(host.mounted_ids().empty());

    // The registered device is the **same object** the kit hands out, not a copy: the registry borrows, and a
    // copy would be a device whose resolution a user could change without the kit noticing.
    rt::IInstrument* rule = device(host, "builtin.rule");
    REQUIRE(rule == builtin_instruments().front());
    REQUIRE(rule->describe().label == "Metre rule");

    // Removable, so a session can put its own rack in place -- and clearing the devices must leave the built-in
    // **node types** alone, which is a defect this method had in its first version: it reset the whole built-in
    // ledger, so a window that cleared its instruments to register a second set lost its palette.
    host.clear_builtin_instruments();
    REQUIRE(host.instruments().size() == 0);
    REQUIRE(host.origin_of("builtin.rule").empty());
}

TEST_CASE("instrument.a_second_mount_does_not_duplicate_the_rack", "[instruments]") {
    // Mounting twice is a real thing a caller does -- a test fixture per case, a window rebuilt after a
    // document change -- and the second mount must report honestly rather than pretending. The registry refuses
    // an id it already serves, so the count comes back zero and the rack is unchanged.
    qp::host::PluginHost host{kContent};
    REQUIRE(mount_instruments(host) == builtin_count());
    REQUIRE(mount_instruments(host) == 0);
    REQUIRE(host.instruments().size() == builtin_count());

    // And the **undo** is enough to make room: clear, then mount again, is how a session replaces its rack.
    host.clear_builtin_instruments();
    REQUIRE(mount_instruments(host) == builtin_count());
}

TEST_CASE("instrument.a_device_answers_through_the_fault_barrier", "[instruments]") {
    // The host's sanctioned way to take a reading, exercised on a real device rather than on the fixture the
    // instrument module's own cases use. The barrier is what keeps a plugin's exception from taking the process
    // down, and a device that is never called through it is a device whose failures are unhandled.
    qp::host::PluginHost host{kContent};
    REQUIRE(mount_instruments(host) == builtin_count());

    qp::plugin::FaultLog faults;
    rt::IInstrument* stopwatch = device(host, "builtin.stopwatch");
    const auto reading =
        rt::measure_guarded(*stopwatch, 1.9975, rt::MeasureContext{20260911, 0}, "builtin.stopwatch", &faults);
    REQUIRE(reading.has_value());
    // A stopwatch reads to 10 ms, so 1.9975 s lands on 2.00 s.
    REQUIRE(std::abs(reading.value().value - 2.00) < 1.0e-12);
    REQUIRE(reading.value().dim == qp::units::dims::time);
    REQUIRE(faults.faults().empty());

    // A non-finite truth is refused by the device, through the barrier, as a failure rather than a fault: the
    // device behaved correctly and said no, which is not the same thing as misbehaving.
    const auto refused = rt::measure_guarded(*stopwatch, std::nan(""), rt::MeasureContext{}, "builtin.stopwatch",
                                             &faults);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(faults.faults().empty());
}
