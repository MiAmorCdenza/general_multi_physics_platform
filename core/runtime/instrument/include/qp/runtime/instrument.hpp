/**
 * @file qp/runtime/instrument.hpp
 * @brief The single entry point of the instrument module.
 *
 * A device is asked for one reading of one truth, at the resolution it is set to and with a seed; what it
 * returns is a value with its standard uncertainty and its dimension. Charter C6 is why this contract exists
 * before any device does.
 *
 * @ownership   observes (the registry borrows the devices)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   An instrument cannot produce a value without an uncertainty
 * @errors      Reports through `diag::Result`
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       instrument.a_reading_is_never_a_bare_value
 */
#pragma once

#include <qp/runtime/instrument/instrument.hpp>

namespace qp::runtime {

/// @brief ABI version of the instrument contract. Bump when `IInstrument`'s signatures, `MeasureRefusal`'s
///        tags or `InstrumentDesc`'s meaning change -- a device compiled against the old one would answer a
///        question the host no longer asks.
inline constexpr int kInstrumentAbiVersion = 1;

}  // namespace qp::runtime
