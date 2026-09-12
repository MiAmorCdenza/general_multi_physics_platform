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
 * The increments are the ones an instrument of each class actually reads to, and the uncertainties follow from
 * `resolution / sqrt(12)`:
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
 */
#include <qp/plugins/instruments/instruments.hpp>

#include <qp/host/host.hpp>
#include <qp/units/dimensions.hpp>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace qp::plugins::instruments {
namespace {

namespace rt = qp::runtime;

/// @brief Builds a device from the facts that distinguish one from another.
///
/// A helper rather than a subclass per device, because the four differ only in these values and a struct per
/// device would be four places to get the uncertainty formula wrong. What it does **not** hide is the offset:
/// every device here is calibrated, so `GraduatedInstrument`'s zero default is used and the reason is written at
/// each call site.
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

/**
 * @brief How many devices this build ships, and the rack's size.
 *
 * The number `builtin_count()` must equal, asserted at compile time in this file. It is written here rather than
 * reused from the header because the array's extent has to be a constant expression in this translation unit,
 * and one number with an assertion against it is the shape that **catches** a drift: adding a fifth device
 * without changing either count is a compile error.
 */
constexpr std::size_t kDeviceCount = 4;

/**
 * @brief Builds the rack, each device constructed where it lives.
 *
 * **Filled in place, and that is a correction rather than a style.** The first version built the array inside a
 * lambda and returned it, and the compiler refused with C2280 on `std::array`'s copy constructor: `std::array`'s
 * implicit move is defined member-wise, so an element type with a deleted move makes the whole array unmovable.
 * Aggregate initialisation from prvalues constructs each element directly in its slot -- elision applies **per
 * element initialiser**, which is the part the earlier attempt got wrong -- and a function whose body is a single
 * return of a prvalue of its own return type is elided into the caller's storage, so no device is relocated.
 *
 * @ownership   owns the returned rack
 * @thread      main (called once, when the rack is initialised)
 * @pre         none
 * @post        `kDeviceCount` devices with unique ids, in the order a device list shows them
 * @invariant   The ids are the ones the platform's documents and lessons name
 * @errors      May allocate for the descriptor strings; allocation failure terminates
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       instrument.the_shipped_kit_is_usable
 */
[[nodiscard]] std::array<GraduatedInstrument, kDeviceCount> build_rack() {
    return std::array<GraduatedInstrument, kDeviceCount>{
        // A metre rule. One setting, because the marks are moulded into the plastic: asking a rule for finer
        // graduations is not a request it can weigh, which is `adjustable = false`.
        make("builtin.rule", "Metre rule", "length", qp::units::dims::length, 1.0e-3, 1.0e-3,
             /*adjustable=*/false),
        // A vernier caliper. The vernier scale is read against the main scale, so the finest increment is a
        // twentieth of a millimetre, and a user may read to whole millimetres instead -- a coarser reading that
        // honestly carries a coarser uncertainty.
        make("builtin.caliper", "Vernier caliper", "length", qp::units::dims::length, 5.0e-5, 1.0e-3,
             /*adjustable=*/true),
        // A digital stopwatch. The increment is the display's, which is 10 ms on the usual bench model, and this
        // is the device the platform's "time ten periods and divide by ten" lesson is about.
        make("builtin.stopwatch", "Digital stopwatch", "time", qp::units::dims::time, 1.0e-2, 1.0e-1,
             /*adjustable=*/true),
        // A bench multimeter on its millivolt range.
        make("builtin.millivoltmeter", "Millivoltmeter", "voltage", qp::units::dims::voltage, 1.0e-4, 1.0e-2,
             /*adjustable=*/true),
    };
}

/**
 * @brief The rack: the storage, and the view of it a caller iterates.
 *
 * Two members because a caller wants `IInstrument*` and what exists is `GraduatedInstrument`, and the view has to
 * live **with** the storage rather than be built per call. The first version built the view on every call and
 * returned a reference to it, which compiled, ran, and produced a reference to a destroyed vector:
 * `builtin_instruments().size()` reported four inside one expression and the mount registered nothing while
 * reporting success. That is exactly the failure a "returns a reference" signature is supposed to make
 * impossible, which is why the view is a member here.
 */
struct Rack final {
    /// Not `const`: the registry borrows **non-const** devices, because a user may change a caliper's
    /// resolution. Nothing outside `set_resolution` writes to a device.
    std::array<GraduatedInstrument, kDeviceCount> devices = build_rack();
    std::vector<rt::IInstrument*> view = [](std::array<GraduatedInstrument, kDeviceCount>& storage) {
        std::vector<rt::IInstrument*> out;
        out.reserve(storage.size());
        for (GraduatedInstrument& device : storage) out.push_back(&device);
        return out;
    }(devices);
};

static_assert(builtin_count() == kDeviceCount,
              "builtin_count() and the devices built in instruments.cpp have drifted apart");

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
