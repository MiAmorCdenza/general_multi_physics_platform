/**
 * @file edge.hpp
 * @brief Edge: one connection from an output port to an input port.
 *
 * ## Why edges are immutable
 *
 * Changing an edge's endpoints equals "delete one edge + add one edge". If in-place mutation were allowed:
 *   - Cache invalidation would have to tell "the endpoints changed" from "the edge was replaced";
 *   - The undo stack would have to record the old value, and the node it points at may already be deleted (dangling);
 *   - When views synchronise, "this edge was modified" and "this edge is new" do not mean the same thing.
 *
 * Immutability collapses all three into one thing: **the old edge disappears, a new edge appears**.
 *
 * ## At most one incoming edge per input port
 *
 * This is a **structural invariant**, not a convention. Several incoming edges would make "which value does
 * this input take" undefined; some node-graph systems hide that behind an implicit "last one connected wins",
 * which yields hard-to-reproduce differences the moment a student changes the connection order.
 *
 * Enforced by `core/graph/structure`, which returns
 * `ErrorCode::duplicate_connection` on connect.
 *
 * @frozen yes
 */
#pragma once

#include <qp/graph/ir/ids.hpp>

namespace qp::graph {

/**
 * @brief One connection.
 *
 * @ownership   pure (value type)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `from` must be an output port and `to` must be an input port
 * @errors      noexcept
 * @frozen      yes
 * @tests       graph.edge.construction, graph.edge_equality,
 *              graph.edge_direction_invariant
 */
struct Edge final {
    /// Source: must be an **output** port.
    PortRef from{};
    /// Target: must be an **input** port.
    PortRef to{};

    [[nodiscard]] bool valid() const noexcept {
        return from.valid() && to.valid() &&
               from.direction == PortDirection::output &&
               to.direction == PortDirection::input;
    }

    [[nodiscard]] friend constexpr bool operator==(const Edge& a, const Edge& b) noexcept {
        return a.from == b.from && a.to == b.to;
    }
    [[nodiscard]] friend constexpr bool operator!=(const Edge& a, const Edge& b) noexcept {
        return !(a == b);
    }
};

}  // namespace qp::graph
