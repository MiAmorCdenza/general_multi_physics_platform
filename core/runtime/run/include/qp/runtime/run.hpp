/**
 * @file qp/runtime/run.hpp
 * @brief The single entry point of the run module.
 *
 * The identity of one experiment run: what it was made of, and which of those
 * inputs were actually recorded. Records gaps rather than papering over them.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Nothing in this module executes a graph
 * @errors      noexcept
 * @complexity  --
 * @nondet      only through the system clock, in now_unix_seconds()
 * @frozen      no
 * @tests       run.spec.completeness
 */
#pragma once

#include <qp/runtime/run/run.hpp>

namespace qp::runtime {

/// @brief ABI version of the run module. Bump when RunSpec's field set changes.
inline constexpr int kRunAbiVersion = 1;

}  // namespace qp::runtime
