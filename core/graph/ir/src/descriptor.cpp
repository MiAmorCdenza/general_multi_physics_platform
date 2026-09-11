/**
 * @file descriptor.cpp
 * @brief Lookup-helper implementation for `NodeDesc`.
 */
#include <qp/graph/ir/descriptor.hpp>

namespace qp::graph {

const PortDesc* NodeDesc::find_port(PortNumber number, bool is_output) const noexcept {
    if (number == 0) return nullptr;
    const auto& list = is_output ? outputs : inputs;
    for (const auto& p : list) {
        if (p.number == number) return &p;
    }
    return nullptr;
}

const PortDesc* NodeDesc::find_by_name(std::string_view name, bool is_output) const noexcept {
    if (name.empty()) return nullptr;
    const auto& list = is_output ? outputs : inputs;
    for (const auto& p : list) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

}  // namespace qp::graph
