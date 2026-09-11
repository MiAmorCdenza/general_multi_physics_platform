/**
 * @file build.hpp
 * @brief Builds the execution plan and checks domain permission.
 *
 * @ownership   pure (does not modify the graph)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Building a plan does not modify the graph
 * @errors      noexcept
 * @frozen      no
 */
#pragma once

#include <qp/graph/domain/declaration.hpp>
#include <qp/graph/domain/plan.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>
#include <qp/graph/validate/report.hpp>

namespace qp::graph {

/// @brief Context needed to build a plan.
struct PlanContext final {
    const INodeCatalog* catalog = nullptr;

    [[nodiscard]] bool valid() const noexcept { return catalog != nullptr; }
};

/**
 * @brief Builds the execution plans for the three domains.
 *
 * Algorithm:
 *   1. from the declared outputs, **walk back** along directed edges to reachable nodes;
 *   2. run a Kahn topological sort on that reachable subset, ties broken by slot index;
 *   3. split the result into three plans by the domain each node belongs to.
 *
 * Undeclared nodes **enter no plan at all** -- that is exactly the pruning.
 *
 * @ownership   borrows (reads g and ctx only)
 * @thread      main
 * @pre         ctx.valid()
 * @post        The returned plan holds only reachable nodes, in a deterministic topological order
 * @invariant   The same graph and declarations give the same plan twice (determinism)
 * @errors      noexcept
 * @complexity  O(V + E)
 * @nondet      none
 * @frozen      no
 * @tests       graph.domain.plan_empty_when_no_declaration,
 *              graph.domain.plan_covers_only_reachable,
 *              graph.domain.plan_order_is_topological,
 *              graph.domain.plan_order_is_deterministic,
 *              graph.domain.plan_skips_render_domain,
 *              graph.domain.plan_splits_by_domain,
 *              graph.domain.plan_declared_output_recorded
 */
[[nodiscard]] ExecutionPlan build_plan(const Graph& g, const PlanContext& ctx,
                                       const Declarations& declared) noexcept;

/**
 * @brief Whether a node type is allowed to appear in a domain.
 *
 * Use: load-time validation (adds a domain-semantics layer over `core/graph/validate`)
 * and node-palette filtering (list only the types usable in the current domain).
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Decided from the two domain flags of NodeDesc; the render domain is always true
 *              (render is not evaluated, so no realtime constraint applies)
 * @invariant   Strictly consistent with the NodeDesc flags
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       graph.domain.allowed_in_domain, graph.domain.render_always_allowed
 */
[[nodiscard]] bool allowed_in_domain(const NodeDesc& desc, Domain d) noexcept;

/**
 * @brief Validates domain permission across the whole graph.
 *
 * Same check as `core/graph/validate::validate_graph`, but this function does the
 * domain item only, for callers such as "switch domain" that care about nothing else.
 *
 * @ownership   pure
 * @thread      main
 * @pre         ctx.valid()
 * @post        The returned Report contains domain-related problems only
 * @invariant   Does not modify the graph
 * @errors      Does not throw; allocation failure while building diagnostic text terminates
 * @complexity  O(V)
 * @nondet      none
 * @frozen      no
 * @tests       graph.domain.report_domain_violation, graph.domain.report_clean,
 *              graph.domain.report_ignores_unknown_types
 */
[[nodiscard]] Report check_domains(const Graph& g, const PlanContext& ctx, Domain d);

/**
 * @brief Checks whether a node's declared outputs reference ports that no longer exist.
 *
 * A declaration can go stale when a node is deleted or a type is hot-reloaded. A stale
 * declaration must be **reported**, not ignored: "I declared an output, where is it?"
 *
 * @ownership   pure
 * @thread      main
 * @pre         ctx.valid()
 * @post        Every stale declaration yields one error
 * @invariant   Does not modify the graph
 * @errors      noexcept
 * @complexity  O(D)
 * @nondet      none
 * @frozen      no
 * @tests       graph.domain.report_stale_declaration
 */
[[nodiscard]] Report check_declarations(const Graph& g, const PlanContext& ctx,
                                        const Declarations& declared);

}  // namespace qp::graph
