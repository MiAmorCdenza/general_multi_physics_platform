/**
 * @file type_catalog.cpp
 * @brief Implementation of the enumerable node-type catalog.
 */
#include <qp/views/model/type_catalog.hpp>

#include <algorithm>

namespace qp::views {
namespace {

/// @brief Whether two descriptions share a category name.
bool same_category(const qp::graph::NodeDesc* d, std::string_view category) {
    return d != nullptr && d->category == category;
}

}  // namespace

qp::diag::Result<void> TypeCatalog::add(qp::graph::NodeDesc desc) noexcept {
    if (desc.type_name.empty()) return qp::diag::ErrorCode::invalid_argument;
    // Refusing a duplicate rather than replacing, unlike the port UI registry: a
    // node type name is what a saved document refers to, so overwriting it would
    // silently change the meaning of every document that already names it. A
    // plugin that wants to supersede a type must use a new name and a migration.
    if (find(desc.type_name) != nullptr) return qp::diag::ErrorCode::duplicate_connection;
    entries_.push_back(std::move(desc));
    return {};
}

const qp::graph::NodeDesc* TypeCatalog::find(std::string_view type_name) const noexcept {
    if (type_name.empty()) return nullptr;
    for (const qp::graph::NodeDesc& d : entries_) {
        if (d.type_name == type_name) return &d;
    }
    return nullptr;
}

std::vector<const qp::graph::NodeDesc*> TypeCatalog::all() const {
    std::vector<const qp::graph::NodeDesc*> out;
    out.reserve(entries_.size());
    for (const qp::graph::NodeDesc& d : entries_) out.push_back(&d);
    return out;
}

std::vector<std::pair<std::string, std::vector<const qp::graph::NodeDesc*>>>
TypeCatalog::by_category() const {
    std::vector<std::pair<std::string, std::vector<const qp::graph::NodeDesc*>>> groups;
    for (const qp::graph::NodeDesc& d : entries_) {
        // A type with no category is grouped under a visible placeholder rather
        // than dropped: a node the palette cannot reach is a node the user cannot
        // place, and "the plugin forgot to set a category" must not look like
        // "the plugin did not register".
        const std::string key =
            d.category.empty() ? std::string{kUncategorised} : d.category;
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&key](const auto& g) { return g.first == key; });
        if (it == groups.end()) {
            groups.emplace_back(key, std::vector<const qp::graph::NodeDesc*>{});
            it = std::prev(groups.end());
        }
        it->second.push_back(&d);
    }
    return groups;
}

bool TypeCatalog::remove(std::string_view type_name) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [type_name](const qp::graph::NodeDesc& d) {
                                     return d.type_name == type_name;
                                 });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

}  // namespace qp::views
