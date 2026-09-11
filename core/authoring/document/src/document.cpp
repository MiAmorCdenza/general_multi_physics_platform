/**
 * @file document.cpp
 * @brief Implementation of document identity and per-view layout slots.
 */
#include <qp/authoring/document/document.hpp>

#include <algorithm>

namespace qp::authoring {
namespace {

/// @brief The value returned for a view that has no slot.
///
/// A function-local static rather than a member: `get` returns a reference into
/// the slot list, so an absent view needs a reference to something that outlives
/// the call. Returning a reference to a temporary would compile and then dangle.
[[nodiscard]] const std::string& empty_layout() noexcept {
    static const std::string kEmpty{};
    return kEmpty;
}

}  // namespace

const ViewLayouts::Slot* ViewLayouts::find(const ViewId& view) const noexcept {
    const auto it = std::find_if(slots_.begin(), slots_.end(),
                                 [&view](const Slot& s) { return s.view == view; });
    return it == slots_.end() ? nullptr : &*it;
}

void ViewLayouts::set(const ViewId& view, std::string data) noexcept {
    if (view.empty()) return;
    // Replacing in place keeps the view's position, so saving the same document
    // twice writes the same bytes in the same order. An append-then-erase would
    // reorder on every edit and make two identical documents differ in the file.
    for (Slot& slot : slots_) {
        if (slot.view == view) {
            slot.data = std::move(data);
            return;
        }
    }
    slots_.push_back(Slot{view, std::move(data)});
}

const std::string& ViewLayouts::get(const ViewId& view) const noexcept {
    const Slot* slot = find(view);
    return slot == nullptr ? empty_layout() : slot->data;
}

bool ViewLayouts::has(const ViewId& view) const noexcept { return find(view) != nullptr; }

bool ViewLayouts::remove(const ViewId& view) noexcept {
    const auto it = std::find_if(slots_.begin(), slots_.end(),
                                 [&view](const Slot& s) { return s.view == view; });
    if (it == slots_.end()) return false;
    slots_.erase(it);
    return true;
}

std::vector<ViewId> ViewLayouts::view_ids() const noexcept {
    std::vector<ViewId> out;
    out.reserve(slots_.size());
    for (const Slot& s : slots_) out.push_back(s.view);
    return out;
}

}  // namespace qp::authoring
