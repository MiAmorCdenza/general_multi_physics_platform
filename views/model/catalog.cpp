/**
 * @file catalog.cpp
 * @brief Implementation of the palette's grouping over the core registry.
 */
#include <qp/views/model/catalog.hpp>

#include <algorithm>

namespace qp::views {

std::vector<std::pair<std::string, std::vector<const qp::graph::NodeDesc*>>>
by_category(const qp::graph::NodeTypeRegistry& catalog) {
    std::vector<std::pair<std::string, std::vector<const qp::graph::NodeDesc*>>> groups;
    for (const qp::graph::NodeDesc& desc : catalog.all()) {
        // A type with no category is grouped under a visible placeholder rather than dropped: a node the
        // palette cannot reach is a node the user cannot place, and "the plugin forgot to set a category"
        // must not look like "the plugin did not register".
        const std::string key =
            desc.category.empty() ? std::string{kUncategorised} : desc.category;
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&key](const auto& group) { return group.first == key; });
        if (it == groups.end()) {
            groups.emplace_back(key, std::vector<const qp::graph::NodeDesc*>{});
            it = std::prev(groups.end());
        }
        it->second.push_back(&desc);
    }
    return groups;
}

}  // namespace qp::views
