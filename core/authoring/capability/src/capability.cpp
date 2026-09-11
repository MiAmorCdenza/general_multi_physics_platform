/**
 * @file capability.cpp
 * @brief Implementation of the capability registry.
 */
#include <qp/authoring/capability/capability.hpp>

#include <algorithm>

namespace qp::authoring {
namespace {

/// @brief Whether `entries` already contains an offer of `id`.
[[nodiscard]] bool has_id(const std::vector<CapabilityId>& ids, const CapabilityId& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

NegotiationVerdict Registry::negotiate(ICapabilityProvider& provider,
                                       plugin::Capability host_grant) {
    // The grant is decided first, before a single entry is written. Registering
    // and then discovering that the plugin was not granted what it needs would
    // leave a window in which the provider is reachable, which is exactly the
    // window a permission check exists to close.
    if (!plugin::only_known_capabilities(provider.needs()) ||
        (static_cast<std::uint32_t>(provider.needs()) &
         ~static_cast<std::uint32_t>(host_grant)) != 0) {
        return NegotiationVerdict::needs_refused;
    }

    const std::vector<CapabilityId> offered = provider.offers();
    if (offered.empty()) return NegotiationVerdict::nothing_offered;

    // Validate everything before writing anything. A half-registered plugin
    // cannot be unloaded cleanly: unload() removes what it finds, so an entry
    // written before a later duplicate was detected would need a rollback that
    // nobody remembered to write.
    std::vector<CapabilityId> seen;
    seen.reserve(offered.size());
    for (const CapabilityId& id : offered) {
        if (id.empty()) return NegotiationVerdict::invalid_id;
        // A provider that lists the same id twice would create two entries for
        // one capability, and unload() would then leave one behind. Checked
        // against the ids seen so far, which is the only list that can contain
        // this provider's own earlier entries.
        if (has_id(seen, id)) return NegotiationVerdict::duplicate_provider;
        for (const Entry& e : entries_) {
            if (e.id == id) return NegotiationVerdict::duplicate_provider;
        }
        seen.push_back(id);
    }

    const std::string plugin_id{provider.plugin_id()};
    if (plugin_id.empty() || is_registered(plugin_id)) return NegotiationVerdict::invalid_id;

    for (const CapabilityId& id : offered) {
        Entry entry;
        entry.plugin_id = plugin_id;
        entry.id = id;
        entry.provider = &provider;
        entries_.push_back(std::move(entry));
    }
    return NegotiationVerdict::granted;
}

diag::Result<void> Registry::unload(std::string_view plugin_id) {
    const auto first = std::remove_if(entries_.begin(), entries_.end(),
                                      [plugin_id](const Entry& e) {
                                          return e.plugin_id == plugin_id;
                                      });
    if (first == entries_.end()) return diag::ErrorCode::unknown_node;
    entries_.erase(first, entries_.end());
    return {};
}

bool Registry::available(std::string_view id) const noexcept {
    return provider_of(id) != nullptr;
}

ICapabilityProvider* Registry::provider_of(std::string_view id) const noexcept {
    if (id.empty()) return nullptr;
    for (const Entry& e : entries_) {
        if (e.id == id) return e.provider;
    }
    return nullptr;
}

std::vector<CapabilityId> Registry::available_ids() const noexcept {
    std::vector<CapabilityId> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.id);
    return out;
}

std::vector<std::string> Registry::plugin_ids() const noexcept {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) {
        if (std::find(out.begin(), out.end(), e.plugin_id) == out.end()) {
            out.push_back(e.plugin_id);
        }
    }
    return out;
}

std::size_t Registry::size() const noexcept { return entries_.size(); }

bool Registry::is_registered(std::string_view plugin_id) const noexcept {
    return std::any_of(entries_.begin(), entries_.end(),
                       [plugin_id](const Entry& e) { return e.plugin_id == plugin_id; });
}

}  // namespace qp::authoring
