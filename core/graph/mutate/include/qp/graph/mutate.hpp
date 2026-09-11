/**
 * @file qp/graph/mutate.hpp
 * @brief The single entry point of the mutate module: the command bus and the undo stack.
 *
 * ## The only reason this module exists
 *
 * A user can edit **the same graph** in the node editor, the YAML text view, or the block view.
 * If every view changed `Graph` directly:
 *   - nothing could validate in one place (the same illegal edit blocked in three places);
 *   - nothing could invalidate in one place (cache and UI refresh each rolling their own);
 *   - the undo stack would live inside one view, so edits made in the YAML view could not be
 *     undone, or the two views would undo independently and make the document jump around.
 *
 * Hence: **one graph, one edit path, one undo stack.**
 *
 * ## Boundaries with neighbouring modules
 *
 * | not here | whose job |
 * |---|---|
 * | graph structure semantics (slots, generations, cycles) | `core/graph/structure` |
 * | type and dimension checking | `core/graph/validate` |
 * | gesture recognition, shortcuts, dragging | the view layer |
 * | serialization | the view layer / IO plugins |
 *
 * The undo stack belongs **here**, not in the view layer -- the piece most easily misplaced.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every structural change must go through CommandBus
 * @errors      noexcept (failures travel as Result)
 * @complexity  —
 * @nondet      none
 * @frozen      no
 * @tests       graph.mutate.undo_set_param, graph.mutate.redo_restores,
 *              graph.mutate.undo_redo_roundtrip, graph.mutate.new_edit_clears_redo,
 *              graph.mutate.merge_consecutive_set_param,
 *              graph.mutate.merge_does_not_cross_labels,
 *              graph.mutate.merge_does_not_cross_nodes,
 *              graph.mutate.apply_add_node, graph.mutate.apply_remove_node,
 *              graph.mutate.apply_set_param, graph.mutate.apply_connect,
 *              graph.mutate.apply_rejects_invalid, graph.mutate.failed_apply_is_noop,
 *              graph.mutate.reserve_id_is_stable_across_undo,
 *              graph.mutate.rejects_apply_while_unstable,
 *              graph.mutate.graph_accessor_is_readonly,
 *              graph.mutate.undo_remove_restores_edges,
 *              graph.mutate.redo_after_undo_keeps_same_id,
 *              graph.mutate.erase_param_undo_restores_value,
 *              graph.mutate.set_name_undo_enforces_uniqueness,
 *              graph.mutate.bypass_undo, graph.mutate.disconnect_undo_restores_edge,
 *              graph.mutate.command_metadata, graph.mutate.undo_stack_labels,
 *              graph.mutate.failed_undo_keeps_history, graph.mutate.long_session_stress
 */
#pragma once

#include <qp/graph/mutate/bus.hpp>
#include <qp/graph/mutate/command.hpp>

namespace qp::graph {

/// @brief Version of the mutate module. Bump when the Command variant set or the API changes.
inline constexpr int kMutateVersion = 1;

}  // namespace qp::graph
