/**
 * @file layout.cpp
 * @brief Implementation of the layout result helpers and the algorithm registry.
 */
#include <qp/authoring/layout/layout.hpp>

#include <algorithm>

namespace qp::authoring {

Point LayoutResult::position_of(graph::NodeId id) const noexcept {
    for (const Placement& p : placements) {
        if (p.id == id) return p.position;
    }
    // (0, 0) for an unplaced node rather than a failure: a caller drawing a graph
    // asks about every node it is about to draw, and an algorithm that legitimately
    // declines to place one node should not abort the whole draw.
    return Point{};
}

diag::Result<LayoutId> LayoutRegistry::add(const LayoutDesc& desc) {
    if (!desc.valid()) return diag::ErrorCode::invalid_argument;
    // Refusing a duplicate rather than overwriting: two algorithms under one name
    // would make "lay out this document" depend on plugin load order, so the same
    // document would be arranged differently on two machines.
    if (find_by_name(desc.name) != nullptr) return diag::ErrorCode::duplicate_connection;

    Entry entry;
    entry.id = LayoutId{next_index_++};
    entry.desc = desc;
    entries_.push_back(entry);
    return entry.id;
}

diag::Result<void> LayoutRegistry::remove(LayoutId id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [id](const Entry& e) { return e.id == id; });
    if (it == entries_.end()) return diag::ErrorCode::unknown_node;
    entries_.erase(it);
    return {};
}

const LayoutDesc* LayoutRegistry::find_by_name(std::string_view name) const noexcept {
    if (name.empty()) return nullptr;
    for (const Entry& e : entries_) {
        if (e.desc.name == name) return &e.desc;
    }
    return nullptr;
}

const LayoutDesc* LayoutRegistry::find(LayoutId id) const noexcept {
    if (!id.valid()) return nullptr;
    for (const Entry& e : entries_) {
        if (e.id == id) return &e.desc;
    }
    return nullptr;
}

std::vector<LayoutDesc> LayoutRegistry::list() const noexcept {
    std::vector<LayoutDesc> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.desc);
    return out;
}

}  // namespace qp::authoring
