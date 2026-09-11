/**
 * @file plan.hpp
 * @brief Execution plan: the nodes of one domain that **really need computing**, in a fixed order.
 *
 * ## How it differs from "the whole graph"
 *
 * A plan holds only the nodes that **can reach a declared output**. This is where pull-based pruning lands:
 * half the branches of a graph may currently be viewed by nobody, and they should not be computed.
 *
 * ## Why the order must be fixed
 *
 * When one level of the plan offers several candidate nodes, ties break by **slot index**.
 * Without a fixed tie-break rule, two runs could call them in different orders,
 * and any implementation with hidden state (including the cache-hit path) would then give different results --
 * which would directly break charter R2's "bit-for-bit reproduction from the same seed".
 *
 * ## Plan shape of the three domains
 *
 * | domain | plan shape |
 * |---|---|
 * | `field` | topological order; computed once, cacheable |
 * | `particle` | topological order; fully recomputed every frame (**the cache key is the frame number**) |
 * | `render` | **never evaluated**: the plan holds declarations only, no execution items |
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   the node order in a plan is a prefix of a topological order (of the subgraph it covers)
 * @errors      noexcept
 * @frozen      no
 */
#pragma once

#include <qp/graph/domain/declaration.hpp>
#include <qp/graph/domain/domain.hpp>
#include <qp/graph/ir.hpp>

#include <cstddef>
#include <vector>

namespace qp::graph {

/**
 * @brief Execution plan of one domain.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   every node in `order` also belongs to `nodes`
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.domain.plan_empty_when_no_declaration,
 *              graph.domain.plan_covers_only_reachable,
 *              graph.domain.plan_order_is_topological,
 *              graph.domain.plan_order_is_deterministic,
 *              graph.domain.plan_skips_render_domain,
 *              graph.domain.plan_splits_by_domain
 */
class DomainPlan final {
public:
    DomainPlan() = default;
    explicit DomainPlan(Domain d) noexcept : domain_(d) {}

    [[nodiscard]] Domain domain() const noexcept { return domain_; }

    /// @brief Nodes of this domain that take part in evaluation (render-domain declarations excluded).
    [[nodiscard]] const std::vector<NodeId>& nodes() const noexcept { return nodes_; }

    /// @brief The fixed execution order. Empty for the `render` domain.
    [[nodiscard]] const std::vector<NodeId>& order() const noexcept { return order_; }

    /// @brief Outputs declared in this domain (the render domain uses it, since it has no order).
    [[nodiscard]] const std::vector<DeclaredOutput>& declared() const noexcept {
        return declared_;
    }

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty() && declared_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    void add_node(NodeId id) { nodes_.push_back(id); }
    void set_order(std::vector<NodeId> o) { order_ = std::move(o); }
    void add_declared(DeclaredOutput o) { declared_.push_back(o); }

private:
    Domain domain_ = Domain::field;
    std::vector<NodeId> nodes_;
    std::vector<NodeId> order_;
    std::vector<DeclaredOutput> declared_;
};

/// @brief The complete plan for all three domains.
struct ExecutionPlan final {
    DomainPlan field{};
    DomainPlan particle{};
    DomainPlan render{};

    [[nodiscard]] const DomainPlan& of(Domain d) const noexcept {
        switch (d) {
            case Domain::field: return field;
            case Domain::particle: return particle;
            case Domain::render: return render;
        }
        return field;
    }
};

}  // namespace qp::graph
