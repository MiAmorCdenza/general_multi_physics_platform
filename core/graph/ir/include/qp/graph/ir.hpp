/**
 * @file qp/graph/ir.hpp
 * @brief 图中间表示（IR）的唯一入口。
 *
 * ## IR 是插件 API 本身
 *
 * 这个模块定义的形状就是**插件作者要面对的契约**。因此它必须：
 *   - 足够小：太多概念会让"写一个节点"变成学习负担；
 *   - 足够稳：改动会波及每一个已发布的插件；
 *   - 只描述**结构与类型**，不含求值、缓存、布局坐标。
 *
 * ## 明确不做的事
 *
 * | 不做 | 归谁 |
 * |---|---|
 * | 求值 | `core/graph/eval` |
 * | 缓存 | `core/graph/eval` |
 * | 无环校验 | `core/graph/structure` |
 * | 节点坐标 | 视图层（`view_layouts`，按视图 id 分槽） |
 * | 参数校验逻辑 | `core/graph/validate` |
 *
 * 最后一条尤其重要：**节点的 x/y 是布局算法的输出，不是图的性质**。
 * 旧工程把它塞进了 `Graph.auto_layout()`，导致一种布局算法被焊进内核。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   本模块不依赖 core/graph 内的其他子模块
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      是（IR 形状冻结；改动需 ADR）
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

/// @brief IR 的版本。`NodeDesc` 形状或 ID 语义变更时必须递增。
inline constexpr int kIrVersion = 1;

}  // namespace qp::graph
