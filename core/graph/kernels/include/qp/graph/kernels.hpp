/**
 * @file qp/graph/kernels.hpp
 * @brief The single entry point of the kernels module.
 *
 * The native operator contract and its registry. No integrator scheme is
 * implemented here: Boris, leapfrog, RK4 and Verlet are plugins.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   This module registers no kernel of its own
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       kernel.registry.register_and_find
 */
#pragma once

#include <qp/graph/kernels/kernel.hpp>
#include <qp/graph/kernels/registry.hpp>

namespace qp::graph::kernels {

/// @brief ABI version of the kernels module. Bump when PlanOp or IBatchAdvancer changes shape.
inline constexpr int kKernelsAbiVersion = 1;

}  // namespace qp::graph::kernels
