/**
 * @file qp/graph/structure.hpp
 * @brief 图结构模块的唯一入口。
 *
 * 这个模块只回答四个问题：
 *   1. 图里有哪些节点与边？
 *   2. 加/删/连/断之后，图变成了什么样？
 *   3. 当前版本号是多少？
 *   4. 这个操作能不能做（会不会成环、会不会撞端口）？
 *
 * 它**不**回答"这个图算出来是什么"（→ `eval`），
 * 也**不**回答"节点该画在哪"（→ 视图层的 `view_layouts`）。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   任何成功变异后图仍无环、每个输入端口至多一条入边
 * @errors      noexcept（失败走 Result）
 * @complexity  —
 * @nondet      none
 * @frozen      否
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

/// @brief structure 模块的版本。Graph 的公开接口变更时递增。
inline constexpr int kStructureVersion = 1;

}  // namespace qp::graph
