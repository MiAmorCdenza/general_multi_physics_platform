/**
 * @file instruments.hpp
 * @brief The devices this build ships, and the one call that registers them.
 *
 * ## What "content" means here, and why it is a fixed list
 *
 * A lab course's kit is a fixed list: a metre rule, a vernier caliper, a stopwatch, a multimeter. The list is
 * the content, in the same sense that `demo_library()` is content one layer up -- it is what this build ships,
 * not what the framework allows. A course that wants a micrometer ships one the same way, by adding a device
 * here or by loading a plugin that registers its own through `IPluginHost::add_instrument`.
 *
 * ## Why the numbers are what they are
 *
 * Each device's finest increment is the increment a real instrument of that class reads to, and the uncertainty
 * follows from it by `resolution_uncertainty`. The consequences are worth looking at, because they are the
 * lesson the platform is for:
 *
 *   - a **metre rule** reads to 1 mm, so one reading carries 0.29 mm of standard uncertainty no matter how
 *     carefully it is taken. A student who writes "24.1 mm +/- 0.01 mm" has claimed a precision their ruler
 *     does not have;
 *   - a **vernier caliper** reads to 0.05 mm and carries 0.014 mm -- twenty times better, and still not exact;
 *   - a **stopwatch** reads to 10 ms, so a period timed once carries 2.9 ms. Timing ten periods and dividing by
 *     ten is the standard trick, and it works because the uncertainty divides with the reading -- which the
 *     propagation in `plugins/analysis` will do when it exists;
 *   - a **multimeter** on its millivolt range reads to 0.1 mV.
 *
 * Every one of those is a number a student can compare against what they wrote down, which is the point.
 *
 * ## Ownership
 *
 * `builtin_instruments()` returns a reference to a function-local set, so the devices outlive any host that
 * borrows them -- and `InstrumentRegistry` borrows rather than owns, which is why a temporary here would be a
 * dangling pointer the first time somebody took a reading. `mount_instruments` registers each one through the
 * host, so every device is attributed and removable like a plugin's.
 *
 * @ownership   owns the devices it returns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The same devices every call, and `desc().id` is unique across them
 * @errors      See each declaration
 * @frozen      no
 * @tests       instrument.the_shipped_kit_is_usable
 */
#pragma once

#include <qp/host/host.hpp>
#include <qp/plugins/instruments/ruler.hpp>

#include <cstddef>
#include <vector>

namespace qp::plugins::instruments {

/// @brief How many devices this build ships, so a caller can iterate without a sentinel.
///
/// @ownership   pure
/// @thread      main
/// @pre         none
/// @post        `>= 1`
/// @invariant   Constant
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       instrument.the_shipped_kit_is_usable
[[nodiscard]] constexpr std::size_t builtin_count() noexcept { return 4; }

/**
 * @brief The devices this build ships, in the order a device list should show them.
 *
 * Ordered by the resolution they can reach, finest last, which is the order a lab's kit is usually laid out in
 * and the order that makes "which of these is the right tool" answerable by reading down the list.
 *
 * @ownership   borrows from a function-local set that outlives any host
 * @thread      main
 * @pre         none
 * @post        `size() == builtin_count()` and every entry is non-null with a non-empty id
 * @invariant   Stable for the lifetime of the process, so a host may hold these pointers
 * @errors      May allocate on the first call; allocation failure terminates
 * @complexity  O(1) after the first call
 * @nondet      none
 * @frozen      no
 * @tests       instrument.the_shipped_kit_is_usable
 */
[[nodiscard]] std::vector<qp::runtime::IInstrument*>& builtin_instruments();

/**
 * @brief Registers every shipped device with `host` as a built-in.
 *
 * Goes through `PluginHost::add_builtin_instrument` rather than reaching into the registry, for the reason the
 * editor's demonstrator node types go through `add_builtin_node_type`: a contribution that bypassed the record
 * would be the one thing `origin_of` could not answer for, and the one thing `clear_builtin_instruments` could
 * not take back.
 *
 * Returns how many were registered rather than a code, because a **partial** result is meaningful and a single
 * failure is not fatal: four devices where one id collided is a usable instrument rack with a device missing,
 * and the caller can compare against `builtin_count()` to find out. Refusing the whole set because one id was
 * taken would turn a name clash into "this build has no instruments".
 *
 * @param host The host. Borrowed, and it must outlive the devices -- which it does, because they are static.
 *
 * @ownership   observes `host`
 * @thread      main
 * @pre         `host` is not null
 * @post        Every device whose id was free is registered and attributed to `PluginHost::kBuiltinOrigin`
 * @invariant   A device already registered under its id is left alone rather than duplicated
 * @errors      noexcept
 * @complexity  O(devices)
 * @nondet      none
 * @frozen      no
 * @tests       instrument.the_shipped_kit_registers_with_the_host,
 *              instrument.a_second_mount_does_not_duplicate_the_rack,
 *              instrument.a_device_answers_through_the_fault_barrier
 */
[[nodiscard]] std::size_t mount_instruments(qp::host::PluginHost& host) noexcept;

}  // namespace qp::plugins::instruments
