/**
 * @file qp/graph/eval.hpp
 * @brief eval 模块的唯一入口：求值调度 + 内容寻址缓存。
 *
 * ## 缓存为什么是"内容寻址"而不是"版本号失效"
 *
 * 版本号失效（图上任何改动 → 清空缓存）在课堂现场不可用：
 * 老师调一个参数，整张图的烘焙结果全被丢掉，每次调节都要重算。
 *
 * 内容寻址的语义是"同样的输入 → 同样的输出"：
 *   - 改一个参数 → 只有下游的键变了，无关分支照旧命中；
 *   - **撤销回上一步 → 之前的键重新出现，立刻命中**。
 *
 * 第二条尤其重要：撤销/重做在课堂演示里是高频操作，
 * 若每次撤销都触发重算，演示节奏就断了。
 *
 * ## 缓存键里必须含世代号（这是旧工程的真实缺陷）
 *
 * 旧工程的缓存键用了 `id(value.lattice)`——**内存地址**。
 * 那不只是"可能碰撞"的问题，更根本的是：地址不是标识。
 * 本项目改为 `NodeId{index, generation}`：删掉槽位 3 的节点再新建，
 * 新节点也占槽位 3 但世代不同，因此**不会命中旧节点的缓存**。
 *
 * ## 大对象走标识，不逐字节哈希
 *
 * 19MB 的场不允许参与键构造。`field_handle` 按其格子描述符混入哈希，
 * "同一格子 = 同一份数据"由 `core/abi` 的发布方保证。
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   求值不修改图与版本号
 * @errors      失败走 Result
 * @complexity  —
 * @nondet      none
 * @frozen      否
 * @tests       graph.eval.hash_is_fnv1a, graph.eval.hash_of_empty_is_seed,
 *              graph.eval.hash_is_deterministic, graph.eval.hash_value_kind_discriminates,
 *              graph.eval.hash_value_f32_f64_differ, graph.eval.hash_field_uses_lattice,
 *              graph.eval.hash_port_number_matters, graph.eval.hash_order_matters,
 *              graph.eval.canonical_text_is_stable,
 *              graph.eval.canonical_text_discriminates,
 *              graph.eval.cache_key_equality, graph.eval.cache_key_generation_matters,
 *              graph.eval.cache_key_param_change_differs,
 *              graph.eval.cache_store_and_fetch, graph.eval.cache_miss_on_first_lookup,
 *              graph.eval.cache_eviction_lru, graph.eval.cache_disabled_when_zero,
 *              graph.eval.cache_clear, graph.eval.cache_stats_are_consistent,
 *              graph.eval.cache_undo_redo_hits,
 *              graph.eval.single_node, graph.eval.chain_propagates,
 *              graph.eval.deterministic_across_runs,
 *              graph.eval.cache_hit_on_second_run,
 *              graph.eval.param_change_invalidates_only_downstream,
 *              graph.eval.unknown_type_fails, graph.eval.missing_param_fails,
 *              graph.eval.bypass_passthrough, graph.eval.diamond_evaluates_once,
 *              graph.eval.is_readonly, graph.eval.result_lookup,
 *              graph.eval.evaluator_error_propagates,
 *              graph.eval.empty_graph_succeeds
 */
#pragma once

#include <qp/graph/eval/cache.hpp>
#include <qp/graph/eval/evaluator.hpp>
#include <qp/graph/eval/value_key.hpp>

namespace qp::graph {

/// @brief eval 模块的版本。缓存键构造或求值语义变更时必须递增。
inline constexpr int kEvalVersion = 1;

}  // namespace qp::graph
