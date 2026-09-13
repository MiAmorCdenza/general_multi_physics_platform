/**
 * @file node.hpp
 * @brief A node instance in the graph: type + parameter values + location-independent identity.
 *
 * ## The key distinction: `NodeDesc` is the type, `Node` is the instance
 *
 * `NodeDesc` (see descriptor.hpp) describes "what a dipole-field node looks like";
 * there is exactly one per graph. `Node` describes "the third dipole-field node in
 * the graph, with these parameter values"; there is one per instance.
 *
 * Confusing the two is a common mistake: it leads to "editing one node's port
 * definition changes every same-type node in the graph", which the user cannot understand.
 *
 * ## A node holds no state
 *
 * A `Node` holds **declarations** only: type, parameters, bypass flag.
 * Evaluation intermediates belong to the evaluation context, time stepping to the run,
 * and cache to the cache store. A node holding mutable state would make cache invalidation unsolvable.
 *
 * @frozen no (extensible; the semantics of existing fields are frozen)
 */
#pragma once

#include <qp/graph/ir/ids.hpp>
#include <qp/ports/value.hpp>

#include <string>
#include <vector>

namespace qp::graph {

/// @brief One parameter value of a node (indexed by port number).
struct ParamValue final {
    PortNumber number = 0;
    qp::ports::Value value{};
};

/**
 * @brief A node instance in the graph.
 *
 * @ownership   owns
 * @thread      main (both mutation and reading happen on the main thread; eval threads read a snapshot)
 * @pre         none
 * @post        none
 * @invariant   `type_name` does not change while the instance lives (changing type = delete + add)
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.node.construction, graph.node.param_lookup,
 *              graph.node.set_param_replaces, graph.node.bypass_flag,
 *              graph.node.user_name_is_separate_from_id
 */
struct Node final {
    NodeId id{};

    /// Type name. A **copy** of `NodeDesc::type_name` -- the descriptor may be hot-reloaded,
    /// while an instance should remember which type it was created as.
    std::string type_name;

    /// Stable user-facing name (the key in YAML, the reference in reports).
    ///
    /// Separate from `id`: `id` is the internal addressing handle (it carries a generation
    /// and goes stale on delete), `name` is the label for humans. Both may be empty, but only `id` addresses.
    std::string name;

    std::vector<ParamValue> params;

    /// Bypass this node: evaluation passes the matching input straight through to the output.
    /// Used to "temporarily disable a filter node" without cutting the wires.
    bool bypassed = false;

    /// Evaluation-order hint. Only a secondary criterion when the topological sort is ambiguous,
    /// it **does not relax the topological constraint** (the graph is always acyclic).
    std::int32_t order_hint = 0;

    /// @brief Get a parameter value. Returns an invalid Value when unset.
    [[nodiscard]] qp::ports::Value param(PortNumber number) const noexcept {
        for (const auto& p : params) {
            if (p.number == number) return p.value;
        }
        return qp::ports::Value{};
    }

    /**
     * @brief Set a parameter value. Replaces an existing entry, otherwise appends.
     *
     * @ownership   owns (copies the value)
     * @thread      main
     * @pre         number != 0
     * @post        `param(number) == value`
     * @invariant   The same number appears at most once
     * @errors      Allocates when the number is new; a failure to allocate propagates
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       graph.node.set_param_replaces
     */
    void set_param(PortNumber number, qp::ports::Value value) {
        for (auto& p : params) {
            if (p.number == number) {
                p.value = std::move(value);
                return;
            }
        }
        params.push_back(ParamValue{number, std::move(value)});
    }

    /// @brief Erase a parameter value. Returns whether it was actually erased.
    bool erase_param(PortNumber number) noexcept {
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (it->number == number) {
                params.erase(it);
                return true;
            }
        }
        return false;
    }
};

}  // namespace qp::graph
