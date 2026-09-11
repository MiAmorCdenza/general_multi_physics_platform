/**
 * @file qp/graph/ir.hpp
 * @brief The single entry point of the graph intermediate representation (IR).
 *
 * ## The IR is the plugin API itself
 *
 * The shapes this module defines are exactly **the contract a plugin author faces**. It therefore must be:
 *   - Small enough: too many concepts turn "write a node" into a learning burden;
 *   - Stable enough: a change ripples into every plugin already published;
 *   - **Structure and types** only, with no evaluation, caching, or layout coordinates.
 *
 * ## What it deliberately does not do
 *
 * | not done here | owner |
 * |---|---|
 * | evaluation | `core/graph/eval` |
 * | caching | `core/graph/eval` |
 * | acyclicity validation | `core/graph/structure` |
 * | node coordinates | view layer (`view_layouts`, slotted by view id) |
 * | parameter validation logic | `core/graph/validate` |
 *
 * The last row matters most: **a node's x/y is the output of a layout algorithm, not a property of the graph**.
 * An older project stuffed it into `Graph.auto_layout()`, which welded one layout algorithm into the kernel.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   this module depends on no other submodule inside core/graph
 * @errors      noexcept
 * @complexity  n/a
 * @nondet      none
 * @frozen      yes (the IR shape is frozen; changes need an ADR)
 * @tests       graph.ids.node_default_is_invalid, graph.ids.node_equality,
 *              graph.ids.node_generation_matters, graph.ids.port_ref_default_is_invalid,
 *              graph.ids.port_ref_equality,
 *              graph.desc.port_basic, graph.desc.port_connectable_flag,
 *              graph.desc.port_numeric_bounds, graph.desc.port_choices,
 *              graph.desc.input_view_lookup,
 *              graph.desc.node_basic, graph.desc.node_port_lookup,
 *              graph.desc.node_output_count, graph.desc.node_has_hooks,
 *              graph.node.construction, graph.node.param_lookup,
 *              graph.node.set_param_replaces, graph.node.bypass_flag,
 *              graph.node.user_name_is_separate_from_id,
 *              graph.edge.construction, graph.edge_equality,
 *              graph.edge_direction_invariant
 */
#pragma once

#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/ir/edge.hpp>
#include <qp/graph/ir/ids.hpp>
#include <qp/graph/ir/node.hpp>

namespace qp::graph {

/// @brief Version of the IR. Must be incremented when the `NodeDesc` shape or the ID semantics change.
inline constexpr int kIrVersion = 1;

}  // namespace qp::graph
