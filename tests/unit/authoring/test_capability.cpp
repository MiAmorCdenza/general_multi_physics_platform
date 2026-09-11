/**
 * @file test_capability.cpp
 * @brief Tests for capability negotiation.
 *
 * Test case ids match the @tests fields in the capability headers byte for byte.
 *
 * The cases that matter are the refusals. "A plugin that offers X can be found by
 * X" is the easy half; the half that keeps a platform honest is that a plugin
 * whose needs were refused is not registered at all, that a refused or failed
 * registration leaves the registry byte-for-byte unchanged, and that the answer
 * to "who can write files" is the host's decision rather than the plugin's
 * claim.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/authoring/capability.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace qp::authoring;
using qp::plugin::Capability;

namespace {

/// @brief A provider that offers a fixed set of capability ids.
class FakeProvider final : public ICapabilityProvider {
public:
    FakeProvider(std::string id, std::vector<CapabilityId> offers,
                 Capability needs = Capability::none)
        : id_(std::move(id)), offers_(std::move(offers)), needs_(needs) {}

    [[nodiscard]] std::string_view plugin_id() const noexcept override { return id_; }
    [[nodiscard]] std::vector<CapabilityId> offers() const noexcept override { return offers_; }
    [[nodiscard]] Capability needs() const noexcept override { return needs_; }

    void set_offers(std::vector<CapabilityId> offers) { offers_ = std::move(offers); }

private:
    std::string id_;
    std::vector<CapabilityId> offers_;
    Capability needs_;
};

/// @brief A snapshot of everything observable about a registry.
struct Snapshot final {
    std::vector<CapabilityId> ids;
    std::vector<std::string> plugins;
    std::size_t size = 0;

    [[nodiscard]] friend bool operator==(const Snapshot& a, const Snapshot& b) {
        return a.ids == b.ids && a.plugins == b.plugins && a.size == b.size;
    }
};

Snapshot snapshot_of(const Registry& r) {
    Snapshot s;
    s.ids = r.available_ids();
    s.plugins = r.plugin_ids();
    s.size = r.size();
    return s;
}

}  // namespace

// ===========================================================================
// Verdict vocabulary
// ===========================================================================

TEST_CASE("capability.verdict_names_are_stable", "[capability]") {
    STATIC_REQUIRE(std::string_view(to_string(NegotiationVerdict::granted)) == "granted");
    STATIC_REQUIRE(std::string_view(to_string(NegotiationVerdict::needs_refused)) ==
                   "needs_refused");
    STATIC_REQUIRE(std::string_view(to_string(NegotiationVerdict::duplicate_provider)) ==
                   "duplicate_provider");
    STATIC_REQUIRE(std::string_view(to_string(NegotiationVerdict::nothing_offered)) ==
                   "nothing_offered");
    STATIC_REQUIRE(std::string_view(to_string(NegotiationVerdict::invalid_id)) == "invalid_id");

    // granted is the zero value, so a default-constructed verdict means
    // "accepted" rather than "refused for an unnamed reason".
    STATIC_REQUIRE(static_cast<std::uint8_t>(NegotiationVerdict::granted) == 0);
}

// ===========================================================================
// Registration and lookup
// ===========================================================================

TEST_CASE("capability.registry.negotiate_grants_declared", "[capability]") {
    Registry reg;
    REQUIRE(reg.size() == 0);
    REQUIRE_FALSE(reg.available("qp.io.export_dataset"));
    REQUIRE(reg.provider_of("qp.io.export_dataset") == nullptr);
    REQUIRE(reg.available_ids().empty());
    REQUIRE(reg.plugin_ids().empty());

    FakeProvider exporter{"org.example.csv", {"qp.io.export_dataset"}};
    REQUIRE(reg.negotiate(exporter, Capability::file_io) == NegotiationVerdict::granted);

    REQUIRE(reg.size() == 1);
    REQUIRE(reg.available("qp.io.export_dataset"));
    REQUIRE(reg.provider_of("qp.io.export_dataset") == &exporter);
    REQUIRE(reg.is_registered("org.example.csv"));
    REQUIRE_FALSE(reg.is_registered("org.example.other"));
    REQUIRE(reg.available_ids() == std::vector<CapabilityId>{"qp.io.export_dataset"});
    REQUIRE(reg.plugin_ids() == std::vector<std::string>{"org.example.csv"});

    // One plugin can offer several unrelated things, and every one of them must
    // resolve. A registry that kept only the first offer would make the second
    // capability silently unreachable.
    FakeProvider multi{"org.example.multi",
                       {"qp.model.spring", "qp.instrument.ruler", "qp.kernel.boris"}};
    REQUIRE(reg.negotiate(multi, Capability::node_types | Capability::kernels) ==
            NegotiationVerdict::granted);
    REQUIRE(reg.size() == 4);
    REQUIRE(reg.provider_of("qp.model.spring") == &multi);
    REQUIRE(reg.provider_of("qp.instrument.ruler") == &multi);
    REQUIRE(reg.provider_of("qp.kernel.boris") == &multi);
    REQUIRE(reg.available_ids().size() == 4);
    REQUIRE(reg.plugin_ids().size() == 2);

    // Order is registration order, not sorted: a caller that takes "the first
    // provider of X" gets a stable answer only if this list is stable.
    REQUIRE(reg.available_ids()[0] == "qp.io.export_dataset");
    REQUIRE(reg.available_ids()[1] == "qp.model.spring");

    // An unknown id and an empty id both resolve to nothing rather than to some
    // default provider.
    REQUIRE_FALSE(reg.available("qp.nonexistent"));
    REQUIRE_FALSE(reg.available(""));
    REQUIRE(reg.provider_of("") == nullptr);
}

TEST_CASE("capability.registry.refuses_ungranted", "[capability]") {
    Registry reg;

    SECTION("a need the host does not grant refuses the load") {
        // The plugin claims it can write files and says it needs to. A host that
        // granted only node types must refuse rather than register it, because a
        // plugin loaded without the power it declared fails later, somewhere
        // unrelated to the real cause.
        FakeProvider writer{"org.example.writer", {"qp.io.export_dataset"},
                            Capability::file_io};
        REQUIRE(reg.negotiate(writer, Capability::node_types) ==
                NegotiationVerdict::needs_refused);
        REQUIRE(reg.size() == 0);
        REQUIRE_FALSE(reg.available("qp.io.export_dataset"));
        REQUIRE_FALSE(reg.is_registered("org.example.writer"));
    }

    SECTION("a need the host grants is accepted") {
        FakeProvider writer{"org.example.writer", {"qp.io.export_dataset"},
                            Capability::file_io};
        REQUIRE(reg.negotiate(writer, Capability::file_io) == NegotiationVerdict::granted);
        REQUIRE(reg.available("qp.io.export_dataset"));
    }

    SECTION("a need outside the known set is refused even if the host grants everything") {
        // An unknown bit cannot be honoured by any host, so granting it would be
        // granting something nobody implements.
        FakeProvider exotic{"org.example.exotic", {"qp.model.x"},
                            static_cast<Capability>(1U << 31)};
        REQUIRE(reg.negotiate(exotic, qp::plugin::kKnownCapabilities) ==
                NegotiationVerdict::needs_refused);
        REQUIRE(reg.size() == 0);
    }

    SECTION("offering nothing is refused") {
        // A provider with no offers contributes nothing, and registering it
        // would put an entry in the plugin list that no query can ever find.
        FakeProvider empty{"org.example.empty", {}};
        REQUIRE(reg.negotiate(empty, qp::plugin::kKnownCapabilities) ==
                NegotiationVerdict::nothing_offered);
        REQUIRE(reg.size() == 0);
    }

    SECTION("an empty capability id is refused") {
        FakeProvider bad{"org.example.bad", {"qp.model.ok", ""}};
        REQUIRE(reg.negotiate(bad, Capability::node_types) == NegotiationVerdict::invalid_id);
        REQUIRE(reg.size() == 0);   // and the valid id was not half-registered
        REQUIRE_FALSE(reg.available("qp.model.ok"));
    }
}

TEST_CASE("capability.registry.duplicate_provider_is_refused", "[capability]") {
    Registry reg;
    FakeProvider first{"org.example.first", {"qp.io.export_dataset"}};
    REQUIRE(reg.negotiate(first, Capability::file_io) == NegotiationVerdict::granted);

    SECTION("two plugins cannot provide the same capability") {
        // Refusing rather than overwriting: with two providers the answer to
        // "who exports a dataset" would depend on load order, and a saved
        // document that meant the first would silently get the second.
        FakeProvider second{"org.example.second", {"qp.io.export_dataset"}};
        REQUIRE(reg.negotiate(second, Capability::file_io) ==
                NegotiationVerdict::duplicate_provider);
        REQUIRE(reg.size() == 1);
        REQUIRE(reg.provider_of("qp.io.export_dataset") == &first);
        REQUIRE_FALSE(reg.is_registered("org.example.second"));
    }

    SECTION("one plugin cannot list the same id twice") {
        FakeProvider twice{"org.example.twice", {"qp.model.a", "qp.model.a"}};
        REQUIRE(reg.negotiate(twice, Capability::node_types) ==
                NegotiationVerdict::duplicate_provider);
        REQUIRE(reg.size() == 1);
        REQUIRE_FALSE(reg.available("qp.model.a"));
    }

    SECTION("one plugin cannot register under an id already taken") {
        FakeProvider again{"org.example.first", {"qp.model.b"}};
        REQUIRE(reg.negotiate(again, Capability::node_types) == NegotiationVerdict::invalid_id);
        REQUIRE_FALSE(reg.available("qp.model.b"));
    }

    SECTION("an empty plugin id is refused") {
        FakeProvider anonymous{"", {"qp.model.c"}};
        REQUIRE(reg.negotiate(anonymous, Capability::node_types) ==
                NegotiationVerdict::invalid_id);
        REQUIRE_FALSE(reg.available("qp.model.c"));
    }
}

TEST_CASE("capability.registry.failed_registration_leaves_no_trace", "[capability]") {
    // The property that makes unload() reliable: a registration either happens
    // completely or not at all. A half-written plugin would need a rollback that
    // nobody wrote, and the leftover entry would be reachable for the rest of
    // the process.
    Registry reg;
    FakeProvider existing{"org.example.existing", {"qp.model.a"}};
    REQUIRE(reg.negotiate(existing, Capability::node_types) == NegotiationVerdict::granted);
    const Snapshot before = snapshot_of(reg);

    // Offer order matters for this test: the valid id comes first, so a
    // registry that wrote entries as it validated would leave it behind.
    FakeProvider bad{"org.example.bad", {"qp.model.b", "qp.model.a"}};
    REQUIRE(reg.negotiate(bad, Capability::node_types) ==
            NegotiationVerdict::duplicate_provider);
    REQUIRE(snapshot_of(reg) == before);

    FakeProvider invalid{"org.example.invalid", {"qp.model.c", ""}};
    REQUIRE(reg.negotiate(invalid, Capability::node_types) == NegotiationVerdict::invalid_id);
    REQUIRE(snapshot_of(reg) == before);

    FakeProvider refused{"org.example.refused", {"qp.model.d"}, Capability::file_io};
    REQUIRE(reg.negotiate(refused, Capability::node_types) ==
            NegotiationVerdict::needs_refused);
    REQUIRE(snapshot_of(reg) == before);

    FakeProvider nothing{"org.example.nothing", {}};
    REQUIRE(reg.negotiate(nothing, Capability::node_types) ==
            NegotiationVerdict::nothing_offered);
    REQUIRE(snapshot_of(reg) == before);
}

TEST_CASE("capability.registry.unload_removes_offers", "[capability]") {
    Registry reg;
    FakeProvider multi{"org.example.multi", {"qp.model.a", "qp.model.b"}};
    FakeProvider other{"org.example.other", {"qp.io.export_dataset"}};
    REQUIRE(reg.negotiate(multi, Capability::node_types | Capability::file_io) ==
            NegotiationVerdict::granted);
    REQUIRE(reg.negotiate(other, Capability::node_types | Capability::file_io) ==
            NegotiationVerdict::granted);
    REQUIRE(reg.size() == 3);

    REQUIRE(reg.unload("org.example.multi").has_value());

    // Both of its offers go, not just the first. A registry that removed one
    // entry would leave a capability pointing at an unloaded plugin, which is
    // the "deleted plugin's node type still exists" defect.
    REQUIRE(reg.size() == 1);
    REQUIRE_FALSE(reg.available("qp.model.a"));
    REQUIRE_FALSE(reg.available("qp.model.b"));
    REQUIRE(reg.provider_of("qp.model.a") == nullptr);
    REQUIRE_FALSE(reg.is_registered("org.example.multi"));

    // The other plugin is untouched.
    REQUIRE(reg.available("qp.io.export_dataset"));
    REQUIRE(reg.provider_of("qp.io.export_dataset") == &other);
    REQUIRE(reg.plugin_ids() == std::vector<std::string>{"org.example.other"});

    // Unloading something that was never loaded is reported, not ignored: a
    // silent success would hide a double-unload bug in the host.
    REQUIRE_FALSE(reg.unload("org.example.multi").has_value());
    REQUIRE_FALSE(reg.unload("org.example.never").has_value());
    REQUIRE(reg.size() == 1);

    // The freed ids become available again, so a reload succeeds.
    REQUIRE(reg.negotiate(multi, Capability::node_types) == NegotiationVerdict::granted);
    REQUIRE(reg.available("qp.model.a"));
    REQUIRE(reg.size() == 3);
}
