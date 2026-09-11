/**
 * @file port_ui.cpp
 * @brief Implementation of the port-type UI registry.
 */
#include <qp/authoring/portui/port_ui.hpp>

#include <algorithm>
#include <cmath>

namespace qp::authoring {

bool PortUiDesc::is_valid() const noexcept {
    // Deliberately NOT a check on `port_type`. The fallback description for an
    // unregistered type carries an empty type on purpose, and it must itself pass
    // validation -- otherwise the one path that is supposed to always work is the
    // one path that hands a renderer something invalid. Whether a *registry* will
    // accept the description is a separate question, answered by set().

    // A choice with no options is unrenderable: there is nothing to choose
    // between, and a control that cannot be drawn is worse than the fallback.
    if (editor == EditorKind::choice && choices.empty()) return false;

    // A bound that is not a number would make every comparison false, so the
    // control would accept nothing. Rejecting it here means the plugin fails to
    // load with a reason, instead of producing a panel where typing anything is
    // an error.
    if (minimum.has_value() && std::isnan(*minimum)) return false;
    if (maximum.has_value() && std::isnan(*maximum)) return false;
    if (minimum.has_value() && maximum.has_value() && *minimum > *maximum) return false;
    if (step.has_value() && !(*step > 0.0)) return false;

    return true;
}

bool PortUiDesc::accepts(double value) const noexcept {
    // Infinity as well as NaN: a description that accepted an infinite value
    // would let a caller store one, and every downstream computation would then
    // produce inf or nan with no indication of where it entered.
    if (!std::isfinite(value)) return false;
    if (minimum.has_value() && value < *minimum) return false;
    if (maximum.has_value() && value > *maximum) return false;
    return true;
}

diag::Result<void> PortUiRegistry::set(const PortUiDesc& desc) {
    // An unnamed type is refused here rather than in is_valid(): the registry
    // keys on the type, so an entry with no key could never be looked up and
    // could never be removed.
    if (desc.port_type.empty()) return diag::ErrorCode::invalid_argument;
    if (!desc.is_valid()) return diag::ErrorCode::invalid_argument;

    // Last registration wins, deliberately: a plugin overriding the generic
    // editor for a type it owns is the intended use. The other registries in this
    // project refuse duplicates because there a duplicate is a collision; here it
    // is a substitution, and the plugin load order that decides it is
    // deterministic.
    for (Entry& e : entries_) {
        if (e.port_type == desc.port_type) {
            e.desc = desc;
            return {};
        }
    }
    entries_.push_back(Entry{desc.port_type, desc});
    return {};
}

PortUiDesc PortUiRegistry::describe(const std::string& port_type) const noexcept {
    for (const Entry& e : entries_) {
        if (e.port_type == port_type) return e.desc;
    }
    return fallback(port_type);
}

diag::Result<void> PortUiRegistry::remove(const std::string& port_type) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&port_type](const Entry& e) {
                                     return e.port_type == port_type;
                                 });
    if (it == entries_.end()) return diag::ErrorCode::unknown_node;
    entries_.erase(it);
    return {};
}

bool PortUiRegistry::has(const std::string& port_type) const noexcept {
    return std::any_of(entries_.begin(), entries_.end(), [&port_type](const Entry& e) {
        return e.port_type == port_type;
    });
}

std::vector<std::string> PortUiRegistry::port_types() const noexcept {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.port_type);
    return out;
}

PortUiDesc PortUiRegistry::fallback(const std::string& port_type) noexcept {
    PortUiDesc desc;
    desc.port_type = port_type;

    // read_only, not `number`. An unknown type might hold text, a handle, or
    // something with no numeric meaning at all, and a number control that cannot
    // represent the value is worse than an honest display: the user would see a
    // 0 where their file reference used to be and would believe it.
    desc.editor = EditorKind::read_only;
    // The port type may legitimately be empty -- a node can declare a parameter
    // before its type is registered, and a document can be opened on a machine
    // that has not installed the plugin. The label carries the meaning instead.
    desc.label = port_type.empty() ? std::string{"untyped"} : port_type;
    return desc;
}

}  // namespace qp::authoring
