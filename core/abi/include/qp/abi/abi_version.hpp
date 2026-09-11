/**
 * @file abi_version.hpp
 * @brief ABI version and compatibility verdicts.
 *
 * Why the version number must be a first-class citizen (not a comment aside):
 *   a plugin's `.dll` / `.so` is **separately compiled**, so the host cannot see a mismatch at
 *   compile time. The commonest plugin failure is a silent mismatch: no crash, just wrong results.
 *
 * Therefore:
 *   - every cross-boundary struct carries its own version constant;
 *   - the host decides at **load time**, rejects, and never tries "best-effort";
 *   - the decision rule is a pure function, testable without loading a plugin.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The verdict for one (host, plugin) pair is constant
 * @errors      noexcept
 * @frozen      yes (enum values and constant values are frozen)
 * @tests       abi.version.values_are_frozen, abi.version.compatibility_matrix,
 *              abi.version.rejects_newer_major, abi.version.accepts_newer_minor
 */
#pragma once

#include <cstdint>

namespace qp::abi {

/// @brief Major version. **Any layout change** must bump it: fields, alignment, semantics.
inline constexpr std::uint16_t kAbiMajor = 1;

/// @brief Minor version. Added, never changed: new optional capabilities, new enum values.
inline constexpr std::uint16_t kAbiMinor = 0;

/// @brief Layout version of `FieldBuffer`. Evolves independently of kAbiMajor.
inline constexpr std::uint16_t kFieldBufferLayout = 1;

/// @brief Layout version of `LatticeDesc`.
inline constexpr std::uint16_t kLatticeDescLayout = 1;

/// @brief Version triple. Appears in plugin manifests, serialization headers, handshakes.
struct Version final {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;

    [[nodiscard]] friend constexpr bool operator==(Version a, Version b) noexcept {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
    }
    [[nodiscard]] friend constexpr bool operator!=(Version a, Version b) noexcept {
        return !(a == b);
    }
};

/// @brief The host's version.
inline constexpr Version kHostVersion{kAbiMajor, kAbiMinor, 0};

/**
 * @brief The verdict of a version compatibility check.
 *
 * It deliberately **gives a reason** rather than a bool: when a plugin fails
 * to load, the user needs to know why; "load failed" alone says nothing.
 */
enum class CompatVerdict : std::uint8_t {
    /// Fully compatible.
    compatible = 0,
    /// Plugin needs a newer host: loading it may call missing symbols. **Reject**.
    host_too_old = 1,
    /// Plugin major is behind: semantics may have changed. **Reject**.
    plugin_too_old = 2,
    /// Plugin major is ahead: as above, opposite direction. **Reject**.
    plugin_too_new = 3,
    /// Struct layout mismatch (added fields / changed alignment). **Reject**.
    layout_mismatch = 4,
};

/**
 * @brief Decides whether a plugin version can load on a given host version.
 *
 * Rules (deliberately conservative):
 *   - **Major versions must be exactly equal**. A major bump means the layout or
 *     semantics changed; "best-effort compatibility" becomes a silent error.
 *   - plugin minor > host minor -> reject (plugin may use capabilities we lack).
 *   - plugin minor <= host minor -> allow (the host is backward compatible).
 *   - Layout versions must be equal.
 *
 * Note that this rule **deliberately skips** semver backward-compatibility
 * inference: at ABI level, "I think it works" vs "it works" is data corruption.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a definite verdict and never throws
 * @invariant   Reflexive: host_version compared with itself is compatible
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes (rule frozen; relaxing it requires bumping kAbiMajor)
 * @tests       abi.version.compatibility_matrix, abi.version.rejects_newer_major,
 *              abi.version.accepts_newer_minor, abi.version.rejects_older_major,
 *              abi.version.rejects_layout_mismatch, abi.version.reflexive
 */
[[nodiscard]] constexpr CompatVerdict check_compatible(
    Version host, Version plugin,
    std::uint16_t host_layout = kFieldBufferLayout,
    std::uint16_t plugin_layout = kFieldBufferLayout) noexcept {
    if (host_layout != plugin_layout) return CompatVerdict::layout_mismatch;
    if (plugin.major > host.major) return CompatVerdict::host_too_old;
    if (plugin.major < host.major) return CompatVerdict::plugin_too_old;
    if (plugin.minor > host.minor) return CompatVerdict::plugin_too_new;
    return CompatVerdict::compatible;
}

/// @brief Whether it can load (equals verdict == compatible). Use directly in `if`.
[[nodiscard]] constexpr bool is_compatible(Version host, Version plugin,
                                           std::uint16_t host_layout = kFieldBufferLayout,
                                           std::uint16_t plugin_layout = kFieldBufferLayout) noexcept {
    return check_compatible(host, plugin, host_layout, plugin_layout) ==
           CompatVerdict::compatible;
}

/**
 * @brief Stable short name of a verdict. Used in logs and user-facing hints.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty ASCII identifier
 * @invariant   The same verdict always returns the same string
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes
 * @tests       abi.version.verdict_names
 */
[[nodiscard]] constexpr const char* to_string(CompatVerdict v) noexcept {
    switch (v) {
        case CompatVerdict::compatible: return "compatible";
        case CompatVerdict::host_too_old: return "host_too_old";
        case CompatVerdict::plugin_too_old: return "plugin_too_old";
        case CompatVerdict::plugin_too_new: return "plugin_too_new";
        case CompatVerdict::layout_mismatch: return "layout_mismatch";
    }
    return "unknown";
}

}  // namespace qp::abi
