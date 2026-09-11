/**
 * @file validate.hpp
 * @brief Load-time validation: complain when the experiment opens, not ten minutes into nonsense.
 *
 * ## Why validation must happen at load time
 *
 * Deferring a type or dimension error to evaluation means "the student computed for ten minutes
 * and the result is nonsense". Rejecting at load time costs one error dialog when the experiment
 * opens: an order of magnitude apart. That is the whole case for `validate` in `docs/plan-tree.md`.
 *
 * ## The five layers of validation
 *
 * 1. **Structure**: node types are registered, every edge points at nodes and ports that
 *    really exist, and the type field is not empty. This layer uses the graph alone.
 * 2. **Port existence**: the port numbers at both ends of an edge must really exist in `NodeDesc`.
 *    The graph-structure layer cannot check that -- it does not know about `NodeDesc`.
 * 3. **Connection compatibility**: apply the ports layer's `check_connection` to every edge.
 *    This is the step that actually uses the resolved `same_as_input` result.
 * 4. **Required parameters**: a port marked `required` and not connectable must carry a value.
 * 5. **Domain permission**: whether a node may appear in the domain it was placed in
 *    (the real-time domain forbids blocking/allocation, so it is denied by default).
 *
 * ## Why report every problem at once
 *
 * Stopping at the first error would make the user fix and retry N times to open one experiment.
 * So validation **collects every problem** and returns them together, each located by node/port.
 *
 * @ownership   pure (does not modify the graph)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   validation is read-only: graph and version are unchanged across the call
 * @errors      noexcept (problems are expressed by a Report; nothing is thrown)
 * @frozen      no
 */
#pragma once

#include <qp/graph/structure.hpp>
#include <qp/graph/validate/dimensions.hpp>
#include <qp/graph/validate/report.hpp>

namespace qp::graph {

/**
 * @brief Validation options.
 *
 * Note: the `Domain` enum is **not defined here** -- it belongs to `core/graph/domain`
 * (three-domain semantics are that module's job). An early version defined one
 * `Domain` of the same name in each module, which became a redefinition error in any
 * translation unit including both. Domain semantics deserve exactly one definition.
 *
 * This module therefore offers domain checking in **parameterized** form: the caller (usually
 * `core/graph/domain`) maps `Domain` onto the few flags here.
 * Then validate does not depend on domain, while domain may depend on validate's Report, and
 * no module cycle exists.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   the defaults are "the strictest reasonable setting"
 * @errors      noexcept
 * @frozen      no
 */
struct ValidateOptions final {
    /// Whether to check a node type's permission for the current domain. Off: no domain check.
    bool check_domain = false;
    /// Whether the current domain is allowed to appear in the baked domain.
    bool domain_allows_field = true;
    /// Whether the current domain is allowed to appear in the real-time domain.
    bool domain_allows_particle = true;
    /// Human-readable domain name, used in error messages.
    const char* domain_name = "field";
    /// Whether this domain runs every frame (real-time), for a more accurate hint.
    bool domain_runs_every_frame = false;

    /// Whether "an output port is unconnected" counts as a warning.
    ///
    /// Default **false**: leaving a leaf output unwired is normal (the user wants one result).
    /// Switching it on suits an "experiment template completeness" check.
    bool warn_unconnected_outputs = false;
    /// Whether every required parameter must be present. Default true.
    bool require_required_params = true;
};

/**
 * @brief Validate the whole graph.
 *
 * Read-only: graph and version are **completely unchanged** by the call (a test asserts this).
 *
 * @ownership   borrows (read-only g and ctx)
 * @thread      main
 * @pre         ctx.valid()
 * @post        the returned Report aggregates **all** problems found, not just the first
 * @invariant   validation does not modify the graph; two runs on one graph agree
 * @errors      noexcept
 * @complexity  O(V + E)
 * @nondet      none
 * @frozen      no
 * @tests       graph.validate.ok_on_consistent_graph,
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
 *              graph.validate.rejects_dangling_edge_to_deleted_node
 */
[[nodiscard]] Report validate_graph(const Graph& g, const ResolveContext& ctx,
                                    const ValidateOptions& options = {});

/**
 * @brief Check "can this edge be attached", for previewing before wiring.
 *
 * Unlike `validate_graph`, it answers whether a **single action** may be performed, so
 * `core/graph/mutate` can give a reason before actually mutating the graph.
 *
 * @ownership   borrows
 * @thread      main
 * @pre         ctx.valid()
 * @post        an empty Report means the edge may be made; otherwise the rejection reason
 * @invariant   does not modify the graph
 * @errors      noexcept
 * @complexity  O(V + E)
 * @nondet      none
 * @frozen      no
 * @tests       graph.validate.check_edge_ok, graph.validate.check_edge_rejects_mismatch,
 *              graph.validate.check_edge_rejects_unknown_ports
 */
[[nodiscard]] Report check_edge(const Graph& g, const ResolveContext& ctx, PortRef from,
                                PortRef to);

}  // namespace qp::graph
