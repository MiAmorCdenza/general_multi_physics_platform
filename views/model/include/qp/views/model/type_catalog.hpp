/**
 * @file type_catalog.hpp
 * @brief A concrete, enumerable node-type catalog for the view layer.
 *
 * ## Why the view layer owns this and the core does not
 *
 * `graph::INodeCatalog` answers exactly one question -- "what is the description
 * of type X" -- and deliberately offers no way to enumerate the types. That is the
 * right interface for the *core*, which only ever looks up a type it already has a
 * name for.
 *
 * An editor needs the other direction: it has to offer the user a list of what can
 * be placed. So the enumerable catalog belongs in the view layer, next to the
 * palette that consumes it. Putting it in the core would mean the foundation owned
 * a container whose only caller is a UI.
 *
 * ## Why registration is not a template or a macro
 *
 * A plugin registers a `NodeDesc` and an optional compute function. Nothing here
 * knows about any particular physics, which is the point: this file must stay
 * valid when every domain plugin is replaced.
 *
 * @ownership   owns (the descriptors, never the descriptors' owner)
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   A type name appears at most once
 * @errors      registration reports failure through diag::Result
 * @frozen      no
 * @tests       views.catalog.register_and_enumerate
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qp::views {

/// @brief Category label used for a type whose descriptor set no category.
///
/// A visible placeholder rather than a dropped entry: a node the palette cannot
/// reach is a node the user cannot place, so "the plugin forgot to set a category"
/// must not look like "the plugin did not register at all".
inline constexpr std::string_view kUncategorised = "(uncategorised)";

/**
 * @brief An enumerable catalog of node types.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   Every registered type has a non-empty type_name
 * @errors      noexcept
 * @frozen      no
 * @tests       views.catalog.register_and_enumerate
 */
class TypeCatalog final : public qp::graph::INodeCatalog {
public:
    TypeCatalog() = default;

    /// @brief Register a narrow-compatible description. Refuses a duplicate name.
    [[nodiscard]] qp::diag::Result<void> add(qp::graph::NodeDesc desc) noexcept;

    // -- INodeCatalog --------------------------------------------------------

    /// @brief Look up by type name, or null when unregistered.
    [[nodiscard]] const qp::graph::NodeDesc* find(std::string_view type_name) const noexcept override;

    /// @brief Number of registered types.
    [[nodiscard]] std::size_t size() const noexcept override { return entries_.size(); }

    // -- Enumeration, which the core interface deliberately does not offer ----

    /**
     * @brief Every registered type, in registration order.
     *
     * Registration order rather than sorted: the palette groups by category and
     * then shows types in the order the plugins were loaded, which is stable
     * across runs because plugin loading is ordered. Sorting here would give a
     * second ordering rule that a user could not predict from their plugin list.
     *
     * @ownership   pure (returns pointers into the catalog)
     * @thread      ui
     * @pre         none
     * @post        One entry per registered type
     * @invariant   The order is the registration order
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       views.catalog.register_and_enumerate
     */
    [[nodiscard]] std::vector<const qp::graph::NodeDesc*> all() const;

    /**
     * @brief Registered type names, grouped by category, preserving both orders.
     *
     * Grouping is computed here rather than in the widget so that it can be tested
     * without constructing a single Qt object. A palette that grouped wrongly
     * would still draw; a test can tell the difference.
     *
     * @ownership   pure (returns pointers into the catalog)
     * @thread      ui
     * @pre         none
     * @post        One group per distinct category, in first-appearance order
     * @invariant   Every registered type appears in exactly one group
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(n x groups)
     * @nondet      none
     * @frozen      no
     * @tests       views.catalog.grouped_by_category
     */
    [[nodiscard]] std::vector<std::pair<std::string, std::vector<const qp::graph::NodeDesc*>>>
    by_category() const;

    /// @brief Remove a type, for an unloading plugin. Returns whether it existed.
    bool remove(std::string_view type_name) noexcept;

private:
    std::vector<qp::graph::NodeDesc> entries_;
};

}  // namespace qp::views
