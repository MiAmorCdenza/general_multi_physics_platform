/**
 * @file node_type_registry.hpp
 * @brief Where node types come from: the mutable side of `INodeCatalog`.
 *
 * ## Why this exists, and why it is in `graph/ir`
 *
 * `INodeCatalog` answers one question -- "what does this type look like" -- and is deliberately read-only,
 * because evaluation, validation and the palette all need to read it and none of them may change it. Something
 * has to **fill** it, and until this file existed that something was `views::TypeCatalog`: the demo library
 * registered into a container that lives in the consumer layer. The consequence was not cosmetic. A content
 * plugin's main contribution is a node type -- its ports, its dimensions, its domain flags -- and with no
 * registry in the foundation there was nowhere for a plugin to put one. "Everything is a plugin" was true of
 * kernels and formats and false of the thing plugins mostly are.
 *
 * It lives beside `NodeDesc` because a registry of descriptors is the descriptor's own business: the type
 * definition, the lookup helpers and the place they are collected belong together, and a module that owns a
 * value type but not its collection is a module every consumer re-implements.
 *
 * ## What registration refuses, and why refusing is the whole job
 *
 * A registry that accepts anything is a place where a plugin's mistake becomes the host's problem much later,
 * in a form nobody can attribute:
 *
 *   - an **empty** type name addresses nothing, and a node created from it could never be found again;
 *   - a **duplicate** type name makes "which descriptor describes this node" depend on registration order --
 *     the same defect as two devices under one instrument id, and worse here because the graph stores the type
 *     *name* and would resolve it against whichever was registered last;
 *   - **duplicate port numbers** within one descriptor make `find_port` return one of two, silently;
 *   - a port numbered **zero** is the "no port" sentinel, so an edge or a parameter can never address it.
 *
 * All four are checked at registration, where the plugin's name is still in hand and the failure can be
 * reported as "plugin X's type Y is malformed" rather than surfacing as a graph that behaves oddly.
 *
 * ## What it does not do
 *
 * It does not check that a port's `PortTypeId` exists in the port registry. That check needs a second registry
 * and belongs where both are available -- `graph/validate` already reports an unknown port type for a graph
 * whose descriptor names one, and doing it twice would give two answers to one question.
 *
 * @ownership   owns (the descriptors it is given, by value)
 * @thread      main (registration) / any (lookup)
 * @pre         none
 * @post        none
 * @invariant   A type name appears at most once
 * @errors      Reports through `diag::Result`
 * @frozen      no
 * @tests       graph.catalog.register_and_find
 */
#pragma once

#include <qp/graph/ir/descriptor.hpp>

#include <qp/diag/result.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qp::graph {

/**
 * @brief A registry of node types, keyed by type name.
 *
 * @ownership   owns the descriptors
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Descriptors keep their registration order, so a palette built from it is stable
 * @errors      See each declaration
 * @frozen      no
 * @tests       graph.catalog.register_and_find
 */
class NodeTypeRegistry final : public INodeCatalog {
public:
    NodeTypeRegistry() = default;
    NodeTypeRegistry(const NodeTypeRegistry&) = delete;
    NodeTypeRegistry& operator=(const NodeTypeRegistry&) = delete;

    /**
     * @brief Registers a node type.
     *
     * The descriptor is **moved in**, so a caller cannot keep editing the copy it handed over while the
     * registry serves the one it kept -- which is how two descriptions of one type come to disagree.
     *
     * @param desc The description. Its type name must be non-empty and unused; its ports must be numbered
     *             above zero and appear at most once per direction.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success `find(desc.type_name)` returns a descriptor equal to the argument, and
     *              `size()` is one larger
     * @invariant   On failure the registry is unchanged
     * @errors      `invalid_argument` for an empty type name, a port numbered zero, or a repeated port
     *              number; `duplicate_connection` for a type name already registered
     * @complexity  O(types + ports)
     * @nondet      none
     * @frozen      no
     * @tests       graph.catalog.register_and_find, graph.catalog.refuses_unusable_descriptions,
     *              graph.catalog.duplicate_name_is_refused
     */
    [[nodiscard]] diag::Result<void> register_type(NodeDesc desc);

    /**
     * @brief Removes a type, for a plugin that is unloading.
     *
     * @param type_name The name to remove.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success `find(type_name)` is null and the other types keep their order
     * @invariant   Removing a type does not touch any other
     * @errors      noexcept; `unknown_node` when no such type is registered
     * @complexity  O(types)
     * @nondet      none
     * @frozen      no
     * @tests       graph.catalog.unload_removes_the_type
     */
    [[nodiscard]] diag::Result<void> remove_type(std::string_view type_name) noexcept;

    /**
     * @brief The description registered under `type_name`, or null.
     *
     * @ownership   borrows from this object
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   The pointer stays valid until the type is removed or the registry dies
     * @errors      noexcept
     * @complexity  O(types)
     * @nondet      none
     * @frozen      no
     * @tests       graph.catalog.register_and_find
     */
    [[nodiscard]] const NodeDesc* find(std::string_view type_name) const noexcept override;

    /// @brief Number of registered types.
    [[nodiscard]] std::size_t size() const noexcept override { return types_.size(); }

    /// @brief Every description, in registration order.
    [[nodiscard]] const std::vector<NodeDesc>& all() const noexcept { return types_; }

    /// @brief Every type in one category, in registration order.
    ///
    /// The query a palette groups by. Computed here rather than in a view so the grouping rule can be asserted
    /// without building widgets -- which is the same reason `FormatRegistry::with_uncertainty` exists.
    [[nodiscard]] std::vector<const NodeDesc*> in_category(std::string_view category) const noexcept;

    /// @brief Drops every registration. For a test or a full reload, not for a running session.
    void clear() noexcept { types_.clear(); }

private:
    std::vector<NodeDesc> types_{};
};

}  // namespace qp::graph
