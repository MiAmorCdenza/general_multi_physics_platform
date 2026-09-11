/**
 * @file manifest.cpp
 * @brief Implementation of the manifest judgement and load ordering.
 */
#include <qp/plugin/manifest.hpp>

#include <algorithm>

namespace qp::plugin {
namespace {

/// @brief Whether `m` dependencies `id`.
[[nodiscard]] bool dependencies_id(const Manifest& m, const std::string& id) {
    return std::find(m.dependencies.begin(), m.dependencies.end(), id) != m.dependencies.end();
}

}  // namespace

HostInfo current_host() noexcept {
    HostInfo h;
    h.version = abi::kHostVersion;
    h.layout = abi::kFieldBufferLayout;
    h.capabilities = kKnownCapabilities;
    return h;
}

ManifestVerdict judge(const Manifest& m, const HostInfo& host) noexcept {
    // Order is the order a user should hear the problems in. Identity first:
    // without a name there is nothing to attach a version complaint to, and
    // "plugin <empty> is incompatible" is not an actionable message.
    if (!m.has_identity()) return ManifestVerdict::missing_identity;

    switch (abi::check_compatible(host.version, m.version, host.layout, m.layout)) {
        case abi::CompatVerdict::compatible:
            break;
        case abi::CompatVerdict::layout_mismatch:
            return ManifestVerdict::layout_mismatch;
        case abi::CompatVerdict::host_too_old:
            return ManifestVerdict::abi_major_mismatch;
        case abi::CompatVerdict::plugin_too_old:
            return ManifestVerdict::abi_major_mismatch;
        case abi::CompatVerdict::plugin_too_new:
            return ManifestVerdict::abi_minor_ahead;
    }

    if (m.capabilities == Capability::none) return ManifestVerdict::no_capabilities;
    if (!only_known_capabilities(m.capabilities)) return ManifestVerdict::unknown_capability;

    // A host may decline to grant a capability it knows about. Refusing rather
    // than silently dropping the bit: a plugin loaded without the power it
    // declared would fail later, at a point unrelated to the real cause.
    if (!only_known_capabilities(host.capabilities) ||
        (static_cast<std::uint32_t>(m.capabilities) &
         ~static_cast<std::uint32_t>(host.capabilities)) != 0) {
        return ManifestVerdict::unknown_capability;
    }

    return ManifestVerdict::ok;
}

ManifestVerdict judge_with_dependencies(
    const Manifest& m, const HostInfo& host,
    const std::vector<std::string>& available_ids) noexcept {
    const ManifestVerdict base = judge(m, host);
    if (base != ManifestVerdict::ok) return base;

    for (const std::string& dep : m.dependencies) {
        // Depending on yourself is not a missing dependency: a plugin with
        // several parts declaring a common id is legitimate, and reporting a
        // cycle here would be wrong.
        if (dep == m.id) continue;
        if (std::find(available_ids.begin(), available_ids.end(), dep) == available_ids.end()) {
            return ManifestVerdict::missing_dependency;
        }
    }
    return ManifestVerdict::ok;
}

std::vector<std::size_t> load_order(const std::vector<Manifest>& manifests) noexcept {
    const std::size_t n = manifests.size();
    std::vector<std::size_t> order;
    order.reserve(n);

    // Kahn's algorithm over the dependency edges, with ties broken by input
    // index. Deterministic on purpose: the load order decides which plugin wins
    // a name collision, which makes it part of what a run has to reproduce.
    std::vector<std::size_t> remaining;
    remaining.reserve(n);
    for (std::size_t i = 0; i < n; ++i) remaining.push_back(i);

    std::vector<bool> placed(n, false);
    while (!remaining.empty()) {
        std::size_t pick = remaining.size();
        for (std::size_t k = 0; k < remaining.size(); ++k) {
            const std::size_t candidate = remaining[k];
            bool ready = true;
            for (const std::string& dep : manifests[candidate].dependencies) {
                if (dep == manifests[candidate].id) continue;
                // A dependency is satisfied when a manifest with that id has
                // already been placed.
                bool satisfied = false;
                for (std::size_t j = 0; j < n; ++j) {
                    if (placed[j] && manifests[j].id == dep) {
                        satisfied = true;
                        break;
                    }
                }
                if (!satisfied) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                pick = k;
                break;
            }
        }

        if (pick == remaining.size()) {
            // Nothing is ready and something remains: a cycle. Returning an
            // empty order rather than a partial one means the caller cannot
            // accidentally load half a plugin set and then run with it.
            return {};
        }
        const std::size_t chosen = remaining[pick];
        order.push_back(chosen);
        placed[chosen] = true;
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(pick));
    }

    return order;
}

}  // namespace qp::plugin
