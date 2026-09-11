/**
 * @file qp/graph/mutate.hpp
 * @brief mutate 模块的唯一入口：命令总线与撤销栈。
 *
 * ## 这个模块存在的唯一理由
 *
 * 用户可以在节点编辑器、YAML 文本视图、积木视图里改**同一张图**。
 * 如果每个视图各自直接改 `Graph`：
 *   - 没有地方能统一校验（同一个非法操作要在三处各挡一次）；
 *   - 没有地方能统一失效（缓存与 UI 刷新各写一套）；
 *   - 撤销栈只能住在某个视图里，于是 YAML 视图的修改无法被撤销，
 *     或者两个视图各撤各的让文档来回跳。
 *
 * 因此：**一个图，一条编辑通道，一个撤销栈。**
 *
 * ## 与相邻模块的边界
 *
 * | 不做 | 归谁 |
 * |---|---|
 * | 图的结构语义（槽位、世代、环） | `core/graph/structure` |
 * | 类型与量纲校验 | `core/graph/validate` |
 * | 手势识别、快捷键、拖拽 | 视图层 |
 * | 序列化 | 视图层 / IO 插件 |
 *
 * 撤销栈**在这里**而不是在视图层——这是本模块最容易被放错的东西。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   任何结构变更都必须经过 CommandBus
 * @errors      noexcept（失败走 Result）
 * @complexity  —
 * @nondet      none
 * @frozen      否
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

/// @brief mutate 模块的版本。Command 变体集合或 API 变更时递增。
inline constexpr int kMutateVersion = 1;

}  // namespace qp::graph
