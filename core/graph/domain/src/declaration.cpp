/**
 * @file declaration.cpp
 * @brief Implementation of the declared-output set.
 */
#include <qp/graph/domain/declaration.hpp>

#include <algorithm>

namespace qp::graph {

bool Declarations::add(DeclaredOutput out) {
    if (!out.valid()) return false;
    if (contains(out)) return false;
    items_.push_back(out);
    return true;
}

bool Declarations::remove(DeclaredOutput out) {
    const auto it = std::find(items_.begin(), items_.end(), out);
    if (it == items_.end()) return false;
    items_.erase(it);
    return true;
}

std::size_t Declarations::remove_node(NodeId node) {
    const auto before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [node](const DeclaredOutput& o) { return o.node == node; }),
                 items_.end());
    return before - items_.size();
}

bool Declarations::contains(DeclaredOutput out) const noexcept {
    return std::find(items_.begin(), items_.end(), out) != items_.end();
}

}  // namespace qp::graph
