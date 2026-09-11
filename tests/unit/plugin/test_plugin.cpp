/**
 * @file test_plugin.cpp
 * @brief Tests for the plugin manifest contract: judgement and load order.
 *
 * Test case ids match the @tests fields in the plugin headers byte for byte.
 *
 * The judgement is deliberately a pure predicate over a value, so all of this
 * runs without a filesystem, a shared library, or a process boundary. That is the
 * design being tested: everything decidable before any code is mapped into the
 * process must be decided -- and tested -- before any code is mapped in.
 *
 * The property sweep at the end exists because a plugin manifest is the most
 * attacker-adjacent input the platform has: it arrives with the plugin, from
 * outside. `judge` must be total over it and must never report ok for a manifest
 * that fails any single check.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugin.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace qp::plugin;

namespace {

/// @brief A manifest that passes every check, so each test varies one field.
Manifest good_manifest() {
    Manifest m;
    m.id = "org.example.spring";
    m.name = "Spring oscillator";
    m.version = qp::abi::kHostVersion;
    m.capabilities = Capability::node_types;
    return m;
}

}  // namespace

// ===========================================================================
// Capability vocabulary
// ===========================================================================

TEST_CASE("plugin.capability.flags", "[plugin]") {
    STATIC_REQUIRE(!has_capability(Capability::none, Capability::node_types));
    STATIC_REQUIRE(has_capability(Capability::node_types, Capability::node_types));
    STATIC_REQUIRE(has_capability(Capability::kernels, Capability::kernels));
    STATIC_REQUIRE(has_capability(Capability::file_io, Capability::file_io));

    // The bits are independent, so a combination keeps every part of itself.
    const Capability both = Capability::node_types | Capability::kernels;
    REQUIRE(has_capability(both, Capability::node_types));
    REQUIRE(has_capability(both, Capability::kernels));
    REQUIRE_FALSE(has_capability(both, Capability::file_io));

    // Every named bit is inside the known set. A capability that is defined but
    // not listed in kKnownCapabilities would be refused by the host that
    // implements it -- a silent, self-inflicted incompatibility.
    for (const Capability c : {Capability::node_types, Capability::port_types, Capability::kernels,
                               Capability::view_items, Capability::file_io,
                               Capability::field_domain, Capability::particle_domain,
                               Capability::render_domain}) {
        REQUIRE(only_known_capabilities(c));
        REQUIRE(has_capability(kKnownCapabilities, c));
        REQUIRE(std::string(to_string(c)) != "unknown");
    }
    REQUIRE(only_known_capabilities(Capability::none));
}

TEST_CASE("plugin.manifest.capability_names_are_stable", "[plugin]") {
    // These strings reach the run ledger, so a rename invalidates recorded runs.
    STATIC_REQUIRE(std::string_view(to_string(Capability::node_types)) == "node_types");
    STATIC_REQUIRE(std::string_view(to_string(Capability::port_types)) == "port_types");
    STATIC_REQUIRE(std::string_view(to_string(Capability::kernels)) == "kernels");
    STATIC_REQUIRE(std::string_view(to_string(Capability::view_items)) == "view_items");
    STATIC_REQUIRE(std::string_view(to_string(Capability::file_io)) == "file_io");
    STATIC_REQUIRE(std::string_view(to_string(Capability::field_domain)) == "field_domain");
    STATIC_REQUIRE(std::string_view(to_string(Capability::particle_domain)) == "particle_domain");
    STATIC_REQUIRE(std::string_view(to_string(Capability::render_domain)) == "render_domain");
}

TEST_CASE("plugin.manifest.verdict_names", "[plugin]") {
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::ok)) == "ok");
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::missing_identity)) ==
                   "missing_identity");
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::abi_major_mismatch)) ==
                   "abi_major_mismatch");
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::abi_minor_ahead)) ==
                   "abi_minor_ahead");
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::layout_mismatch)) ==
                   "layout_mismatch");
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::no_capabilities)) ==
                   "no_capabilities");
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::unknown_capability)) ==
                   "unknown_capability");
    STATIC_REQUIRE(std::string_view(to_string(ManifestVerdict::missing_dependency)) ==
                   "missing_dependency");

    // ok is the zero value: a default-constructed verdict must mean "accepted"
    // rather than "refused for an unnamed reason".
    STATIC_REQUIRE(static_cast<std::uint8_t>(ManifestVerdict::ok) == 0);
}

// ===========================================================================
// Manifest judgement
// ===========================================================================

TEST_CASE("plugin.manifest.valid", "[plugin]") {
    const Manifest m = good_manifest();
    REQUIRE(m.has_identity());
    REQUIRE(judge(m, current_host()) == ManifestVerdict::ok);

    // The host description matches the binary that is running, and a manifest
    // built against it is compatible by construction. This is the reflexive case
    // that must never be refused: if the platform refuses itself, nothing loads.
    const HostInfo host = current_host();
    REQUIRE(host.version == qp::abi::kHostVersion);
    REQUIRE(host.layout == qp::abi::kFieldBufferLayout);
    REQUIRE(judge(m, host) == ManifestVerdict::ok);
}

TEST_CASE("plugin.manifest.rejects_missing_identity", "[plugin]") {
    const HostInfo host = current_host();

    Manifest no_id = good_manifest();
    no_id.id.clear();
    REQUIRE_FALSE(no_id.has_identity());
    REQUIRE(judge(no_id, host) == ManifestVerdict::missing_identity);

    Manifest no_name = good_manifest();
    no_name.name.clear();
    REQUIRE(judge(no_name, host) == ManifestVerdict::missing_identity);

    // Identity is checked before the version: a plugin that cannot be named has
    // no useful version complaint, and telling the user about a version problem
    // in an unnamed plugin is not actionable.
    Manifest both_wrong = good_manifest();
    both_wrong.id.clear();
    both_wrong.version.major = 99;
    REQUIRE(judge(both_wrong, host) == ManifestVerdict::missing_identity);
}

TEST_CASE("plugin.host.judge_accepts_matching", "[plugin]") {
    HostInfo host = current_host();
    Manifest m = good_manifest();

    // A plugin built for this ABI, with capabilities the host grants.
    m.capabilities = Capability::node_types | Capability::kernels;
    REQUIRE(judge(m, host) == ManifestVerdict::ok);

    // An older minor is accepted: the host is backward compatible within a major.
    m.version = qp::abi::Version{host.version.major, 0, 0};
    REQUIRE(judge(m, host) == ManifestVerdict::ok);

    // A host that grants more is not a problem.
    host.capabilities = kKnownCapabilities;
    m.version = qp::abi::kHostVersion;
    REQUIRE(judge(m, host) == ManifestVerdict::ok);
}

TEST_CASE("plugin.host.judge_rejects_mismatch", "[plugin]") {
    const HostInfo host = current_host();

    SECTION("newer major") {
        Manifest m = good_manifest();
        m.version = qp::abi::Version{static_cast<std::uint16_t>(host.version.major + 1), 0, 0};
        REQUIRE(judge(m, host) == ManifestVerdict::abi_major_mismatch);
    }

    SECTION("older major") {
        Manifest m = good_manifest();
        REQUIRE(host.version.major > 0);
        m.version = qp::abi::Version{static_cast<std::uint16_t>(host.version.major - 1), 0, 0};
        REQUIRE(judge(m, host) == ManifestVerdict::abi_major_mismatch);
    }

    SECTION("newer minor") {
        // Within a major, a plugin built against a newer minor may use fields the
        // host does not have. Loading it would be the "silent wrong numbers"
        // failure the version check exists to prevent.
        Manifest m = good_manifest();
        m.version = qp::abi::Version{host.version.major,
                                 static_cast<std::uint16_t>(host.version.minor + 1), 0};
        REQUIRE(judge(m, host) == ManifestVerdict::abi_minor_ahead);
    }

    SECTION("layout mismatch") {
        // The plugin's own declared layout differs from the host's. This is the
        // check that a "compare the host with itself" bug silently disables: an
        // earlier version of judge() passed host.layout for both sides, so no
        // plugin was ever refused for this reason.
        Manifest m = good_manifest();
        m.layout = static_cast<std::uint16_t>(qp::abi::kFieldBufferLayout + 1);
        REQUIRE(judge(m, current_host()) == ManifestVerdict::layout_mismatch);

        // And the mirror image: a host offering a layout the plugin was not
        // built against refuses it.
        HostInfo other_layout = host;
        other_layout.layout = static_cast<std::uint16_t>(host.layout + 1);
        REQUIRE(judge(good_manifest(), other_layout) == ManifestVerdict::layout_mismatch);
    }

    SECTION("no capabilities") {
        Manifest m = good_manifest();
        m.capabilities = Capability::none;
        REQUIRE(judge(m, host) == ManifestVerdict::no_capabilities);
    }

    SECTION("unknown capability bit") {
        Manifest m = good_manifest();
        m.capabilities = static_cast<Capability>(1U << 31);
        REQUIRE_FALSE(only_known_capabilities(m.capabilities));
        REQUIRE(judge(m, host) == ManifestVerdict::unknown_capability);
    }

    SECTION("capability the host declines to grant") {
        // Refusing rather than silently dropping the bit: a plugin loaded without
        // the power it declared would fail later, somewhere unrelated.
        HostInfo limited = host;
        limited.capabilities = Capability::node_types;
        Manifest m = good_manifest();
        m.capabilities = Capability::file_io;
        REQUIRE(judge(m, limited) == ManifestVerdict::unknown_capability);
    }
}

TEST_CASE("plugin.host.judge_check_order", "[plugin]") {
    // With several problems at once, the reported one is the one a user can act
    // on first. This is a contract, not an implementation detail: the order is
    // what makes the message stable across releases.
    const HostInfo host = current_host();

    Manifest m = good_manifest();
    m.id.clear();                                        // identity
    m.version.major = static_cast<std::uint16_t>(host.version.major + 1);   // version
    m.capabilities = static_cast<Capability>(1U << 31);  // capability
    REQUIRE(judge(m, host) == ManifestVerdict::missing_identity);

    m.id = "org.example.x";
    REQUIRE(judge(m, host) == ManifestVerdict::abi_major_mismatch);

    m.version = qp::abi::kHostVersion;
    REQUIRE(judge(m, host) == ManifestVerdict::unknown_capability);

    m.capabilities = Capability::none;
    REQUIRE(judge(m, host) == ManifestVerdict::no_capabilities);
}

TEST_CASE("plugin.host.judge_missing_dependency", "[plugin]") {
    const HostInfo host = current_host();
    Manifest m = good_manifest();
    m.dependencies = {"org.example.base"};

    REQUIRE(judge_with_dependencies(m, host, {"org.example.spring", "org.example.base"}) ==
            ManifestVerdict::ok);
    REQUIRE(judge_with_dependencies(m, host, {"org.example.spring"}) ==
            ManifestVerdict::missing_dependency);
    REQUIRE(judge_with_dependencies(m, host, {}) == ManifestVerdict::missing_dependency);

    // Depending on yourself is not a missing dependency: a plugin with several
    // parts may declare a common id, and reporting that as an absence would make
    // a legitimate manifest unloadable.
    Manifest self = good_manifest();
    self.dependencies = {self.id};
    REQUIRE(judge_with_dependencies(self, host, {self.id}) == ManifestVerdict::ok);

    // A dependency problem does not mask a version problem: the version is
    // checked first, because a plugin built for another ABI cannot be fixed by
    // installing another plugin.
    Manifest both = m;
    both.version.major = static_cast<std::uint16_t>(host.version.major + 1);
    REQUIRE(judge_with_dependencies(both, host, {}) == ManifestVerdict::abi_major_mismatch);
}

// ===========================================================================
// Load order
// ===========================================================================

TEST_CASE("plugin.host.load_order_respects_dependencies", "[plugin]") {
    std::vector<Manifest> manifests(3);
    manifests[0].id = "c";   // depends on b
    manifests[0].name = "C";
    manifests[0].capabilities = Capability::node_types;
    manifests[0].dependencies = {"b"};
    manifests[1].id = "b";   // depends on a
    manifests[1].name = "B";
    manifests[1].capabilities = Capability::node_types;
    manifests[1].dependencies = {"a"};
    manifests[2].id = "a";   // depends on nothing
    manifests[2].name = "A";
    manifests[2].capabilities = Capability::node_types;

    const std::vector<std::size_t> order = load_order(manifests);
    REQUIRE(order.size() == 3);

    // Every dependency must appear earlier than its dependent.
    const auto position = [&order](std::size_t index) {
        return std::find(order.begin(), order.end(), index) - order.begin();
    };
    REQUIRE(position(2) < position(1));   // a before b
    REQUIRE(position(1) < position(0));   // b before c
}

TEST_CASE("plugin.host.load_order_detects_cycle", "[plugin]") {
    // Two plugins that each require the other. Neither can go first, so there is
    // no valid order at all.
    std::vector<Manifest> manifests(2);
    manifests[0].id = "x";
    manifests[0].name = "X";
    manifests[0].capabilities = Capability::node_types;
    manifests[0].dependencies = {"y"};
    manifests[1].id = "y";
    manifests[1].name = "Y";
    manifests[1].capabilities = Capability::node_types;
    manifests[1].dependencies = {"x"};

    // An empty order signals a cycle. Returning a partial order would let the
    // caller load half a plugin set and then run with it, which is worse than
    // refusing: the run would look successful.
    REQUIRE(load_order(manifests).empty());
}
