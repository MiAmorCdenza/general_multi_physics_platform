/**
 * @file qp/graph/structure.hpp
 * @brief The single entry point of the graph structure module.
 *
 * This module answers exactly four questions:
 *   1. Which nodes and edges does the graph hold?
 *   2. After add/remove/connect/disconnect, what does the graph look like?
 *   3. What is the current version number?
 *   4. May this operation happen (would it create a cycle or clash on a port)?
 *
 * It does **not** answer "what does this graph compute" (-> `eval`),
 * and it does **not** answer "where should a node be drawn" (-> the view layer's `view_layouts`).
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   After any successful mutation the graph is still acyclic, and each input port has at most one incoming edge
 * @errors      noexcept (failures travel as Result)
 * @complexity  —
 * @nondet      none
 * @frozen      no
 * @tests       graph.structure.empty_graph, graph.structure.add_node,
 *              graph.structure.add_node_uses_fresh_generation,
 *              graph.structure.remove_node_invalidates_handle,
 *              graph.structure.remove_node_drops_edges,
 *              graph.structure.connect_and_lookup,
 *              graph.structure.connect_rejects_duplicate_input,
 *              graph.structure.connect_rejects_unknown_node,
 *              graph.structure.connect_rejects_direction,
 *              graph.structure.connect_rejects_cycle,
 *              graph.structure.connect_rejects_self_loop,
 *              graph.structure.disconnect_removes_edge,
 *              graph.structure.version_bumps_on_success_only,
 *              graph.structure.failed_mutation_is_noop,
 *              graph.structure.node_count_tracks_slots,
 *              graph.structure.incoming_lookup_by_input,
 *              graph.structure.find_node_by_user_name,
              graph.structure.clear_resets,
 *              graph.structure.deterministic_iteration_order,
 *              graph.structure.no_cycle_after_many_connects
 */
#pragma once

#include <qp/graph/structure/graph.hpp>

namespace qp::graph {

/// @brief Version of the structure module. Bump when Graph's public interface changes.
inline constexpr int kStructureVersion = 1;

}  // namespace qp::graph
