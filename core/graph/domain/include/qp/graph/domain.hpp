/**
 * @file qp/graph/domain.hpp
 * @brief The single entry point of the domain module: three-domain semantics and plans.
 *
 * ## This module answers two questions
 *
 * 1. **Which nodes belong to which domain**, and the hard constraints that imposes.
 *    The real-time domain runs every frame, so blocking, allocation, exceptions and
 *    virtual calls are banned -- undiscoverable at run time, so declared and checked at load.
 *
 * 2. **Which nodes really need computing**. Declared outputs + reverse reachability = pull
 *    pruning; push evaluation computes the whole graph, branches the view never reads.
 *
 * ## Boundaries with the neighboring modules
 *
 * | Not done here | Owner |
 * |---|---|
 * | Actual evaluation | `core/graph/eval` |
 * | Type and dimension validation | `core/graph/validate` |
 * | Drawing | view plugins (the render domain only declares) |
 * | Time stepping | `core/runtime/run` |
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   building a plan does not modify the graph
 * @errors      noexcept (problems are expressed by a Report)
 * @frozen      no
 * @tests       graph.domain.predicates, graph.domain.capabilities,
 *              graph.domain.declaration_add, graph.domain.declaration_dedup,
 *              graph.domain.declaration_remove, graph.domain.declaration_lookup,
 *              graph.domain.plan_empty_when_no_declaration,
 *              graph.domain.plan_covers_only_reachable,
 *              graph.domain.plan_order_is_topological,
 *              graph.domain.plan_order_is_deterministic,
 *              graph.domain.plan_skips_render_domain,
 *              graph.domain.plan_splits_by_domain,
 *              graph.domain.plan_declared_output_recorded,
 *              graph.domain.allowed_in_domain, graph.domain.render_always_allowed,
 *              graph.domain.report_domain_violation, graph.domain.report_clean,
 *              graph.domain.report_ignores_unknown_types,
 *              graph.domain.plan_is_deterministic,
 *              graph.domain.plan_ignores_stale_declarations,
 *              graph.domain.report_stale_declaration,
 *              graph.domain.report_stale_declaration_for_deleted_node
 */
#pragma once

#include <qp/graph/domain/build.hpp>
#include <qp/graph/domain/declaration.hpp>
#include <qp/graph/domain/domain.hpp>
#include <qp/graph/domain/plan.hpp>

namespace qp::graph {

/// @brief Version of the domain module. Bump when the Domain enum or plan semantics change.
inline constexpr int kDomainVersion = 1;

}  // namespace qp::graph
