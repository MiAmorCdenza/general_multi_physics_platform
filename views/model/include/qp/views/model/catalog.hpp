/**
 * @file catalog.hpp
 * @brief The palette's view of the node catalog: grouping, and nothing else.
 *
 * ## What used to be here, and why it is gone
 *
 * This module used to declare `views::TypeCatalog`, an enumerable container of `NodeDesc`s. It existed for a
 * real reason at the time: `graph::INodeCatalog` answers exactly one question -- "what does this type look
 * like" -- and offers no enumeration, because the core only ever looks up a name it already has. An editor
 * needs the other direction, so the enumerable container was written where the enumerating caller was.
 *
 * The cost only became visible when content plugins arrived. A node type is a plugin's main contribution, and
 * with the only enumerable container living in the **consumer** layer, there was nowhere for a plugin to put
 * one: `core/graph/ir` now owns `NodeTypeRegistry`, which is that container, in the layer that owns the
 * descriptor. Two containers of the same thing would be two answers to "which types exist", and the palette
 * would show whichever one it happened to be given.
 *
 * So the container moved down and this file keeps the part that is genuinely the view's: **how a palette
 * groups types**. Grouping is a presentation decision -- the core has no opinion about it -- and it is
 * computed here rather than inside a widget so that it can be asserted without constructing a single Qt
 * object. A palette that grouped wrongly would still draw.
 *
 * @ownership   pure (reads a catalog it does not own)
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   Every type in the catalog appears in exactly one group
 * @errors      May allocate; allocation failure terminates, as elsewhere in this project
 * @frozen      no
 * @tests       views.catalog.grouped_by_category
 */
#pragma once

#include <qp/graph/ir/node_type_registry.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qp::views {

/// @brief Category label used for a type whose descriptor set no category.
///
/// A visible placeholder rather than a dropped entry: a node the palette cannot reach is a node the user
/// cannot place, so "the plugin forgot to set a category" must not look like "the plugin did not register at
/// all". The same reasoning as the plugin list's treatment of a plugin with no name.
inline constexpr std::string_view kUncategorised = "(uncategorised)";

/**
 * @brief The catalog's types, grouped by category, preserving both orders.
 *
 * Groups appear in **first-appearance** order and types keep their registration order within a group. Not
 * sorted: a palette shows types in the order the plugins registered them, which is stable across runs because
 * plugin loading is ordered, and sorting would add a second ordering rule a user could not predict from their
 * plugin list.
 *
 * @param catalog The registry to group. Read only.
 *
 * @ownership   pure (returns pointers into `catalog`)
 * @thread      ui
 * @pre         none
 * @post        One group per distinct category, and every registered type in exactly one of them
 * @invariant   The sum of the groups' sizes equals `catalog.size()`
 * @errors      May allocate
 * @complexity  O(types x groups)
 * @nondet      none
 * @frozen      no
 * @tests       views.catalog.grouped_by_category
 */
[[nodiscard]] std::vector<std::pair<std::string, std::vector<const qp::graph::NodeDesc*>>>
by_category(const qp::graph::NodeTypeRegistry& catalog);

}  // namespace qp::views
