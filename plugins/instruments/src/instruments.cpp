/**
 * @file instruments.cpp
 * @brief What this build ships as a **rack**: the devices, the count that goes with them, and the registration.
 *
 * `ruler.cpp` owns the one class every device here is an instance of; this file owns the set -- which devices
 * exist, how many there are, and how they reach the host. Two translation units because they have two reasons to
 * change: a new device is content, and reaching the host is mechanism, and a file that did both would make a
 * caller who only wants to measure with a caliper include the plugin host.
 *
 * ## The numbers, and why they are worth reading as a table
 *
 * The increments are the ones an instrument of each class actually reads to, and the uncertainties of the four
 * graduated devices follow from `resolution / sqrt(12)`:
 *
 *   | device      | increment | standard uncertainty | what a student usually writes |
 *   |-------------|-----------|----------------------|-------------------------------|
 *   | metre rule  | 1 mm      | 0.29 mm              | "0.1 mm"                      |
 *   | vernier     | 0.05 mm   | 0.014 mm             | "0.05 mm"                     |
 *   | stopwatch   | 10 ms     | 2.9 ms               | "1 ms"                        |
 *   | millivolt   | 0.1 mV    | 0.029 mV             | "0.01 mV"                     |
 *
 * The right-hand column is the habit this platform exists to correct, and it is not a small one: it is the
 * difference between an error bar that came from the instrument and one that came from the display's last
 * digit. The middle column is what a repeated measurement's standard error should be compared against.
 *
 * The fifth device is the ammeter, and it gets no row because its uncertainty is not a number: it is
 * `(0.01 * |reading| + 0.5 mA) / sqrt(3)`, so at 20 mA it is 0.40 mA and at 2 mA it is 0.29 mA. A table column
 * would have to pick one reading to be about, which is exactly the point of shipping it -- a specification
 * that reads "+/- (1% + 0.5 mA)" cannot be turned into a single uncertainty for the device.
 */
#include <qp/plugins/instruments/instruments.hpp>

#include <qp/host/host.hpp>
#include <qp/units/dimensions.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace qp::plugins::instruments {
namespace {

namespace rt = qp::runtime;

/// @brief Builds a device from the facts that distinguish one from another.
///
/// A helper rather than a subclass per device, because the graduated devices differ only in these values and a
/// struct per device would be four places to get the uncertainty formula wrong. What it does **not** hide is the
/// offset: every device here is calibrated, so `GraduatedInstrument`'s zero default is used and the reason is
/// written at each call site.
[[nodiscard]] GraduatedInstrument make(const char* id, const char* label, const char* quantity,
                                      qp::units::Dim dim, double finest, double coarsest, bool adjustable) {
    rt::InstrumentDesc desc;
    desc.id = id;
    desc.label = label;
    desc.quantity = quantity;
    desc.dim = dim;
    desc.finest_resolution = finest;
    desc.adjustable = adjustable;
    // `coarsest` is absent from `InstrumentDesc` on purpose: the description records what the device **is** -- its
    // finest increment and whether it can be adjusted -- while the coarsest setting is a range the constructor
    // takes and `set_resolution` enforces. Putting it in the description would mean a wire format and a
    // compatibility rule for a number nothing outside this file reads.
    return GraduatedInstrument{std::move(desc), coarsest, /*offset=*/0.0};
}

/// @brief Builds an analogue meter from its declaration and its two error coefficients.
///
/// A separate helper because an `AnalogueMeter` is not a `GraduatedInstrument` and cannot go through `make`:
/// the two classes share `IInstrument` and nothing else. What they *do* share is the shape of a helper taking
/// the declaration's fields positionally, so the one call site reads like the four above it and a reader
/// comparing them sees the difference in the arguments rather than in the style.
[[nodiscard]] AnalogueMeter meter(const char* id, const char* label, const char* quantity, qp::units::Dim dim,
                                  double finest, double relative, double floor) {
    rt::InstrumentDesc desc;
    desc.id = id;
    desc.label = label;
    desc.quantity = quantity;
    desc.dim = dim;
    desc.finest_resolution = finest;
    // Adjustable, because an analogue meter's range switch is exactly a resolution change: the display keeps
    // its number of digits and the increment follows the range. `set_resolution` bounds the request by a ratio
    // rather than by an absolute limit, which is what a range switch does.
    desc.adjustable = true;
    return AnalogueMeter{std::move(desc), relative, floor};
}

/**
 * @brief How many devices this build ships, and the rack's size.
 *
 * The number `builtin_count()` must equal, asserted at compile time in this file. It is written here rather than
 * reused from the header because the array's extent has to be a constant expression in this translation unit,
 * and one number with an assertion against it is the shape that **catches** a drift: adding a sixth device
 * without changing either count is a compile error.
 *
 * Four of the five entries are `GraduatedInstrument`s and the fifth is an `AnalogueMeter`, so the rack holds
 * **base-class pointers** rather than concrete devices. That is the change the fifth device forced, and it is
 * the honest shape: the rack is a set of `IInstrument`s, and it only looked like a set of graduated devices
 * while every member happened to be one.
 */
constexpr std::size_t kDeviceCount = 5;

/**
 * @brief The rack: the storage, and the view of it a caller iterates.
 *
 * ## Why each device is held by a `unique_ptr`
 *
 * The devices have to outlive the call that makes them, and the two ways to get that are to put them in an
 * object that outlives the call or to allocate them somewhere whose lifetime is not a stack frame. They cannot
 * be values in a container, because a device is not movable -- its address is its identity to the registry, and
 * `InstrumentRegistry` holds non-owning pointers -- so `vector` cannot hold one and neither can `optional`,
 * which was the second attempt: `std::optional<GraduatedInstrument>` needs the move constructor that
 * `GraduatedInstrument` deletes, and MSVC said so from three frames down in `xutility` as
 * `std::construct_at` finding no overload.
 *
 * A `unique_ptr` is the shape that fits: the **pointer** moves and the device never does. It also removes the
 * hazard that the first attempt died of, structurally rather than by care. That version built five named
 * locals in a `build_rack()` function and returned `{&rule, &caliper, ...}`: the array was returned by value,
 * the locals died at the closing brace, and the rack was five dangling pointers -- which the test suite met
 * with a `SIGSEGV` in the very test whose name says the shipped kit is usable, once a later call's vector had
 * overwritten the stack the devices had been on. Here the builder **cannot** return a pointer to something it
 * owns by value: it allocates, and the only question left is whether the `unique_ptr` survives, which is what
 * the shelf's lifetime answers.
 *
 * The view has to live **with** the storage rather than be built per call, and that is the defect that started
 * this comment: the first version built the view on every call and returned a reference to it, so
 * `builtin_instruments().size()` reported four inside one expression and `mount_instruments` registered nothing
 * while reporting success.
 *
 * The devices are **not** `const`: the registry borrows non-const devices, because a user may change a caliper's
 * resolution or an ammeter's range. Nothing outside `set_resolution` writes to one.
 */
struct DeviceShelf final {
    /// One owner per device. Declaration order is the shelf's order, and `view` is built from it.
    std::unique_ptr<GraduatedInstrument> rule;
    std::unique_ptr<GraduatedInstrument> caliper;
    std::unique_ptr<GraduatedInstrument> stopwatch;
    std::unique_ptr<GraduatedInstrument> millivoltmeter;
    std::unique_ptr<AnalogueMeter> ammeter;
};

/**
 * @brief Fills the shelf and returns the devices in the order a device list should show them.
 *
 * @ownership   owns the return value; what it holds belongs to `shelf`, which must outlive it
 * @thread      main
 * @pre         `shelf` is empty, and outlives the returned vector and everything it points at
 * @post        `shelf` holds `kDeviceCount` devices with unique ids, finest-reading last
 * @invariant   The ids are the ones the platform's documents and lessons name
 * @errors      May allocate for the descriptor strings and the shelf; allocation failure terminates
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       instrument.the_shipped_kit_is_usable
 *
 * `new` rather than `std::make_unique`, and that is not a style choice: `make_unique<T>(args...)` forwards its
 * arguments into a `T` it constructs by value, so it needs `T`'s move constructor, and `GraduatedInstrument` and
 * `AnalogueMeter` both declare theirs `= delete` on purpose -- a device's address is its identity. MSVC's first
 * error for the `make_unique` version pointed three frames down inside `memory`, which is where a rule enforced
 * by a deleted constructor shows up when a helper assumes it away.
 */
[[nodiscard]] std::vector<rt::IInstrument*> build_rack(DeviceShelf& shelf) {
    // A metre rule. One setting, because the marks are moulded into the plastic: asking a rule for finer
    // graduations is not a request it can weigh, which is `adjustable = false`.
    shelf.rule = std::unique_ptr<GraduatedInstrument>(new GraduatedInstrument(
        make("builtin.rule", "Metre rule", "length", qp::units::dims::length, 1.0e-3, 1.0e-3,
             /*adjustable=*/false)));

    // A vernier caliper. The vernier scale is read against the main scale, so the finest increment is a
    // twentieth of a millimetre, and a user may read to whole millimetres instead -- a coarser reading that
    // honestly carries a coarser uncertainty.
    shelf.caliper = std::unique_ptr<GraduatedInstrument>(new GraduatedInstrument(
        make("builtin.caliper", "Vernier caliper", "length", qp::units::dims::length, 5.0e-5, 1.0e-3,
             /*adjustable=*/true)));

    // A digital stopwatch. The increment is the display's, which is 10 ms on the usual bench model, and this
    // is the device the platform's "time ten periods and divide by ten" lesson is about.
    shelf.stopwatch = std::unique_ptr<GraduatedInstrument>(new GraduatedInstrument(
        make("builtin.stopwatch", "Digital stopwatch", "time", qp::units::dims::time, 1.0e-2, 1.0e-1,
             /*adjustable=*/true)));

    // A bench multimeter on its millivolt range.
    shelf.millivoltmeter = std::unique_ptr<GraduatedInstrument>(new GraduatedInstrument(
        make("builtin.millivoltmeter", "Millivoltmeter", "voltage", qp::units::dims::voltage, 1.0e-4, 1.0e-2,
             /*adjustable=*/true)));

    // A bench ammeter, and the one device here whose error is **not** a graduation. The specification is
    // "+/- (1% of reading + 0.5 mA)", which is not a tick size: it is a proportional term plus a floor, and
    // neither of them is a resolution. The finest resolution the description names is the display's finest
    // digit on the most sensitive range, which is a separate fact from the error -- that separation is the
    // whole reason `AnalogueMeter` exists (see `analogue_meter.hpp`).
    shelf.ammeter = std::unique_ptr<AnalogueMeter>(new AnalogueMeter(
        meter("builtin.ammeter", "Bench ammeter", "current", qp::units::dims::current, 1.0e-6, 0.01, 5.0e-4)));

    return {shelf.rule.get(), shelf.caliper.get(), shelf.stopwatch.get(), shelf.millivoltmeter.get(),
            shelf.ammeter.get()};
}

/// @brief The one rack this build ships: the shelf, and the view of it a caller iterates.
struct Rack final {
    /// The storage. Declared first because `view` names what is in it.
    DeviceShelf shelf;

    /// What a caller iterates. The same vector on every call, which is what "returns a reference" has to mean
    /// for a caller that holds it across calls.
    std::vector<rt::IInstrument*> view = build_rack(shelf);

    /// The size the compile-time assert below is about: a fifth device must appear in the array **and** in
    /// `builtin_count()`, or the build fails.
    static_assert(builtin_count() == kDeviceCount,
                  "builtin_count() and the devices built in instruments.cpp have drifted apart");
};

}  // namespace

std::vector<rt::IInstrument*>& builtin_instruments() {
    // A function-local rack rather than a namespace-scope one: this is initialised on first use, so it cannot be
    // built before `main` for a translation unit that merely names the type, and both the devices and the view
    // therefore outlive every host that borrows from them. The **same** view comes back every call, which is what
    // "returns a reference" has to mean for a caller that holds it across calls.
    static Rack rack;
    return rack.view;
}

std::size_t mount_instruments(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (rt::IInstrument* device : builtin_instruments()) {
        // A refusal is counted rather than fatal: a device whose id is already taken is one device missing from
        // the rack, which the caller finds by comparing against `builtin_count()`. Refusing the whole set would
        // turn a name clash into "this build has no instruments" -- a much larger claim than the evidence.
        if (host.add_builtin_instrument(device) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

}  // namespace qp::plugins::instruments
