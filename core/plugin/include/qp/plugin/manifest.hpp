/**
 * @file manifest.hpp
 * @brief The plugin manifest schema and what the host decides from it.
 *
 * ## Why a manifest schema is foundation and its parser is not
 *
 * A plugin architecture fails in one characteristic way: a version mismatch that
 * nobody notices until the numbers go wrong. So the foundation has to own three
 * things -- what a plugin declares about itself, how the host judges that
 * declaration, and what the host does when the judgement is "no".
 *
 * It does **not** own the file format. Reading YAML, TOML, JSON or a compiled-in
 * table is a serialisation choice, it is replaceable, and the project already
 * treats serialisation as a plugin elsewhere (plan-tree.md section 2.2). This
 * header is therefore a plain value plus pure predicates over it: a format
 * adapter builds a `Manifest`, and nothing downstream can tell which format it
 * came from.
 *
 * ## The judgement is deliberately not semver
 *
 * "1.4 is probably fine on a 1.6 host" is a reasonable guess in source-land and
 * data corruption at ABI level: the plugin's structs are a different size, and
 * the failure shows up as plausible wrong numbers. `qp::abi::check_compatible`
 * skips the inference and returns a definite verdict; `judge` here adds only the
 * platform-level reasons a manifest can be refused before any code is loaded.
 *
 * @ownership   pure (a value type)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A Manifest describes itself completely; nothing is inferred from a file name
 * @errors      noexcept
 * @frozen      yes (the schema is a contract; adding an optional field is allowed)
 * @tests       plugin.manifest.valid, plugin.manifest.rejects_missing_identity,
 *              plugin.manifest.capability_names_are_stable, plugin.manifest.verdict_names
 */
#pragma once

#include <qp/abi/abi_version.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace qp::plugin {

/**
 * @brief What a plugin is allowed to contribute.
 *
 * Capabilities exist so the host can answer "should this plugin be able to do
 * that" before it does it, and so a user can refuse a plugin one power without
 * refusing the plugin. They are also the natural unit for the run ledger: a run
 * records which capabilities were active, which is what makes a result
 * explainable months later.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A capability bit means the same thing in every plugin generation
 * @errors      noexcept
 * @frozen      yes (renumbering a bit would silently change a permission)
 * @tests       plugin.capability.flags
 */
enum class Capability : std::uint32_t {
    none = 0,
    /// Registers node types (the models and instruments of a domain).
    node_types = 1U << 0,
    /// Registers port types (a new kind of value on an edge).
    port_types = 1U << 1,
    /// Provides a native time-stepping kernel.
    kernels = 1U << 2,
    /// Draws or edits something in a view slot.
    view_items = 1U << 3,
    /// Performs file I/O on the user's behalf (import, export, format adapters).
    file_io = 1U << 4,
    /// Bakes field-domain results.
    field_domain = 1U << 5,
    /// Steps particle-domain state.
    particle_domain = 1U << 6,
    /// Declares what a render-domain view shows; never evaluated by the core.
    render_domain = 1U << 7,
};

[[nodiscard]] constexpr Capability operator|(Capability a, Capability b) noexcept {
    return static_cast<Capability>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr Capability operator&(Capability a, Capability b) noexcept {
    return static_cast<Capability>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

/// @brief The set of bits a host understands. Anything outside it is refused.
///
/// A single named constant rather than "check each bit in turn": when a new
/// capability is added, exactly one line changes, and a manifest declaring it is
/// accepted by a host that has it and refused by a host that does not.
inline constexpr Capability kKnownCapabilities =
    Capability::node_types | Capability::port_types | Capability::kernels |
    Capability::view_items | Capability::file_io | Capability::field_domain |
    Capability::particle_domain | Capability::render_domain;

/// @brief Whether at least one bit of `f` is set in `set`.
[[nodiscard]] constexpr bool has_capability(Capability set, Capability f) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(f)) != 0;
}

/// @brief Whether every bit set in `set` is a bit this host knows.
[[nodiscard]] constexpr bool only_known_capabilities(Capability set) noexcept {
    return (static_cast<std::uint32_t>(set) & ~static_cast<std::uint32_t>(kKnownCapabilities)) == 0;
}

/// @brief Stable short name of a single capability bit, for logs and the ledger.
[[nodiscard]] constexpr const char* to_string(Capability c) noexcept {
    switch (c) {
        case Capability::none: return "none";
        case Capability::node_types: return "node_types";
        case Capability::port_types: return "port_types";
        case Capability::kernels: return "kernels";
        case Capability::view_items: return "view_items";
        case Capability::file_io: return "file_io";
        case Capability::field_domain: return "field_domain";
        case Capability::particle_domain: return "particle_domain";
        case Capability::render_domain: return "render_domain";
    }
    return "unknown";
}

/**
 * @brief Why a manifest was accepted or refused.
 *
 * A definite verdict, never a message: the text a user sees is built from the
 * verdict plus the manifest's own fields, and it is never parsed back. Same rule
 * as `ErrorCode` -- free text is for humans.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One verdict per distinct reason
 * @errors      noexcept
 * @frozen      yes (a published verdict must not be renumbered)
 * @tests       plugin.manifest.verdict_names
 */
enum class ManifestVerdict : std::uint8_t {
    ok = 0,
    /// No id, no name, or no version: the plugin cannot be attributed or ordered.
    missing_identity = 1,
    /// The plugin's ABI major differs from the host's.
    abi_major_mismatch = 2,
    /// The plugin was built against a newer ABI minor than the host implements.
    abi_minor_ahead = 3,
    /// The plugin's declared field-buffer layout differs from the host's.
    layout_mismatch = 4,
    /// No capability is declared, so loading the plugin could have no effect.
    no_capabilities = 5,
    /// The plugin names a capability bit this host does not know.
    unknown_capability = 6,
    /// A declared dependency is absent, so the plugin cannot work.
    missing_dependency = 7,
};

/// @brief Stable short name of a verdict. Safe to grep and to alert on.
[[nodiscard]] constexpr const char* to_string(ManifestVerdict v) noexcept {
    switch (v) {
        case ManifestVerdict::ok: return "ok";
        case ManifestVerdict::missing_identity: return "missing_identity";
        case ManifestVerdict::abi_major_mismatch: return "abi_major_mismatch";
        case ManifestVerdict::abi_minor_ahead: return "abi_minor_ahead";
        case ManifestVerdict::layout_mismatch: return "layout_mismatch";
        case ManifestVerdict::no_capabilities: return "no_capabilities";
        case ManifestVerdict::unknown_capability: return "unknown_capability";
        case ManifestVerdict::missing_dependency: return "missing_dependency";
    }
    return "unknown";
}

/**
 * @brief What a plugin declares about itself.
 *
 * Strings are owned: a manifest outlives the file it was read from, and a
 * `string_view` into a parsed buffer would dangle the moment the buffer is
 * released -- typically right after a successful load, which is exactly when the
 * host starts printing the plugin's name in diagnostics.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `id` is unique among installed plugins; `name` is what a user sees
 * @errors      noexcept
 * @frozen      yes (field set; new fields must be optional with a safe default)
 * @tests       plugin.manifest.valid, plugin.manifest.rejects_missing_identity
 */
struct Manifest final {
    /// Stable machine identity, e.g. "org.example.spring". Never shown as a title.
    std::string id{};
    /// Human-readable name, shown in the plugin list.
    std::string name{};
    /// Plugin version, judged against the host's ABI version.
    abi::Version version{};
    /// Field-buffer layout the plugin was built against. Must equal the host's.
    ///
    /// This field is the reason `judge` can detect a layout mismatch at all. An
    /// earlier version passed `host.layout` for both sides of
    /// `abi::check_compatible`, which made that check compare the host with
    /// itself: it could never fire, so every plugin was judged layout-compatible
    /// including one built against a different buffer layout. The bug was
    /// invisible until a test supplied a host with a different layout.
    std::uint16_t layout = abi::kFieldBufferLayout;
    /// What the plugin intends to contribute.
    Capability capabilities = Capability::none;
    /// Ids of plugins that must be loaded first. Ordering, not capability passing.
    std::vector<std::string> dependencies{};

    /// @brief Whether the manifest names itself at all. Says nothing about compatibility.
    [[nodiscard]] bool has_identity() const noexcept {
        return !id.empty() && !name.empty();
    }
};

/**
 * @brief The host side of the compatibility decision: which ABI and layout it offers.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A host description is constant for the lifetime of the process
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.host.judge_accepts_matching, plugin.host.judge_rejects_mismatch
 */
struct HostInfo final {
    abi::Version version{};
    std::uint16_t layout = abi::kFieldBufferLayout;
    /// Capabilities this host is willing to grant. A plugin needing more is refused.
    Capability capabilities = kKnownCapabilities;
};

/// @brief The host description matching the binary that is running.
[[nodiscard]] HostInfo current_host() noexcept;

/**
 * @brief Judges a manifest against a host description. Pure, total, and O(n) in the dependency count.
 *
 * The order of the checks is the order a user should be told about the problems:
 * identity first (nothing else can be reported without it), then the ABI, then
 * the capabilities, then dependencies. A manifest with several problems reports
 * the first, because a plugin that cannot be attributed has no meaningful
 * version complaint.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        Returns ok only when every check passes
 * @invariant   Depends on nothing but its arguments: same inputs, same verdict
 * @errors      noexcept
 * @complexity  O(dependencies.size())
 * @nondet      none
 * @frozen      no
 * @tests       plugin.host.judge_accepts_matching, plugin.host.judge_rejects_mismatch,
 *              plugin.host.judge_check_order
 */
[[nodiscard]] ManifestVerdict judge(const Manifest& m, const HostInfo& host) noexcept;

/**
 * @brief Judges a manifest against the running host, additionally checking that
 *        every declared dependency is in `available_ids`.
 *
 * Dependency checking is separated from `judge` because it needs knowledge the
 * host description does not carry: which plugins are actually installed. Keeping
 * the two apart means the pure predicate stays pure and testable without a
 * filesystem.
 *
 * @ownership   pure
 * @thread      main
 * @pre         `available_ids` holds every installed plugin id, including `m.id`
 * @post        Returns missing_dependency when a required id is absent
 * @invariant   A manifest that depends only on itself is accepted
 * @errors      noexcept
 * @complexity  O(dependencies.size() * available_ids.size())
 * @nondet      none
 * @frozen      no
 * @tests       plugin.host.judge_missing_dependency
 */
[[nodiscard]] ManifestVerdict judge_with_dependencies(
    const Manifest& m, const HostInfo& host,
    const std::vector<std::string>& available_ids) noexcept;

/**
 * @brief Orders manifests so that every dependency loads before its dependents.
 *
 * Deterministic: ties break by index in the input, so a plugin set produces one
 * order rather than "an order". Loading order affects which plugin wins a name
 * collision, which makes it part of what a run reproduces.
 *
 * @ownership   pure (returns indices into `manifests`)
 * @thread      main
 * @pre         none
 * @post        Every dependency of an included plugin appears earlier, or the
 *              cycle list is non-empty and the order is empty
 * @invariant   The result is a permutation of [0, manifests.size())
 * @errors      noexcept
 * @complexity  O(n^2) in the plugin count, which is small by construction
 * @nondet      none
 * @frozen      no
 * @tests       plugin.host.load_order_respects_dependencies,
 *              plugin.host.load_order_detects_cycle
 */
[[nodiscard]] std::vector<std::size_t> load_order(const std::vector<Manifest>& manifests) noexcept;

}  // namespace qp::plugin
