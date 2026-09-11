/**
 * @file qp/graph/validate.hpp
 * @brief validate 模块的唯一入口：加载期校验。
 *
 * ## 这个模块存在的唯一理由
 *
 * 类型与量纲错误若推迟到求值期，症状是"学生算了十分钟，结果荒谬"。
 * 加载期拒绝的代价是"打开实验时弹一个错误框"。
 * 两者对课堂的影响差一个数量级。
 *
 * ## 它包含一件不显然的事：量纲解析
 *
 * 端口描述里的 `same_as_input`（"我的输出量纲跟输入一样"）在端口层
 * **无法判定**——它需要图的上下文。校验层沿拓扑序把它解析成具体量纲，
 * 然后才可能判定一条边的两端是否真的同量纲。
 *
 * 因此本模块不只是"调用 ports 的检查函数"，它还补上了
 * **只有看到整张图才能完成的那一步**。
 *
 * ## 与相邻模块的边界
 *
 * | 不做 | 归谁 |
 * |---|---|
 * | 无环、端口占用等结构性约束 | `core/graph/structure`（连接时就拒绝了） |
 * | 类型兼容的**单点**判定 | `core/ports` |
 * | 求值 | `core/graph/eval` |
 * | 在 UI 上展示问题 | 视图层 |
 *
 * @ownership   pure（只读图）
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   校验不修改图：调用前后图与版本号完全不变
 * @errors      noexcept（问题以 Report 表达）
 * @frozen      否
 * @tests       graph.validate.issue_text, graph.validate.issue_location,
 *              graph.validate.report_ok, graph.validate.report_collects_all,
 *              graph.validate.report_worst_severity,
 *              graph.validate.dimensions_lookup,
 *              graph.validate.dimensions_unknown_for_unset,
 *              graph.validate.resolve_dimension_from_port_type,
 *              graph.validate.resolve_same_as_input_chain,
 *              graph.validate.resolve_unknown_when_input_missing,
 *              graph.validate.resolve_is_deterministic,
 *              graph.validate.resolve_ignores_unknown_types,
 *              graph.validate.resolve_adder_takes_first_connected_input,
 *              graph.validate.check_edge_does_not_modify_graph,
 *              graph.validate.ok_on_consistent_graph,
 *              graph.validate.rejects_unknown_node_type,
 *              graph.validate.rejects_missing_port,
 *              graph.validate.rejects_dimension_mismatch_on_edge,
 *              graph.validate.rejects_type_mismatch_on_edge,
 *              graph.validate.rejects_missing_required_param,
 *              graph.validate.rejects_domain_violation,
 *              graph.validate.collects_all_issues,
 *              graph.validate.is_readonly,
 *              graph.validate.warns_on_any_port,
 *              graph.validate.warns_on_unconnected_output_when_enabled,
 *              graph.validate.deterministic,
 *              graph.validate.allows_unconnected_optional_input,
 *              graph.validate.rejects_dangling_edge_to_deleted_node,
 *              graph.validate.check_edge_ok, graph.validate.check_edge_rejects_mismatch,
 *              graph.validate.check_edge_rejects_unknown_ports
 */
#pragma once

#include <qp/graph/validate/dimensions.hpp>
#include <qp/graph/validate/report.hpp>
#include <qp/graph/validate/validate.hpp>

namespace qp::graph {

/// @brief validate 模块的版本。Issue/Severity 或校验规则集变更时递增。
inline constexpr int kValidateVersion = 1;

}  // namespace qp::graph
