/**
 * @file view_items.cpp
 * @brief The third list, defined beside the other two.
 *
 * The binders, the run providers and the view items are three answers to "where does content come from", asked
 * at three different moments of one application's life -- before a run, during a run, after one. They are
 * deliberately in one translation unit for the reason the first two are: a reader looking for "what did the
 * application mount" should find all of it in one place, and three files of ten lines each would make one
 * mechanism look like three.
 *
 * **This file must stay in `views/model/CMakeLists.txt`'s source list.** A definition placed in a file nobody
 * compiles produced a latent link error in this repository once already: the library archived without it and the
 * first target that actually used the symbol failed to link, a round later.
 */
#include <qp/views/model/view_items.hpp>

#include <algorithm>

namespace qp::views::model {

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

}  // namespace qp::views::model
