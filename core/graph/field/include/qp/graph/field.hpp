/**
 * @file qp/graph/field.hpp
 * @brief The single entry point of the field module.
 *
 * The minimal semantics of "a field" -- is this one, what shape is it, which components exist -- plus the store
 * a bake publishes its samples into. Physical models are plugins.
 *
 * @ownership   pure (describes the module; the store it pulls in owns sample buffers)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   No physical model is implemented in this module
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       field.kind_matches_abi
 */
#pragma once

#include <qp/graph/field/field.hpp>
#include <qp/graph/field/field_set.hpp>

namespace qp::graph::field {

/// @brief ABI version of the field module. Bump when FieldValue's layout changes.
inline constexpr int kFieldAbiVersion = 1;

}  // namespace qp::graph::field
