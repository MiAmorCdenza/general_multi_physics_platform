/**
 * @file qp/runtime/trace.hpp
 * @brief The single entry point of the trace module.
 *
 * The timeline: the platform's record of what happened, and when. Charter C3 makes
 * it a platform mechanism rather than a view's private state, so that a saved run
 * has a timeline and two views cannot disagree about one.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Nothing in this module resamples, smooths or interpolates
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       trace.trace.append_is_ordered
 */
#pragma once

#include <qp/runtime/trace/trace.hpp>

namespace qp::runtime {

/// @brief ABI version of the trace module. Bump when Sample's or Channel's field
///        set changes.
inline constexpr int kTraceAbiVersion = 1;

}  // namespace qp::runtime
