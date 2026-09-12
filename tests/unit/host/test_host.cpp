/**
 * @file test_host.cpp
 * @brief Tests for the composition root: mounting real plugins, recording what they contribute, taking it back.
 *
 * ## What these tests map, and why that is the point
 *
 * Every other test in the suite works on a value. These map real `MODULE` libraries and let them call back into
 * the host, because the properties under test cannot be observed from a value:
 *
 *   - a plugin's contributions actually land in the host's registries;
 *   - a refusal happens **before** `register_into`, which is only visible from host-owned memory;
 *   - unloading removes what the plugin registered even when the plugin's own `unregister` does nothing.
 *
 * The fixtures live in `tests/fixtures/host_plugin/`, one deliberate deviation each, and they link `qp::host`
 * -- unlike the loader's fixtures, which must be able to disagree with the host about the ABI. See that
 * directory's header for why the two sets are built differently.
 *
 * ## The property that needed a fixture designed for it
 *
 * `content.cpp`'s `unregister` is empty. Not forgetful -- empty on purpose. The host keeps its own record of
 * every successful `add_*` and withdraws that record itself, and a fixture that cleaned up after itself would
 * make the record untestable: after `unload`, the catalog being empty is the only evidence that the host, and
 * not the plugin, is what makes unloading exact.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/host/host.hpp>

#include <qp/plugin/loader.hpp>
#include <qp/runtime/file/file.hpp>

#include "fixtures/host_plugin/host_plugin_fixture.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace qp;
using qp::test::host_fixture::kContentId;
using qp::test::host_fixture::kEmptyId;
using qp::test::host_fixture::kFormatExtension;
using qp::test::host_fixture::kFormatName;
using qp::test::host_fixture::kHungryId;
using qp::test::host_fixture::kKernelName;
using qp::test::host_fixture::kNeedyId;
using qp::test::host_fixture::kNeedyNodeType;
using qp::test::host_fixture::kNodeType;
using qp::test::host_fixture::kRefusesHalfType;
using qp::test::host_fixture::kRefusesId;
using qp::test::host_fixture::kSecondNodeType;

namespace {

/// @brief The capability set the tests grant: what `content` declares, plus what `needy` declares.
///
/// Deliberately not `kKnownCapabilities`: a host that grants everything cannot exercise a refusal, and the
/// refusal is the half of the gate that a manifest exists for. `file_io` is here because `content` declares it;
/// `view_items` because `needy` does; `field_domain` is withheld and no fixture wants it, which is the state a
/// grant is normally in.
constexpr plugin::Capability kGrant = plugin::Capability::node_types | plugin::Capability::kernels |
                                      plugin::Capability::file_io | plugin::Capability::view_items;

/// @brief Whether a node type is in the catalog, by the name the fixture registered.
[[nodiscard]] bool catalog_has(const host::PluginHost& h, std::string_view type_name) {
    return h.node_types().find(type_name) != nullptr;
}

/// @brief Whether a name appears in a list of ids.
[[nodiscard]] bool contains(const std::vector<std::string>& ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// @brief Whether a refusal list names a plugin.
[[nodiscard]] bool refused_id(const std::vector<host::PluginOutcome>& outcomes, std::string_view id) {
    return std::any_of(outcomes.begin(), outcomes.end(),
                       [id](const host::PluginOutcome& o) { return o.id == id; });
}

/**
 * @brief A directory holding copies of named fixtures, so a scan can be pointed at a controlled set.
 *
 * Copies rather than a scan of the build directory: the build directory also holds the loader's fixtures and
 * will hold whatever the next fixture set adds, so a scan of it would assert on a set this test does not own.
 * A test whose subject is "which files were offered" has to decide which files those are.
 */
class FixtureDir final {
public:
    FixtureDir(const test::TempDir& parent, std::string_view name)
        : root_(parent.path(std::string{name})) {
        std::error_code ec;
        std::filesystem::create_directories(runtime::to_path(root_), ec);
    }

    /// @brief Copies the built fixture library in under `as`.
    void add(std::string_view source_path, std::string_view as) const {
        std::error_code ec;
        std::filesystem::copy_file(runtime::to_path(std::string{source_path}),
                                   runtime::to_path(root_ + "/" + std::string{as}),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
    }

    /// @brief Writes a plain file, so a scan meets something that is not a plugin at all.
    void add_junk(std::string_view as, std::string_view bytes) const {
        const std::string content{bytes};
        REQUIRE(runtime::write_whole_file(root_ + "/" + std::string{as}, content) ==
                runtime::FileOutcome::ok);
    }

    [[nodiscard]] std::string path() const { return root_; }

private:
    std::string root_;
};

/**
 * @brief The file name a fixture library has on this platform.
 *
 * MinGW prefixes a `MODULE` library with `lib` and MSVC does not, so the copy destination is built from the
 * real target file's own name rather than from a guess. Reconstructing a toolchain's output name is testing
 * the guess, which is the mistake `tests/CMakeLists.txt` records for the loader's fixtures.
 */
[[nodiscard]] std::string name_of(std::string_view built_path) {
    const std::string full{built_path};
    const std::size_t slash = full.find_last_of("/\\");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

}  // namespace

TEST_CASE("host.loads_a_real_plugin_and_mounts_what_it_registers", "[host]") {
    host::PluginHost h{kGrant};
    const host::PluginOutcome outcome = h.load(QP_HOST_FIXTURE_content);

    REQUIRE(outcome.ok());
    REQUIRE(outcome.id == kContentId);
    REQUIRE(outcome.name == "Host Fixture Content");
    // Two node types, one kernel, one export format. Counted rather than asserted as a floor: a host that
    // recorded one registration per plugin would pass a floor and lose three contributions.
    REQUIRE(outcome.contributions == 4);

    // The content, in the three registries it belongs to.
    CHECK(catalog_has(h, kNodeType));
    CHECK(catalog_has(h, kSecondNodeType));
    REQUIRE(h.node_types().find(kNodeType) != nullptr);
    CHECK(h.node_types().find(kNodeType)->label == "Fixture Spinner");
    // The port the fixture declared survives the trip, which is what makes the descriptor useful rather than
    // merely present.
    REQUIRE(h.node_types().find(kNodeType)->inputs.size() == 1);
    CHECK(h.node_types().find(kNodeType)->inputs.front().name == "value");

    CHECK(h.kernels().find_by_name(kKernelName) != nullptr);
    CHECK(h.formats().find_by_name(kFormatName) != nullptr);
    CHECK(h.formats().find_by_extension(kFormatExtension) != nullptr);

    // Attribution comes from the host's record, not from the descriptor's `source` field, which the plugin
    // filled in and could have got wrong.
    CHECK(h.origin_of(kNodeType) == kContentId);
    CHECK(h.origin_of("nothing.registered.this").empty());

    // The capability registry knows the plugin under the host's id, and the grant is the host's.
    CHECK(h.capabilities().is_registered(kContentId));
    CHECK(h.capabilities().available("qp.plugin.node_types"));
    CHECK(h.capabilities().available("qp.plugin.kernels"));
    CHECK(h.capabilities().available("qp.plugin.file_io"));
    CHECK_FALSE(h.capabilities().available("qp.plugin.field_domain"));

    CHECK(contains(h.mounted_ids(), kContentId));
}

TEST_CASE("host.unload_takes_the_contributions_back", "[host]") {
    host::PluginHost h{kGrant};
    REQUIRE(h.load(QP_HOST_FIXTURE_content).ok());
    REQUIRE(h.node_types().size() == 2);

    REQUIRE(h.unload(kContentId).has_value());

    // Every registry, and the capability registry too. This is the case the fixture's empty `unregister` exists
    // for: nothing but the host's own record removed these.
    CHECK(h.node_types().size() == 0);
    CHECK_FALSE(catalog_has(h, kNodeType));
    CHECK_FALSE(catalog_has(h, kSecondNodeType));
    CHECK(h.kernels().find_by_name(kKernelName) == nullptr);
    CHECK(h.kernels().size() == 0);
    CHECK(h.formats().find_by_name(kFormatName) == nullptr);
    CHECK(h.formats().size() == 0);
    CHECK_FALSE(h.capabilities().is_registered(kContentId));
    CHECK(h.capabilities().size() == 0);
    CHECK(h.mounted_ids().empty());
    CHECK(h.origin_of(kNodeType).empty());

    // Unloading a plugin that is not mounted is reported rather than ignored: a caller asking to remove
    // something is entitled to know it was not there.
    const diag::Result<void> again = h.unload(kContentId);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == diag::ErrorCode::unknown_node);
}

TEST_CASE("host.refuses_a_capability_it_did_not_grant", "[host]") {
    // `file_io` is withheld. `hungry`'s manifest declares it, so the refusal has to happen before its code
    // runs -- and the fixture writes into host-owned memory when it is entered, so "before" is observable
    // rather than argued.
    host::PluginHost h{plugin::Capability::node_types};
    test::host_fixture::Recording recording;
    const char* order[4] = {nullptr, nullptr, nullptr, nullptr};
    recording.order = order;
    recording.capacity = 4;

    const host::PluginOutcome outcome = h.load(QP_HOST_FIXTURE_hungry);
    REQUIRE_FALSE(outcome.ok());
    CHECK(outcome.code == diag::ErrorCode::plugin_capability_missing);
    CHECK(outcome.id == kHungryId);
    // The sentence names the bit, because "refused" without the reason sends the user to the wrong file.
    CHECK(outcome.detail.find("file_io") != std::string::npos);
    CHECK(outcome.contributions == 0);
    CHECK(h.mounted_ids().empty());
    CHECK(h.capabilities().size() == 0);
}

TEST_CASE("host.reports_a_plugins_own_reason_for_refusing", "[host]") {
    host::PluginHost h{kGrant};
    const host::PluginOutcome outcome = h.load(QP_HOST_FIXTURE_refuses);

    REQUIRE_FALSE(outcome.ok());
    CHECK(outcome.id == kRefusesId);
    // The plugin's own sentence, not a code. This is what `IPluginHost::refuse` exists for.
    CHECK(outcome.detail.find("the fixture's bench supply is not connected") != std::string::npos);
    // And the half it installed before refusing is gone: a refusal that leaves a node type behind is the state
    // the ledger exists to undo.
    CHECK(h.node_types().size() == 0);
    CHECK_FALSE(catalog_has(h, kRefusesHalfType));
    CHECK(h.mounted_ids().empty());
}

TEST_CASE("host.reports_every_failure_with_a_reason", "[host]") {
    host::PluginHost h{kGrant};

    const host::PluginOutcome mounted = h.load(QP_HOST_FIXTURE_content);
    REQUIRE(mounted.ok());

    // A manifest that declares no capability: `judge` refuses it, so the host never maps what it would have
    // contributed.
    const host::PluginOutcome empty = h.load(QP_HOST_FIXTURE_empty);
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.id == kEmptyId);
    CHECK_FALSE(empty.detail.empty());

    // The same plugin twice. The second attempt is refused rather than quietly returning the first one's
    // answer, because the caller asked for a second mount and did not get one.
    const host::PluginOutcome twice = h.load(QP_HOST_FIXTURE_content);
    REQUIRE_FALSE(twice.ok());
    CHECK(twice.detail.find("already mounted") != std::string::npos);

    // A path that names nothing: the sentence says so instead of naming a plugin that failed to install.
    const host::PluginOutcome absent = h.load("no/such/directory/plugin.dll");
    REQUIRE_FALSE(absent.ok());
    CHECK(absent.detail == "no such file");

    // Every attempt is reported, one line each, and the report names the reason rather than a code.
    host::LoadReport report;
    report.mounted_plugins.push_back(mounted);
    report.refused.push_back(empty);
    report.refused.push_back(twice);
    CHECK(report.attempts() == 3);
    CHECK_FALSE(report.all_mounted());
    const std::string text = report.to_text();
    CHECK(text.find("mounted " + std::string{kContentId}) != std::string::npos);
    CHECK(text.find("refused " + std::string{kEmptyId}) != std::string::npos);

    // The three failures are three different reasons, which is the property that makes a log worth reading.
    CHECK(empty.detail != twice.detail);
    CHECK(empty.detail != absent.detail);
    CHECK(empty.code != twice.code);
}

TEST_CASE("host.mounts_a_dependent_after_its_dependency", "[host]") {
    // `needy` declares a dependency on `content` and refuses to install unless `content` is already mounted.
    // The manifest gives the loader the ids; only the host can say whether the dependency is really there.
    const test::TempDir tmp{"qp_host_order"};
    const FixtureDir dir{tmp, "both"};
    // Copied in reverse alphabetical order, so the file order is the opposite of the load order: a host that
    // installed in directory order would enter `needy` first and fail.
    dir.add(QP_HOST_FIXTURE_needy, "zz_" + name_of(QP_HOST_FIXTURE_needy));
    dir.add(QP_HOST_FIXTURE_content, "aa_" + name_of(QP_HOST_FIXTURE_content));

    host::PluginHost h{kGrant};
    const host::LoadReport report = h.load_directory(dir.path());

    INFO(report.to_text());
    REQUIRE(report.all_mounted());
    REQUIRE(report.attempts() == 2);
    // Install order, as the mount table reports it, is the dependency order and not the file order.
    REQUIRE(h.mounted_ids().size() == 2);
    CHECK(h.mounted_ids()[0] == kContentId);
    CHECK(h.mounted_ids()[1] == kNeedyId);
    CHECK(catalog_has(h, kNeedyNodeType));

    // And the loaded set unloads cleanly, newest first: nothing the dependency provided is needed by the
    // dependent once it is gone, which is why `unload` takes one id at a time.
    REQUIRE(h.unload(kNeedyId).has_value());
    REQUIRE(h.unload(kContentId).has_value());
    CHECK(h.node_types().size() == 0);
    CHECK(h.kernels().size() == 0);
    CHECK(h.formats().size() == 0);
}

TEST_CASE("host.a_builtin_type_is_attributed_and_removable", "[host]") {
    // A built-in is this build's own content: the editor's demonstrator types. It goes through the same record a
    // plugin's contribution does, which is what makes `origin_of` able to answer for it and `unload` able to
    // take it back -- a second registration path would be the one hole in the record.
    host::PluginHost h{kGrant};

    qp::graph::NodeDesc builtin;
    builtin.type_name = "builtin.demo";
    builtin.label = "Built-in Demo";
    builtin.category = "fixture";
    REQUIRE(h.add_builtin_node_type(builtin) == diag::ErrorCode::ok);
    CHECK(catalog_has(h, "builtin.demo"));
    CHECK(h.origin_of("builtin.demo") == host::PluginHost::kBuiltinOrigin);

    // Not a plugin: it has no manifest, no library and no capability declaration, so listing it among the
    // mounted plugins would answer a question nobody asked.
    CHECK(h.mounted_ids().empty());
    CHECK(h.capabilities().size() == 0);

    // A plugin's type and a built-in's are attributed to different origins, which is the whole reason the
    // built-in path exists rather than the window registering into the registry directly.
    REQUIRE(h.load(QP_HOST_FIXTURE_content).ok());
    CHECK(h.origin_of(kNodeType) == kContentId);
    CHECK(h.origin_of("builtin.demo") == host::PluginHost::kBuiltinOrigin);

    // A duplicate is refused by the same rule a plugin's duplicate meets, because it is the same registry.
    qp::graph::NodeDesc clash;
    clash.type_name = "builtin.demo";
    CHECK(h.add_builtin_node_type(clash) == diag::ErrorCode::duplicate_connection);

    h.clear_builtin_node_types();
    CHECK_FALSE(catalog_has(h, "builtin.demo"));
    CHECK(h.origin_of("builtin.demo").empty());
    // And only the built-ins: clearing them must not touch a plugin's contributions.
    CHECK(catalog_has(h, kNodeType));
    CHECK(contains(h.mounted_ids(), kContentId));
}

TEST_CASE("host.mounts_and_refuses_the_loaders_own_fixtures", "[host]") {
    // End to end with the fixtures this repository already had, and they are the right subject for a reason
    // that is easy to miss: `tests/fixtures/plugin/` is built from **hand-written bytes** and deliberately does
    // not link `qp::host`, so those libraries are the only plugins here that the host did not build itself.
    //
    // `good` is a well-formed plugin whose `register_into` returns ok and registers nothing -- which is a
    // legitimate plugin, and the host has to say so rather than reporting a failure or inventing a contribution.
    {
        host::PluginHost h{kGrant};
        const host::PluginOutcome good = h.load(QP_LOADER_FIXTURE_good);
        INFO(good.detail);
        REQUIRE(good.ok());
        CHECK(good.id == "org.qp.fixture.good");
        CHECK(good.name == "Fixture Good");
        // Zero contributions, and reported as zero rather than omitted: "it loaded" and "it did something" are
        // different claims, and a plugin that exists only to be depended on is a real arrangement.
        CHECK(good.contributions == 0);
        CHECK(contains(h.mounted_ids(), "org.qp.fixture.good"));
        // It declared `node_types`, so it is a capability provider -- the host synthesises that from the
        // manifest, which is the only thing a dynamically loaded plugin can offer today.
        CHECK(h.capabilities().is_registered("org.qp.fixture.good"));
        CHECK(h.capabilities().available("qp.plugin.node_types"));
    }

    // A directory scan over a real, unmodified plugin library. One file, in its own directory: the set has to be
    // unambiguous for the reasons the next block makes concrete.
    {
        const test::TempDir tmp{"qp_host_loader"};
        const FixtureDir dir{tmp, "real"};
        dir.add(QP_LOADER_FIXTURE_good, name_of(QP_LOADER_FIXTURE_good));

        host::PluginHost h{kGrant};
        const host::LoadReport report = h.load_directory(dir.path(), "dll");
        INFO(report.to_text());
        REQUIRE(report.all_mounted());
        CHECK(report.attempts() == 1);
        REQUIRE(h.mounted_ids().size() == 1);
        CHECK(h.mounted_ids()[0] == "org.qp.fixture.good");
        CHECK(h.unload("org.qp.fixture.good").has_value());
    }

    // Two of the loader's well-formed fixtures **cannot both mount**, and this is the sharpest thing the real
    // fixtures taught: `good` and `second` are both correct plugins and both declare `node_types`, and the
    // capability registry serves one provider per capability id. So the second is refused with
    // `duplicate_provider` even though its own manifest is fine and its dependency is present.
    //
    // That is a real constraint on plugin authors rather than a defect to paper over -- "who provides this" has
    // to have one answer -- and it is asserted here, in a case built from libraries nobody wrote for it, because
    // a rule that only appears when two plugins meet is a rule nobody learns from a unit test of one.
    {
        host::PluginHost h{kGrant};
        REQUIRE(h.load(QP_LOADER_FIXTURE_good).ok());
        const host::PluginOutcome second = h.load(QP_LOADER_FIXTURE_second);
        INFO(second.detail);
        REQUIRE_FALSE(second.ok());
        CHECK(second.id == "org.qp.fixture.second");
        CHECK(second.detail.find("duplicate_provider") != std::string::npos);
        // And the refusal left nothing behind: the first plugin is untouched and the second is not mounted.
        CHECK(h.mounted_ids().size() == 1);
        CHECK(contains(h.mounted_ids(), "org.qp.fixture.good"));
    }

    // And the loader's malformed libraries, through the host: a missing entry point, a wrong tag and a wrong
    // exports version each become one sentence naming the reason, and the host never maps what it would have
    // contributed.
    {
        host::PluginHost h{kGrant};
        const host::PluginOutcome no_entry = h.load(QP_LOADER_FIXTURE_no_entry);
        REQUIRE_FALSE(no_entry.ok());
        CHECK(no_entry.detail.find("qp_plugin_entry") != std::string::npos);

        const host::PluginOutcome bad_tag = h.load(QP_LOADER_FIXTURE_bad_tag);
        REQUIRE_FALSE(bad_tag.ok());
        CHECK(bad_tag.detail.find("tag") != std::string::npos);

        const host::PluginOutcome bad_version = h.load(QP_LOADER_FIXTURE_bad_version);
        REQUIRE_FALSE(bad_version.ok());
        // Not readable as a plugin: the exports version is checked before any pointer field is followed, so the
        // manifest was never converted. The file name stands in as the identity, which is the honest answer --
        // "this file" rather than a plugin name the host could not have read.
        CHECK(bad_version.id == name_of(QP_LOADER_FIXTURE_bad_version));
        CHECK(bad_version.detail.find("export block version") != std::string::npos);
        CHECK(h.mounted_ids().empty());
    }
}

TEST_CASE("host.scanning_a_directory_mounts_what_it_can_and_reports_the_rest", "[host]") {
    const test::TempDir tmp{"qp_host_scan"};

    SECTION("a directory that is not there is not an error") {
        host::PluginHost h{kGrant};
        const host::LoadReport report = h.load_directory(tmp.path("absent"));
        CHECK(report.attempts() == 0);
        CHECK(report.all_mounted());
        CHECK(h.mounted_ids().empty());
    }

    SECTION("a clean set mounts as a whole") {
        const FixtureDir dir{tmp, "clean"};
        dir.add(QP_HOST_FIXTURE_content, name_of(QP_HOST_FIXTURE_content));
        // Not a plugin library, and named so that it sorts first: the extension filter is what keeps it out,
        // because a scan that offered every file would report every stray file as a broken plugin.
        dir.add_junk("notes.txt", "a text file is not a plugin");

        host::PluginHost h{kGrant};
        const host::LoadReport report = h.load_directory(dir.path(), "dll");
        INFO(report.to_text());
        REQUIRE(report.all_mounted());
        CHECK(report.attempts() == 1);
        CHECK(catalog_has(h, kNodeType));
        CHECK(contains(h.mounted_ids(), kContentId));
    }

    SECTION("one broken candidate refuses the whole set, and every failure is reported") {
        const FixtureDir dir{tmp, "broken"};
        dir.add(QP_HOST_FIXTURE_content, name_of(QP_HOST_FIXTURE_content));
        dir.add(QP_HOST_FIXTURE_empty, name_of(QP_HOST_FIXTURE_empty));
        dir.add(QP_HOST_FIXTURE_hungry, name_of(QP_HOST_FIXTURE_hungry));

        host::PluginHost h{kGrant};
        const host::LoadReport report = h.load_directory(dir.path(), "dll");
        INFO(report.to_text());
        CHECK_FALSE(report.all_mounted());
        // All three, not just the first: the user fixes their plugin directory in one pass.
        CHECK(report.attempts() == 3);
        CHECK(report.mounted_plugins.empty());
        REQUIRE(report.refused.size() == 3);
        CHECK(refused_id(report.refused, kEmptyId));
        CHECK(refused_id(report.refused, kHungryId));
        CHECK(refused_id(report.refused, kContentId));

        // Nothing is left mounted, and no registry holds a trace of the plugins that were mapped.
        CHECK(h.mounted_ids().empty());
        CHECK(h.node_types().size() == 0);
        CHECK(h.kernels().size() == 0);
        CHECK(h.formats().size() == 0);
        CHECK(h.capabilities().size() == 0);
    }

    SECTION("a plugin that cannot work alone takes the set down with it") {
        // `needy` refuses without `content`, so a directory holding only `needy` is a set that must not mount
        // half of anything -- and the refusal is the plugin's own sentence rather than a host guess.
        const FixtureDir dir{tmp, "needy_alone"};
        dir.add(QP_HOST_FIXTURE_needy, name_of(QP_HOST_FIXTURE_needy));

        host::PluginHost h{kGrant};
        const host::LoadReport report = h.load_directory(dir.path(), "dll");
        INFO(report.to_text());
        CHECK_FALSE(report.all_mounted());
        REQUIRE(report.refused.size() == 1);
        CHECK(report.refused.front().id == kNeedyId);
        CHECK(report.refused.front().detail.find("was not mounted before it") != std::string::npos);
        CHECK(h.mounted_ids().empty());
    }
}
