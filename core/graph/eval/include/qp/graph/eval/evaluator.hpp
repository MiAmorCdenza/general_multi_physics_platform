/**
 * @file evaluator.hpp
 * @brief Evaluation: pull-based scheduling + content-addressed cache.
 *
 * ## Why pull-based rather than push-based
 *
 * Push-based evaluation (walk down the edges from the source nodes) computes the **whole
 * graph**, including branches the current view never looks at. Usually only a few nodes in a
 * graph are "declared outputs", and a pull-based walk evaluates only the subgraph reaching them.
 *
 * In a lecture: the teacher turns one knob and only the branches feeding the current view recompute.
 *
 * ## Evaluation must be deterministic
 *
 * The same graph with the same parameters must give **bit-identical** results on two runs
 * (the spirit of charter R2). Hence:
 *   - the topological order breaks ties by slot index when several orders are possible (fixed order);
 *   - the cache key holds no address, time, or random number;
 *   - node implementations are required to be pure functions (the `INodeEvaluator` contract).
 *
 * ## Single-threaded
 *
 * Evaluation runs on the **main thread**. Multi-threaded evaluation comes later (it needs a
 * per-node context); we do not do it now -- concurrency is left until there is a real
 * performance need, instead of importing complexity we cannot verify up front.
 *
 * @ownership   owns (cache and statistics)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Evaluation never modifies the graph or its version numbers
 * @errors      Failures travel as Result, nothing is thrown
 * @frozen      no
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/eval/cache.hpp>
#include <qp/graph/ir.hpp>
#include <qp/plugin/guard.hpp>
#include <qp/graph/structure.hpp>
#include <qp/ports.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace qp::graph {

using qp::diag::Result;

/**
 * @brief The interface of a node implementation.
 *
 * Contract (the agreement with implementers of `INodeEvaluator`):
 *   - **Pure function**: same inputs, same outputs; no reliance on time, randomness, or globals.
 *   - **Must not modify its inputs**.
 *   - **Must not touch the graph**: it sees only its own input values and parameters.
 *   - **Must not throw** (no exceptions on the hot path; failures are expressed with `Result`).
 *
 * These constraints are not "advice": the existence of the cache **requires** purity --
 * a node that depends on hidden state will hit the wrong cache entry.
 */
class INodeEvaluator {
public:
    INodeEvaluator() = default;
    virtual ~INodeEvaluator() = default;
    INodeEvaluator(const INodeEvaluator&) = delete;
    INodeEvaluator& operator=(const INodeEvaluator&) = delete;

    /**
     * @brief Computes the outputs of one node.
     *
     * @ownership   pure (must not retain references to inputs)
     * @thread      main
     * @pre         The node type matches desc
     * @post        Returns the values of all of the node's output ports
     * @invariant   Same inputs always give the same outputs
     * @errors      Result; on failure it returns an error code and does not throw
     * @complexity  Determined by the implementation
     * @nondet      **must be none** -- otherwise the cache returns wrong results
     * @frozen      yes
     */
    [[nodiscard]] virtual Result<std::vector<std::pair<PortNumber, qp::ports::Value>>> evaluate(
        NodeId id, const NodeDesc& desc,
        const std::vector<std::pair<PortNumber, qp::ports::Value>>& inputs) = 0;
};

/// @brief The external dependencies evaluation needs.
///
/// @ownership   observes (every pointer is borrowed for one evaluation)
/// @thread      main
/// @pre         The pointees outlive the evaluation
/// @post        none
/// @invariant   `valid()` is true for a context the evaluator will use
/// @errors      noexcept
/// @frozen      no
struct EvalContext final {
    const INodeCatalog* catalog = nullptr;
    const qp::ports::PortTypeRegistry* types = nullptr;
    INodeEvaluator* evaluator = nullptr;
    EvalCache* cache = nullptr;
    /// Where a plugin fault is recorded, or null for a caller that wants the barrier without the counting.
    ///
    /// Optional, and the optionality is the honest part: an evaluation with no log still catches a plugin's
    /// exception -- the barrier is **not** optional -- but nothing counts the faults, so nothing is
    /// quarantined. A caller that wants a broken plugin dropped after `kFaultLimit` faults owns a log and
    /// hands it over, which is the same shape as `cache`: the evaluator does not decide policy, it uses what
    /// it is given.
    qp::plugin::FaultLog* faults = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return catalog != nullptr && types != nullptr && evaluator != nullptr;
    }
};

/// @brief Statistics of one evaluation. For watching performance and answering "why so slow".
struct EvalStats final {
    std::size_t nodes_visited = 0;    ///< nodes visited in topological order
    std::size_t nodes_computed = 0;   ///< number of calls into a real implementation
    std::size_t cache_hits = 0;       ///< number of cache hits
    std::size_t nodes_skipped = 0;    ///< number of nodes skipped because they are bypassed
};

/**
 * @brief The result of one evaluation: every output indexed by (node, port).
 */
class EvalResult final {
public:
    struct Output final {
        NodeId node{};
        PortNumber port = 0;
        qp::ports::Value value{};
    };

    void set(NodeId node, PortNumber port, qp::ports::Value v) {
        outputs_.push_back(Output{node, port, std::move(v)});
    }

    /// @brief Looks up the value of one output port. An unevaluated port yields an invalid value.
    [[nodiscard]] qp::ports::Value get(NodeId node, PortNumber port) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return outputs_.size(); }
    [[nodiscard]] const std::vector<Output>& all() const noexcept { return outputs_; }

private:
    std::vector<Output> outputs_;
};

/**
 * @brief Evaluates the whole graph.
 *
 * Key behaviors:
 *   - Only the subgraph with a downstream user or itself a sink? **No** -- we evaluate the
 *     whole graph today. Pull-based pruning is a later optimization needing declared
 *     outputs (see `core/graph/domain`); the **cache** keeps unrelated branches paid for once.
 *   - A node's parameters and its inputs together form the cache key; a change in either recomputes.
 *   - A `bypassed` node does not call its implementation: input 1 is passed straight to output 1.
 *
 * @ownership   borrows (reads g and ctx only)
 * @thread      main
 * @pre         ctx.valid()
 * @post        On success returns the outputs of every node; graph and version numbers unchanged
 * @post        On failure the return value is undefined, but the graph itself is unmodified
 * @invariant   Determinism: the same graph and parameters give bit-identical results
 * @errors      Result; an error code when a node implementation fails or a type is unregistered
 * @complexity  O(V + E) plus the cost of each node implementation
 * @nondet      none
 * @frozen      no
 * @errors      Result; an error code when a node implementation fails or a type is unregistered, and
 *              `plugin_fault` / `plugin_quarantined` when the implementation misbehaved rather than refused
 * @tests       graph.eval.single_node, graph.eval.chain_propagates,
 *              graph.eval.deterministic_across_runs,
 *              graph.eval.cache_hit_on_second_run,
 *              graph.eval.param_change_invalidates_only_downstream,
 *              graph.eval.unknown_type_fails, graph.eval.missing_param_fails,
 *              graph.eval.bypass_passthrough, graph.eval.diamond_evaluates_once,
 *              graph.eval.is_readonly, graph.eval.result_lookup,
 *              graph.eval.a_raising_evaluator_is_a_fault
 */
[[nodiscard]] Result<EvalStats> evaluate_graph(const Graph& g, const EvalContext& ctx,
                                               EvalResult& out);

}  // namespace qp::graph
