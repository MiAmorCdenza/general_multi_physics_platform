/**
 * @file dimensions.hpp
 * @brief Dimension resolution: **resolve the `same_as_input` constraint on a port into a concrete dimension**.
 *
 * ## Why resolution is needed
 *
 * The port layer (`core/ports`) does **not** decide `DimensionConstraint::same_as_input`
 * -- that needs graph context. A typical example: an adder node means
 * "output dimension = the common dimension of the two inputs", while a node description is static, so
 * it can only declare "I am the same as my input".
 *
 * Resolution must advance along the graph's **topological order**: a node's output dimension may depend on
 * an upstream node's output dimension, and that upstream depends on one further upstream.
 *
 * ## What about cycles
 *
 * `core/graph/structure` already guarantees the graph is acyclic, so a topological order must exist.
 * But the resolver **must not assume that**: it may be used on another container,
 * or at a moment when "structural validation has not run yet".
 * So an unresolvable dimension is **marked unknown and resolution continues**, never a crash or an endless loop.
 *
 * ## An unknown dimension is not an error
 *
 * `unknown` has two causes, both legitimate:
 *   - The upstream is a source whose dimension is not fixed yet (for example "length entered by the user")
 *   - The input port feeding `same_as_input` has no connection (another check should report that)
 * So unknown in the result is information only; whether to report an error is the validation layer's call.
 *
 * @ownership   owns
 * @thread      any (read-only after construction)
 * @pre         none
 * @post        none
 * @invariant   resolving the same graph twice gives the same result (determinism)
 * @errors      noexcept
 * @frozen      no
 */
#pragma once

#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>
#include <qp/ports.hpp>
#include <qp/units.hpp>

#include <vector>

namespace qp::graph {

/// @brief Resolution result of one output port.
struct ResolvedDimension final {
    NodeId node{};
    PortNumber port = 0;
    /// The resolved dimension. Meaningless when `known == false`.
    qp::units::Dim dimension{};
    /// Whether resolution succeeded.
    bool known = false;

    [[nodiscard]] bool valid() const noexcept { return node.valid() && port != 0; }
};

/**
 * @brief The complete result of dimension resolution.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   at most one record per (node, port)
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.validate.dimensions_lookup, graph.validate.dimensions_unknown_for_unset
 */
class DimensionMap final {
public:
    void set(NodeId node, PortNumber port, qp::units::Dim dim) {
        entries_.push_back(ResolvedDimension{node, port, dim, true});
    }
    void set_unknown(NodeId node, PortNumber port) {
        entries_.push_back(ResolvedDimension{node, port, {}, false});
    }

    /// @brief Look up the resolution result of an output port. Returns nullptr when absent.
    [[nodiscard]] const ResolvedDimension* find(NodeId node, PortNumber port) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::vector<ResolvedDimension>& all() const noexcept { return entries_; }

private:
    std::vector<ResolvedDimension> entries_;
};

/**
 * @brief Context needed for dimension resolution: node type catalog + port type registry.
 *
 * Both are read-only, so they are passed by reference rather than copied.
 */
struct ResolveContext final {
    const INodeCatalog* catalog = nullptr;
    const qp::ports::PortTypeRegistry* types = nullptr;

    [[nodiscard]] bool valid() const noexcept { return catalog != nullptr && types != nullptr; }
};

/**
 * @brief Resolve the dimensions of every output port in the graph.
 *
 * @ownership   borrows (ctx is read-only; no reference is kept past the return)
 * @thread      main
 * @pre         ctx.valid()
 * @post        every node with output ports and a registered type gets a result;
 *              ports that cannot be resolved still appear with `known == false` (rather than missing)
 * @invariant   determinism: two calls on the same graph give the same result
 * @errors      noexcept (a failed resolution does not throw; it is expressed as unknown)
 * @complexity  O(V + E)
 * @nondet      none
 * @frozen      no
 * @tests       graph.validate.resolve_dimension_from_port_type,
 *              graph.validate.resolve_same_as_input_chain,
 *              graph.validate.resolve_unknown_when_input_missing,
 *              graph.validate.resolve_is_deterministic,
 *              graph.validate.resolve_ignores_unknown_types
 */
[[nodiscard]] DimensionMap resolve_dimensions(const Graph& g, const ResolveContext& ctx) noexcept;

}  // namespace qp::graph
