/**
 * @file capability.hpp
 * @brief Capability negotiation: what a plugin offers, and how the host finds a provider.
 *
 * ## The problem this solves
 *
 * `core/plugin` answers "may this plugin load". It cannot answer "who can export
 * a dataset", because that is a question about the *set* of loaded plugins, and
 * because a plugin may offer several unrelated things at once. Without this
 * module every caller that needs "something that can do X" invents its own scan
 * over the plugin list, and the scans disagree: one takes the first match, one
 * takes the last, one forgets to check whether the plugin actually loaded.
 *
 * ## Declaration and grant are two different things
 *
 * A plugin **declares** what it can do. The host **grants** what it will let the
 * plugin do. The two are deliberately separate types of state:
 *
 *   - a declaration is a claim, and a claim can be false;
 *   - a grant is a decision, and the platform must be able to make it on grounds
 *     the plugin cannot influence -- a user turning a permission off, or a
 *     build refusing file access outright.
 *
 * Collapsing them into one registry would mean the answer to "who can write
 * files" depends on what the plugin said about itself, which is backwards.
 * Hence `Registry::negotiate` computes the grant **before** any implementation is
 * reachable, and `declared()` reports the claim while `granted()` reports the
 * decision.
 *
 * ## Why ids are strings and not integers
 *
 * An integer capability id has to be allocated by somebody, and whoever allocates
 * it becomes a bottleneck the plugin ecosystem has to wait on. A namespaced
 * string ("qp.io.export_dataset") can be minted by the plugin that introduces it,
 * needs no central registry, and is legible in a log line. The cost is one string
 * comparison on a path that runs when a plugin loads, not per frame.
 *
 * @ownership   pure (contract) / owns (the registry)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A provider is only returned when the host granted the capability
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       capability.registry.negotiate_grants_declared,
 *              capability.registry.refuses_ungranted,
 *              capability.registry.duplicate_provider_is_refused
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/plugin/manifest.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qp::authoring {

/**
 * @brief A namespaced capability identifier, e.g. "qp.io.export_dataset".
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Non-empty when it names a real capability
 * @errors      noexcept
 * @frozen      no
 * @tests       capability.registry.negotiate_grants_declared
 */
using CapabilityId = std::string;

/**
 * @brief What one plugin says it can do, and what the host decided about it.
 *
 * @ownership   observes (the plugin owns its declaration; the host owns the verdict)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `verdict` is set before `provider` becomes reachable
 * @errors      noexcept
 * @frozen      no
 * @tests       capability.registry.negotiate_grants_declared
 */
struct Declaration final {
    /// The plugin that makes the claim. Must match a manifest id.
    std::string plugin_id{};
    /// Namespaced capability ids this plugin claims.
    std::vector<CapabilityId> offers{};
    /// What the plugin needs to work. Refused if the host does not grant it.
    plugin::Capability needs = plugin::Capability::none;
};

/**
 * @brief A plugin that offers capabilities.
 *
 * The host calls `declare` once per plugin during load, then `negotiate` against
 * the manifest verdict, and only afterwards may anything call the plugin.
 *
 * @ownership   observes (the plugin owns itself; the registry never deletes it)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `plugin_id` outlives the registration, and never changes
 * @errors      `declare` reports failure through Result rather than throwing
 * @frozen      no
 * @tests       capability.registry.negotiate_grants_declared
 */
class ICapabilityProvider {
public:
    ICapabilityProvider() = default;
    virtual ~ICapabilityProvider() = default;
    ICapabilityProvider(const ICapabilityProvider&) = delete;
    ICapabilityProvider& operator=(const ICapabilityProvider&) = delete;

    /// @brief Stable id, matching the plugin's manifest id.
    [[nodiscard]] virtual std::string_view plugin_id() const noexcept = 0;

    /**
     * @brief The capabilities this plugin offers.
     *
     * @ownership   pure (the registry copies what it needs)
     * @thread      main
     * @pre         none
     * @post        Every returned id is non-empty
     * @invariant   The same provider returns the same set while loaded
     * @errors      noexcept; allocation failure terminates, because a plugin that
     *              cannot say what it offers cannot be negotiated with at all
     * @complexity  O(offers)
     * @nondet      none
     * @frozen      no
     * @tests       capability.registry.negotiate_grants_declared
     */
    [[nodiscard]] virtual std::vector<CapabilityId> offers() const noexcept = 0;

    /**
     * @brief The platform capabilities this plugin needs in order to work.
     *
     * Checked against the grant. A plugin whose needs were refused is not
     * registered at all, rather than registered and broken.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   Constant for the lifetime of the provider
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       capability.registry.refuses_ungranted
     */
    [[nodiscard]] virtual plugin::Capability needs() const noexcept = 0;
};

/**
 * @brief Why a negotiation failed.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One code per distinct reason
 * @errors      noexcept
 * @frozen      yes
 * @tests       capability.registry.duplicate_provider_is_refused
 */
enum class NegotiationVerdict : std::uint8_t {
    granted = 0,
    /// The plugin needs a capability the host did not grant it.
    needs_refused = 1,
    /// Another loaded plugin already provides this capability id.
    duplicate_provider = 2,
    /// The plugin offers nothing, so registering it would have no effect.
    nothing_offered = 3,
    /// A capability id was empty or otherwise unusable.
    invalid_id = 4,
};

/**
 * @brief Stable short name of a verdict.
 *
 * These strings reach the run ledger and the diagnostics, so a rename would
 * invalidate every recorded run that mentioned one.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty ASCII identifier
 * @invariant   Distinct verdicts never share a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes
 * @tests       capability.verdict_names_are_stable
 */
[[nodiscard]] constexpr const char* to_string(NegotiationVerdict v) noexcept {
    switch (v) {
        case NegotiationVerdict::granted: return "granted";
        case NegotiationVerdict::needs_refused: return "needs_refused";
        case NegotiationVerdict::duplicate_provider: return "duplicate_provider";
        case NegotiationVerdict::nothing_offered: return "nothing_offered";
        case NegotiationVerdict::invalid_id: return "invalid_id";
    }
    return "unknown";
}

/**
 * @brief Registry of capability providers: who is loaded, who offers what.
 *
 * @ownership   owns (the entries, never the providers)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A capability id maps to at most one granted provider
 * @errors      noexcept (failure is reported through Result)
 * @frozen      no
 * @tests       capability.registry.negotiate_grants_declared
 */
class Registry final {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /**
     * @brief Registers a provider, granting the capabilities it claims.
     *
     * The grant is computed here and now, from `host_grant` and the provider's
     * own `needs`, before any caller can reach the provider. Nothing about the
     * plugin's self-description can widen its own permission.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `provider` outlives the registration
     * @post        On success provider_of() returns it for each offered id
     * @invariant   On failure the registry is byte-for-byte as it was
     * @errors      Returns a NegotiationVerdict describing the refusal
     * @complexity  O(offers x registered)
     * @nondet      none
     * @frozen      no
     * @tests       capability.registry.negotiate_grants_declared,
     *              capability.registry.failed_registration_leaves_no_trace
     */
    [[nodiscard]] NegotiationVerdict negotiate(ICapabilityProvider& provider,
                                               plugin::Capability host_grant);

    /**
     * @brief Removes a provider and everything it offered.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        provider_of() returns null for every id it offered
     * @invariant   Other providers are unaffected
     * @errors      Returns unknown_node when the plugin is not registered
     * @complexity  O(registered)
     * @nondet      none
     * @frozen      no
     * @tests       capability.registry.unload_removes_offers
     */
    [[nodiscard]] diag::Result<void> unload(std::string_view plugin_id);

    /// @brief Whether some registered plugin offers `id` and was granted it.
    [[nodiscard]] bool available(std::string_view id) const noexcept;

    /// @brief The provider for `id`, or null. Null means "nobody can do this".
    [[nodiscard]] ICapabilityProvider* provider_of(std::string_view id) const noexcept;

    /// @brief Every capability id currently available, in registration order.
    ///
    /// Registration order, not sorted: a caller that forwards to "the first
    /// provider" gets a stable answer only if this order is stable, and it is the
    /// order the plugins were loaded in.
    ///
    /// @ownership   pure (returns copies)
    /// @thread      main
    /// @pre         none
    /// @post        One entry per granted offer
    /// @invariant   Repeated calls without intervening mutation return an equal vector
    /// @errors      noexcept; allocation failure terminates
    /// @complexity  O(n)
    /// @nondet      none
    /// @frozen      no
    /// @tests       capability.registry.negotiate_grants_declared
    [[nodiscard]] std::vector<CapabilityId> available_ids() const noexcept;

    /// @brief The plugin ids that were granted a registration, in order.
    ///
    /// @ownership   pure (returns copies)
    /// @thread      main
    /// @pre         none
    /// @post        One entry per registered plugin, deduplicated
    /// @invariant   A plugin offering three capabilities appears once
    /// @errors      noexcept; allocation failure terminates
    /// @complexity  O(n)
    /// @nondet      none
    /// @frozen      no
    /// @tests       capability.registry.negotiate_grants_declared
    [[nodiscard]] std::vector<std::string> plugin_ids() const noexcept;

    /// @brief How many capability ids are available.
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief Whether `plugin_id` is registered.
    [[nodiscard]] bool is_registered(std::string_view plugin_id) const noexcept;

private:
    struct Entry final {
        std::string plugin_id{};
        CapabilityId id{};
        ICapabilityProvider* provider = nullptr;
    };

    std::vector<Entry> entries_;
};

}  // namespace qp::authoring
