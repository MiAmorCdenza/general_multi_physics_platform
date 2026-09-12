/**
 * @file node_type_registry.cpp
 * @brief Registration rules for node types.
 */
#include <qp/graph/ir/node_type_registry.hpp>

#include <qp/graph/ir/ids.hpp>

#include <algorithm>

namespace qp::graph {
namespace {

/// @brief Whether `ports` has a usable numbering: above zero, and each number at most once.
///
/// Port zero is the "no port" sentinel in `ids.hpp`, so a port numbered zero can be addressed by no edge and no
/// parameter -- it would exist in the palette and be unwireable. A repeated number makes `find_port` return one
/// of two, silently, and the panel would edit whichever came first.
[[nodiscard]] bool ports_are_usable(const std::vector<PortDesc>& ports) noexcept {
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i].number == kNoPort) return false;   // zero is the "no port" sentinel in ids.hpp
        for (std::size_t j = i + 1; j < ports.size(); ++j) {
            if (ports[j].number == ports[i].number) return false;
        }
    }
    return true;
}

}  // namespace

diag::Result<void> NodeTypeRegistry::register_type(NodeDesc desc) {
    if (!desc.valid()) return diag::ErrorCode::invalid_argument;
    if (!ports_are_usable(desc.inputs) || !ports_are_usable(desc.outputs)) {
        return diag::ErrorCode::invalid_argument;
    }

    // Refused rather than replaced. A graph stores the type *name*, so two descriptors under one name would
    // make "which one describes this node" depend on registration order -- and the answer would change between
    // the run that saved a document and the run that opened it.
    if (find(desc.type_name) != nullptr) return diag::ErrorCode::duplicate_connection;

    types_.push_back(std::move(desc));
    return {};
}

diag::Result<void> NodeTypeRegistry::remove_type(std::string_view type_name) noexcept {
    const auto it = std::find_if(types_.begin(), types_.end(), [type_name](const NodeDesc& desc) {
        return desc.type_name == type_name;
    });
    if (it == types_.end()) return diag::ErrorCode::unknown_node;
    types_.erase(it);
    return {};
}

const NodeDesc* NodeTypeRegistry::find(std::string_view type_name) const noexcept {
    if (type_name.empty()) return nullptr;
    for (const NodeDesc& desc : types_) {
        if (desc.type_name == type_name) return &desc;
    }
    return nullptr;
}

std::vector<const NodeDesc*> NodeTypeRegistry::in_category(std::string_view category) const noexcept {
    std::vector<const NodeDesc*> out;
    for (const NodeDesc& desc : types_) {
        if (desc.category == category) out.push_back(&desc);
    }
    return out;
}

}  // namespace qp::graph
