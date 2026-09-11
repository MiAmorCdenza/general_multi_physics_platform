/**
 * @file declaration.hpp
 * @brief Declared outputs: **"what I want"**, not "what is in the graph".
 *
 * ## Why it is needed
 *
 * Push-based evaluation (computing downward from source nodes) computes the whole graph,
 * including branches the current view never looks at. Declared outputs make "which results I
 * want" explicit data, so evaluation only has to cover **the subgraph reaching those outputs**.
 *
 * In class: a teacher turns one knob and only branches that affect the current view recompute.
 *
 * ## Why it is part of the graph
 *
 * Declared outputs must be saved with the graph (a student opening the experiment should see
 * the same output list), so they belong to the graph document. Yet they are **neither nodes**
 * **nor edges** -- they are a third kind of element. Forcing them into nodes ("output nodes")
 * would add a crowd of points that exist only as markers and produce no value at all.
 *
 * @ownership   owns
 * @thread      main (edited together with the graph)
 * @pre         none
 * @post        none
 * @invariant   the same (node, port) is declared at most once
 * @errors      noexcept
 * @frozen      no
 */
#pragma once

#include <qp/graph/ir.hpp>

#include <vector>

namespace qp::graph {

/// @brief One declared output port.
struct DeclaredOutput final {
    NodeId node{};
    PortNumber port = 0;

    [[nodiscard]] bool valid() const noexcept { return node.valid() && port != 0; }
    [[nodiscard]] friend constexpr bool operator==(DeclaredOutput a,
                                                   DeclaredOutput b) noexcept {
        return a.node == b.node && a.port == b.port;
    }
    [[nodiscard]] friend constexpr bool operator!=(DeclaredOutput a,
                                                   DeclaredOutput b) noexcept {
        return !(a == b);
    }
};

/**
 * @brief A set of declared outputs.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   no duplicate entries
 * @errors      noexcept
 * @frozen      no
 * @tests       graph.domain.declaration_add, graph.domain.declaration_dedup,
 *              graph.domain.declaration_remove, graph.domain.declaration_lookup
 */
class Declarations final {
public:
/// @brief Add a declaration. Adding a duplicate is idempotent (false means it was there).
    bool add(DeclaredOutput out);

/// @brief Remove a declaration. Returns whether it really was removed.
    bool remove(DeclaredOutput out);

/// @brief Remove every declaration pointing at a node (called when that node is deleted).
    std::size_t remove_node(NodeId node);

/// @brief Whether it is already declared.
    [[nodiscard]] bool contains(DeclaredOutput out) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] const std::vector<DeclaredOutput>& all() const noexcept { return items_; }

    void clear() noexcept { items_.clear(); }

private:
    std::vector<DeclaredOutput> items_;
};

}  // namespace qp::graph
