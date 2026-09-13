/**
 * @file view_items.cpp
 * @brief The list of mounted view items.
 *
 * The counterpart of the two lists in `views/model`: the binders say which operator drives a node, the run
 * providers say which run drives a graph, and this says which item draws a declaration. The two view-layer lists
 * are filled by the application because they are *its* vocabulary; this one is filled by the application too,
 * but it lives here because a plugin implements `IViewItem` and a plugin may not depend on `views/`.
 */
#include <qp/graph/domain/view_items.hpp>

#include <algorithm>

namespace qp::graph {

namespace {

/// @brief The one list. See `execution_binders.cpp` for why it is a function-local static.
std::vector<IViewItem*>& item_list() noexcept {
    static std::vector<IViewItem*> items;
    return items;
}

}  // namespace

const std::vector<IViewItem*>& view_items() noexcept { return item_list(); }

void mount_view_item(IViewItem* item) noexcept {
    if (item == nullptr) return;
    std::vector<IViewItem*>& items = item_list();
    if (std::find(items.begin(), items.end(), item) != items.end()) return;
    items.push_back(item);
}

void clear_view_items() noexcept { item_list().clear(); }

}  // namespace qp::graph
