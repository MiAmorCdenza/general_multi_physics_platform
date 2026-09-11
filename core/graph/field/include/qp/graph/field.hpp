/**
 * @file qp/graph/field.hpp
 * @brief The single entry point of the field module.
 *
 * The minimal semantics of "a field": is this one, what shape is it, which
 * components exist. Physical models are plugins.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   This module owns no state and allocates nothing
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       field.kind_matches_abi
 */
#pragma once

#include <qp/graph/field/field.hpp>

namespace qp::graph::field {

/// @brief ABI version of the field module. Bump when FieldValue's layout changes.
inline constexpr int kFieldAbiVersion = 1;

}  // namespace qp::graph::field
