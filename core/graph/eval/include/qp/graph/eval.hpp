/**
 * @file qp/graph/eval.hpp
 * @brief The single entry point of the eval module: evaluation scheduling + content-addressed cache.
 *
 * ## Why the cache is content-addressed and not invalidated by version number
 *
 * Version-number invalidation (any edit -> clear the cache) fails in a live lecture: the
 * teacher turns one knob and every baked result is dropped, so each tweak recomputes everything.
 *
 * Content addressing means "same inputs -> same outputs":
 *   - change one parameter -> only downstream keys change, unrelated branches still hit;
 *   - **undo back to the previous step -> the old keys reappear and hit immediately**.
 *
 * The second point matters most: undo/redo is a high-frequency operation in a lecture,
 * and recomputing on every undo would break the rhythm of the demonstration.
 *
 * ## The cache key must carry the generation number (a real defect of the old project)
 *
 * The old project's cache key used `id(value.lattice)` -- a **memory address**.
 * That is not just a "possible collision": an address is not an identity at all.
 * This project uses `NodeId{index, generation}`: after the node in slot 3 is deleted and a
 * new one takes that slot, its generation differs, so it **cannot hit the old node's cache**.
 *
 * ## Large objects go by identity, not byte-by-byte hashing
 *
 * A 19MB field must not take part in key construction. `field_handle` is hashed by its
 * lattice descriptor, and "same lattice = same data" is guaranteed by the `core/abi` publisher.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Evaluation never modifies the graph or its version numbers
 * @errors      Failures travel as Result
 * @complexity  —
 * @nondet      none
 * @frozen      no
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

/// @brief Version of the eval module. Bump when cache key construction or eval semantics change.
inline constexpr int kEvalVersion = 1;

}  // namespace qp::graph
