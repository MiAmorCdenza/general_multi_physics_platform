/**
 * @file plugin_fixture_abi.hpp
 * @brief The hand-written export block the plugin fixtures and their host agree on.
 *
 * Deliberately **not** `qp/plugin/loader.hpp`. The fixtures must be able to
 * disagree with the host -- that is the only way to test a tag check or a version
 * check -- and a fixture that includes the real header agrees by construction, so
 * every check it exercises is guaranteed to pass. This struct is the same bytes
 * written out twice, once here and once in the loader, and the tests confirm the
 * two spellings still line up.
 *
 * Integer members only, no `std::string`, no virtual functions, nothing that needs
 * constructing or destroying. A C++ object crossing a module boundary needs both
 * sides to agree on a heap, a standard library and an exception model; the exported
 * entry point exists so that none of those have to agree.
 */
#pragma once

#include <cstdint>

namespace qp::test::fixture {

/// @brief The magic value at offset 0. Spelled as integers so no tool can help.
inline constexpr std::uint32_t kTag = 0x5150504Bu;  // 'QPPK'
/// @brief The exports-block version this fixture set was written against.
inline constexpr std::uint16_t kExportsVersion = 1;
/// @brief The field-buffer layout the host offers. Fixtures must match it.
inline constexpr std::uint16_t kLayout = 1;

/// @brief Capability bits. `node_type` is bit 0 in the host's set.
inline constexpr std::uint32_t kCapNodeType = 1u << 0;

/**
 * @brief The manifest layout, mirroring `qp::plugin::PluginManifestC` exactly.
 *
 * Pointers and integers only. The fixtures must be able to write a manifest the host
 * can read **without sharing a standard library**, because that is the whole point of
 * the flat descriptor: a `std::string` written by the plugin would be read with the
 * host's layout rules, and the two agree only when both sides were built by the same
 * compiler against the same runtime.
 *
 * `version` is three `uint16`s rather than the host's `abi::Version`, for the same
 * reason -- laid out flat, a mismatch in either side's padding shows up as a failed
 * judgement instead of as a field read from the wrong offset.
 */
struct FixtureManifest final {
    const char* id;
    const char* name;
    std::uint16_t version_major;
    std::uint16_t version_minor;
    std::uint16_t version_patch;
    std::uint16_t layout;
    std::uint32_t capabilities;
    std::uint32_t dependency_count;
    const char* const* dependencies;
};

/// @brief The export block, mirroring `qp::plugin::PluginExports` field for field.
struct FixtureExports final {
    std::uint32_t tag;
    std::uint16_t exports_version;
    std::uint16_t reserved;
    const FixtureManifest* manifest;
    std::int32_t (*register_into)(void* host_context);
    void (*unregister)(void* host_context);
};

/**
 * @brief Host-owned record of how often a counting fixture was called.
 *
 * Lives in the **host's** memory and is reached through the `host_context` pointer.
 * The plugin writes through that pointer and keeps no state of its own, so nothing
 * whose lifetime matters crosses the boundary. This is how the test observes that
 * `unregister` ran *before* the library was released -- an ordering that leaves no
 * trace in the final state, because both orders get there and only one of them
 * reads unmapped memory on the way.
 */
struct Counter final {
    std::int32_t registered = 0;
    std::int32_t unregistered = 0;
    /// Non-zero makes `register_into` fail after recording the attempt, so the
    /// partial-installation cleanup path becomes observable.
    std::int32_t fail_registration = 0;
};

}  // namespace qp::test::fixture
