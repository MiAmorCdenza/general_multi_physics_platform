/**
 * @file dimensions.cpp
 * @brief Implementation of dimension resolution.
 *
 * Algorithm: Kahn topological sort + a sweep along that order.
 *
 * For every output port of every node the dimension is determined by this priority:
 *   1. The port type declares `exact`  -> use the declared dimension directly
 *   2. The port type declares `any`    -> unknown (no constraint, and nothing to infer from)
 *   3. The port type declares `same_as_input` -> take the dimension of the **first connected input port**;
 *      if that input is itself `same_as_input`, take the source output dimension of the edge upstream of it
 *      (the upstream was already resolved earlier in the topological order)
 *
 * An unresolvable case is **marked unknown and resolution continues**, never a crash or an endless loop --
 * the resolver may be used at a moment when "structural validation has not run yet".
 */
#include <qp/graph/validate/dimensions.hpp>

#include <algorithm>
#include <deque>

namespace qp::graph {
namespace {

/// @brief Get the node type description. Returns nullptr when unregistered.
[[nodiscard]] const NodeDesc* desc_of(const Graph& g, const ResolveContext& ctx, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr || n->type_name.empty()) return nullptr;
    return ctx.catalog->find(n->type_name);
}

}  // namespace

const ResolvedDimension* DimensionMap::find(NodeId node, PortNumber port) const noexcept {
    for (const auto& e : entries_) {
        if (e.node == node && e.port == port) return &e;
    }
    return nullptr;
}

DimensionMap resolve_dimensions(const Graph& g, const ResolveContext& ctx) noexcept {
    DimensionMap out;
    if (!ctx.valid()) return out;

    // -- Topological order (Kahn) ---------------------------------------------
    //
    // In-degree = how many "input ports that have an incoming edge" this node has.
    const std::size_t n = g.slots().size();
    std::vector<std::size_t> indegree(n, 0);
    for (const auto& e : g.edges()) {
        if (e.to.node.index < n) ++indegree[e.to.node.index];
    }

    std::deque<NodeId> ready;
    for (const auto& s : g.slots()) {
        if (s.occupied && s.node.id.index < n && indegree[s.node.id.index] == 0) {
            ready.push_back(s.node.id);
        }
    }

    // Resolved output-port dimensions, queried by (node, port)
    DimensionMap resolved;

    // Source output port of a given input port. Returns invalid when there is no incoming edge.
    const auto source_of = [&g](NodeId node, PortNumber port) -> PortRef {
        const Edge* e = g.incoming(PortRef{node, port, PortDirection::input});
        return e == nullptr ? PortRef{} : e->from;
    };

    std::size_t processed = 0;
    while (!ready.empty()) {
        const NodeId id = ready.front();
        ready.pop_front();
        ++processed;

        const NodeDesc* d = desc_of(g, ctx, id);
        if (d != nullptr) {
            for (const auto& out_port : d->outputs) {
                const qp::ports::PortTypeDesc* t = ctx.types->find(out_port.type);
                if (t == nullptr) {
                    resolved.set_unknown(id, out_port.number);
                    continue;
                }
                switch (t->constraint) {
                    case qp::ports::DimensionConstraint::exact:
                        resolved.set(id, out_port.number, t->dimension);
                        break;

                    case qp::ports::DimensionConstraint::any:
                        resolved.set_unknown(id, out_port.number);
                        break;

                    case qp::ports::DimensionConstraint::same_as_input: {
                        // Take the dimension of the first connected input port
                        bool found = false;
                        for (const auto& in_port : d->inputs) {
                            const PortRef src = source_of(id, in_port.number);
                            if (!src.valid()) continue;
                            const ResolvedDimension* r =
                                resolved.find(src.node, src.port);
                            if (r != nullptr && r->known) {
                                resolved.set(id, out_port.number, r->dimension);
                                found = true;
                            } else {
                                // The upstream is not resolved yet (or the upstream itself is unknown)
                                resolved.set_unknown(id, out_port.number);
                                found = true;
                            }
                            break;
                        }
                        if (!found) {
                            // No connected input at all -> nothing to infer from
                            resolved.set_unknown(id, out_port.number);
                        }
                        break;
                    }
                }
            }
        }

        // Advance the downstream nodes
        for (const auto& e : g.edges()) {
            if (e.from.node != id) continue;
            if (e.to.node.index >= n) continue;
            if (indegree[e.to.node.index] == 0) continue;
            if (--indegree[e.to.node.index] == 0) {
                ready.push_back(e.to.node);
            }
        }
    }

    // A cycle (which should not happen: the structure layer guarantees acyclicity) -> mark the leftover nodes unknown instead of crashing
    if (processed < g.node_count()) {
        for (const auto& s : g.slots()) {
            if (!s.occupied) continue;
            const NodeDesc* d = desc_of(g, ctx, s.node.id);
            if (d == nullptr) continue;
            for (const auto& out_port : d->outputs) {
                if (resolved.find(s.node.id, out_port.number) == nullptr) {
                    resolved.set_unknown(s.node.id, out_port.number);
                }
            }
        }
    }

    // Move the results to the output (in slots order, which guarantees determinism)
    for (const auto& s : g.slots()) {
        if (!s.occupied) continue;
        const NodeDesc* d = desc_of(g, ctx, s.node.id);
        if (d == nullptr) continue;
        for (const auto& out_port : d->outputs) {
            const ResolvedDimension* r = resolved.find(s.node.id, out_port.number);
            if (r == nullptr) {
                out.set_unknown(s.node.id, out_port.number);
            } else if (r->known) {
                out.set(s.node.id, out_port.number, r->dimension);
            } else {
                out.set_unknown(s.node.id, out_port.number);
            }
        }
    }
    return out;
}

}  // namespace qp::graph
