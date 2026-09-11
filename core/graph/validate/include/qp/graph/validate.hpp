/**
 * @file qp/graph/validate.hpp
 * @brief The single entry point of the validate module: load-time validation.
 *
 * ## The only reason this module exists
 *
 * Deferring a type or dimension error to evaluation looks like "ten minutes, absurd result".
 * Rejecting it at load time costs "an error dialog when the experiment opens".
 * The two differ by an order of magnitude in classroom impact.
 *
 * ## It contains one non-obvious thing: dimension resolution
 *
 * `same_as_input` ("my output dimension equals the input's") cannot be decided at
 * the port layer -- it needs graph context. The validation layer resolves it to a concrete
 * dimension along the topological order, and only then can an edge's ends be compared.
 *
 * So this module is not just "calling the ports check functions": it adds the step
 * **that is only possible with the whole graph in view**.
 *
 * ## Boundary with neighbouring modules
 *
 * | Not done                                     | Owned by                                     |
 * |----------------------------------------------|----------------------------------------------|
 * | structural constraints (cycles, port use)    | `core/graph/structure` (rejected on connect) |
 * | the **single-point** type-compatibility test | `core/ports`                                 |
 * | evaluation                                   | `core/graph/eval`                            |
 * | showing a problem in the UI                  | the view layer                               |
 *
 * @ownership   pure (reads the graph only)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Validation does not modify the graph: graph and version are unchanged
 * @errors      noexcept (problems are expressed as a Report)
 * @frozen      no
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

/// @brief Version of the validate module. Bumped when Issue/Severity or the rules change.
inline constexpr int kValidateVersion = 1;

}  // namespace qp::graph
