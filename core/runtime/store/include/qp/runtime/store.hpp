/**
 * @file qp/runtime/store.hpp
 * @brief The single entry point of the store module.
 *
 * The shape of measured data: a value with its uncertainty, a series of readings,
 * and what a fit produced. The algorithms are plugins.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No regression or propagation algorithm is implemented here
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       store.uncertainty.states, store.dataset.statistics
 */
#pragma once

#include <qp/runtime/store/store.hpp>

namespace qp::runtime {

/// @brief ABI version of the store module. Bump when a structure's field set changes.
inline constexpr int kStoreAbiVersion = 1;

}  // namespace qp::runtime
