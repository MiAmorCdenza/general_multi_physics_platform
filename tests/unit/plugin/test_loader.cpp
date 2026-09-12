/**
 * @file test_loader.cpp
 * @brief Tests for the plugin loader: mapping, refusal, ordering, and unload order.
 *
 * ## What makes these tests different from the rest of the suite
 *
 * Every other unit test in this repository is a pure function over a value. These
 * map real shared libraries into the test process, because the loader's entire
 * purpose is the part that cannot be tested from a value: whether the ABI two
 * separately-built binaries agree on is the ABI they both think they have.
 *
 * The fixtures live under tests/fixtures/plugin/ and are built as `MODULE`
 * libraries. Each one is a single deliberate deviation from a working plugin, so a
 * failing test names one defect rather than "a fixture is broken".
 *
 * ## The two properties worth stating outright
 *
 * **Refusal happens before installation.** The fixtures prove it by being unable to
 * install anything: a plugin that fails judgement never reaches its own
 * `register_into`, and the counter fixture confirms the call count is zero.
 *
 * **Unregister runs before the library is released.** This leaves no trace in the
 * final state -- both orders end with the same counters and the same registries --
 * so it is only observable through the host-owned counter. It is also the property
 * whose violation reads unmapped memory rather than failing a test, which is why it
 * gets a test at all.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugin.hpp>

#include "support/plugin_fixture_abi.hpp"

// <algorithm> is here because `std::any_of` and `std::find` are used below. GCC
// accepted the file without it -- libstdc++ pulls the algorithms in through other
// headers -- and MSVC refused it with C2039. That is the compiler matrix doing its
// job: an include that is present by accident compiles until the day a library
// header reorganises, and then it breaks for whoever upgrades, not for whoever
// omitted it.
#include <algorithm>
#include <string>
#include <vector>

using namespace qp::plugin;
using qp::test::fixture::Counter;

namespace {

/**
 * @brief The built path of one fixture, supplied by the build system.
 *
 * The build system knows the real filename; this test does not. MinGW prefixes a
 * MODULE library with `lib` and MSVC does not, so reconstructing the name from a
 * stem would be a guess about the toolchain -- and a wrong guess presents as
 * `file_missing`, which reads like a fixture that was never built.
 */
#define QP_FIXTURE(name) QP_FIXTURE_##name

/// Loads one fixture, asserting nothing: the caller inspects the outcome.
LoadOutcome try_load(const char* path, void* ctx = nullptr) {
    LoadedPlugin plugin;
    LoadOutcome outcome = load_plugin(path, ctx, &plugin);
    // Drain on success so a test that only meant to inspect an outcome cannot leak
    // a mapping into the next case -- the fixtures all share one process.
    if (plugin.valid()) plugin.unload();
    return outcome;
}

}  // namespace

TEST_CASE("plugin.loader.reports_distinct_failures", "[plugin][loader]") {
    // The failure enum exists so the log can say *which* mistake was made. A loader
    // that collapsed every cause into one code would pass every other test in this
    // file, so the distinctness is itself the property under test.
    REQUIRE(std::string(to_string(LoadFailure::ok)) !=
            std::string(to_string(LoadFailure::library_unmappable)));
    REQUIRE(std::string(to_string(LoadFailure::wrong_tag)) == "wrong_tag");
    REQUIRE(std::string(to_string(LoadFailure::manifest_rejected)) == "manifest_rejected");

    // Every enumerator gets a name, and no two share one: a duplicate would make the
    // log ambiguous exactly when someone is reading it to find out what happened.
    const LoadFailure all[] = {
        LoadFailure::ok,           LoadFailure::file_missing,
        LoadFailure::library_unmappable, LoadFailure::entry_point_missing,
        LoadFailure::plugin_declined,    LoadFailure::wrong_tag,
        LoadFailure::wrong_exports_version, LoadFailure::manifest_missing,
        LoadFailure::unregister_missing, LoadFailure::manifest_rejected,
        LoadFailure::registration_failed};
    for (const LoadFailure a : all) {
        for (const LoadFailure b : all) {
            if (a == b) continue;
            REQUIRE(std::string(to_string(a)) != std::string(to_string(b)));
        }
        REQUIRE(std::string(to_string(a)) != "unknown");
    }
}

TEST_CASE("plugin.loader.missing_file_is_distinct_from_unmappable", "[plugin][loader]") {
    // Two causes, two codes. "No such file" is a packaging mistake -- the plugin was
    // never copied, or is in the wrong directory -- and "the OS refused to map it" is
    // a build mistake, usually architecture or a missing dependency. Reporting the
    // first as the second sends the reader to rebuild something that is fine.
    // The missing path is built by appending to a directory that **does** exist, so
    // this cannot pass because the whole fixture directory is absent.
    const std::string real = QP_FIXTURE(good);
    const std::string absent_path =
        real.substr(0, real.find_last_of("/\\") + 1) + "qp_no_such_plugin_file.dll";

    LoadedPlugin plugin;
    const LoadOutcome absent = load_plugin(absent_path, nullptr, &plugin);
    REQUIRE(absent.failure == LoadFailure::file_missing);
    REQUIRE(absent.failure != LoadFailure::library_unmappable);
    REQUIRE(absent.path == absent_path);
    REQUIRE_FALSE(plugin.valid());
}

TEST_CASE("plugin.loader.refuses_missing_entry", "[plugin][loader]") {
    // A library that exists and maps, but exports a misspelled entry point. The
    // lookup must match the exact name: a prefix match would accept this fixture and
    // then call a function of the wrong signature.
    const LoadOutcome outcome = try_load(QP_FIXTURE(no_entry));
    REQUIRE(outcome.failure == LoadFailure::entry_point_missing);
    // No manifest was reachable, so the report must not pretend to have one.
    REQUIRE_FALSE(outcome.has_manifest);
}

TEST_CASE("plugin.loader.refuses_wrong_tag", "[plugin][loader]") {
    // Everything after the tag is a pointer. A host that trusted the shape would read
    // `manifest` out of bytes that are not a manifest and then follow it.
    const LoadOutcome outcome = try_load(QP_FIXTURE(bad_tag));
    REQUIRE(outcome.failure == LoadFailure::wrong_tag);
    // The tag is checked before the manifest pointer is touched, so nothing was read:
    // an implementation that copied the manifest first would report the identity here.
    REQUIRE_FALSE(outcome.has_manifest);
}

TEST_CASE("plugin.loader.refuses_wrong_exports_version", "[plugin][loader]") {
    // A plugin built from a newer header than the host's. Reading the fields the host
    // thinks it knows is wrong, because a grown block may have moved them.
    const LoadOutcome outcome = try_load(QP_FIXTURE(bad_version));
    REQUIRE(outcome.failure == LoadFailure::wrong_exports_version);
    REQUIRE_FALSE(outcome.has_manifest);
}

TEST_CASE("plugin.loader.refuses_missing_manifest", "[plugin][loader]") {
    const LoadOutcome outcome = try_load(QP_FIXTURE(no_manifest));
    REQUIRE(outcome.failure == LoadFailure::manifest_missing);
}

TEST_CASE("plugin.loader.refuses_missing_unregister", "[plugin][loader]") {
    // The least obvious rule in the loader, so it gets its own case. The plugin
    // works; the problem is that accepting it would make the mapping impossible to
    // ever release, because registries would still hold pointers into it.
    const LoadOutcome outcome = try_load(QP_FIXTURE(no_unregister));
    REQUIRE(outcome.failure == LoadFailure::unregister_missing);
}

TEST_CASE("plugin.loader.refuses_declining_plugin", "[plugin][loader]") {
    // A null return is a decision, not a defect, and must be distinguishable from a
    // missing symbol: one is fixed by rebuilding, the other by changing the host.
    const LoadOutcome outcome = try_load(QP_FIXTURE(declines));
    REQUIRE(outcome.failure == LoadFailure::plugin_declined);
    REQUIRE(outcome.failure != LoadFailure::entry_point_missing);
}

TEST_CASE("plugin.loader.refuses_incompatible_manifest", "[plugin][loader]") {
    // The structurally perfect block whose manifest has no identity. Nothing in
    // `inspect_exports` can catch this, so it isolates `judge` from the shape checks:
    // if this case passes for the wrong reason, `judge` is not being called at all.
    const LoadOutcome outcome = try_load(QP_FIXTURE(rejects));
    REQUIRE(outcome.failure == LoadFailure::manifest_rejected);
    // The manifest was read before judgement, and that is deliberate: a rejected
    // plugin is exactly the case where the user needs to be told which one it was.
    REQUIRE(outcome.has_manifest);
    REQUIRE(outcome.manifest.id.empty());
}

TEST_CASE("plugin.loader.loads_and_unloads", "[plugin][loader]") {
    const std::string path = QP_FIXTURE(good);
    LoadedPlugin plugin;
    const LoadOutcome outcome = load_plugin(path, nullptr, &plugin);

    REQUIRE(outcome.ok());
    REQUIRE(outcome.failure == LoadFailure::ok);
    REQUIRE(outcome.has_manifest);
    REQUIRE(outcome.manifest.id == "org.qp.fixture.good");
    REQUIRE(outcome.manifest.name == "Fixture Good");

    REQUIRE(plugin.valid());
    REQUIRE(plugin.path() == path);
    REQUIRE(plugin.manifest().id == "org.qp.fixture.good");

    // Mapping and judging do not install: installation order comes from the
    // manifests, so `load_plugin` must leave the host untouched.
    REQUIRE_FALSE(plugin.contributes());

    REQUIRE(plugin.install(nullptr) == qp::diag::ErrorCode::ok);
    REQUIRE(plugin.contributes());

    // Idempotent: a second install is a no-op rather than a second registration,
    // because a host that installs twice would register a kernel under a name that
    // is already taken and refuse the plugin for its own host's mistake.
    REQUIRE(plugin.install(nullptr) == qp::diag::ErrorCode::ok);
    REQUIRE(plugin.contributes());

    plugin.unload();
    REQUIRE_FALSE(plugin.valid());
    REQUIRE_FALSE(plugin.contributes());
    // Idempotent again, and this is the path the destructor takes on a moved-from or
    // already-unloaded object.
    plugin.unload();
    REQUIRE_FALSE(plugin.valid());
}

TEST_CASE("plugin.loader.unload_unregisters_first", "[plugin][loader]") {
    // The ordering property. Both orders reach the same final state, so it is only
    // visible through the counter -- and the wrong order reads unmapped memory rather
    // than failing, which is exactly why it needs a test rather than a comment.
    Counter counter;
    LoadedPlugin plugin;
    REQUIRE(load_plugin(QP_FIXTURE(counting), &counter, &plugin).ok());
    REQUIRE(plugin.install(&counter) == qp::diag::ErrorCode::ok);
    REQUIRE(counter.registered == 1);
    REQUIRE(counter.unregistered == 0);

    plugin.unload();
    // One unregister call, and it happened while the library was still mapped. If the
    // implementation released the library first, this counter would not have been
    // incremented -- or the process would have faulted trying.
    REQUIRE(counter.unregistered == 1);
}

TEST_CASE("plugin.loader.failed_registration_leaves_nothing", "[plugin][loader]") {
    // A plugin whose registration fails part way. The host cannot know how far it got,
    // so it must call `unregister` anyway -- which is why the contract requires that
    // callback to be idempotent and safe on partial state.
    Counter counter;
    counter.fail_registration = 1;

    LoadedPlugin plugin;
    REQUIRE(load_plugin(QP_FIXTURE(counting), &counter, &plugin).ok());
    REQUIRE(plugin.install(&counter) != qp::diag::ErrorCode::ok);

    // Register ran and failed; unregister ran anyway to undo whatever it managed.
    REQUIRE(counter.registered == 1);
    REQUIRE(counter.unregistered == 1);
    // A failed install must not leave the plugin claiming to contribute, or the
    // caller would treat a half-installed plugin as live.
    REQUIRE_FALSE(plugin.contributes());

    // And unloading it must not unregister a second time on the strength of a
    // registration that never succeeded.
    plugin.unload();
    REQUIRE(counter.unregistered == 1);
}

TEST_CASE("plugin.loader.moves_transfer_ownership_once", "[plugin][loader]") {
    // Move semantics are part of the safety argument, not a convenience: two objects
    // holding one library handle means two destructors unregistering the same
    // registration. The moved-from object must end up empty.
    LoadedPlugin first;
    REQUIRE(load_plugin(QP_FIXTURE(good), nullptr, &first).ok());
    REQUIRE(first.valid());

    LoadedPlugin second = std::move(first);
    REQUIRE(second.valid());
    REQUIRE_FALSE(first.valid());
    REQUIRE(second.manifest().id == "org.qp.fixture.good");

    // The moved-from object's destructor must not release the library the live object
    // still owns -- which is why `unload` on an invalid object returns immediately.
    first.unload();
    REQUIRE(second.valid());

    second.unload();
    REQUIRE_FALSE(second.valid());
}

TEST_CASE("plugin.loader.move_assignment_releases_the_old_library", "[plugin][loader]") {
    // Assigning over a live plugin is the case that leaks if the target is not
    // unloaded first: the only handle to the old library is overwritten.
    Counter counter;
    LoadedPlugin target;
    REQUIRE(load_plugin(QP_FIXTURE(counting), &counter, &target).ok());
    REQUIRE(target.install(&counter) == qp::diag::ErrorCode::ok);
    REQUIRE(counter.registered == 1);

    LoadedPlugin source;
    REQUIRE(load_plugin(QP_FIXTURE(good), nullptr, &source).ok());

    target = std::move(source);
    // The old occupant was unloaded, so its unregister ran. Without the unload in the
    // assignment operator the counter would still read zero and the library would
    // stay mapped with live registrations.
    REQUIRE(counter.unregistered == 1);
    REQUIRE(target.valid());
    REQUIRE(target.manifest().id == "org.qp.fixture.good");
    REQUIRE_FALSE(source.valid());

    // Self-assignment must not unload the object out from under itself.
    LoadedPlugin& alias = target;
    target = std::move(alias);
    REQUIRE(target.valid());
}

TEST_CASE("plugin.loader.load_set_is_ordered", "[plugin][loader]") {
    // The second fixture depends on the first, and the caller lists them in the wrong
    // order. Load order must come from the manifests: directory order is not
    // dependency order, and on Windows it is not even alphabetical.
    const LoadReport report = load_plugins({QP_FIXTURE(second), QP_FIXTURE(good)}, nullptr);

    REQUIRE(report.all_ok());
    REQUIRE(report.requested == 2);
    REQUIRE(report.plugins.size() == 2);
    REQUIRE(report.first_failure() == nullptr);

    // `good` first, despite being listed second.
    REQUIRE(report.plugins[0].manifest().id == "org.qp.fixture.good");
    REQUIRE(report.plugins[1].manifest().id == "org.qp.fixture.second");
    // And both are installed, because `load_plugins` installs rather than only mapping.
    REQUIRE(report.plugins[0].contributes());
    REQUIRE(report.plugins[1].contributes());

    // The outcomes report every input, in input order, so a caller can line them up
    // with what it asked for.
    REQUIRE(report.outcomes.size() == 2);
    REQUIRE(report.outcomes[0].path == QP_FIXTURE(second));
    REQUIRE(report.outcomes[1].path == QP_FIXTURE(good));
}

TEST_CASE("plugin.loader.load_set_is_all_or_nothing", "[plugin][loader]") {
    // One broken plugin, one good one. The report must name the failure **and** leave
    // nothing installed.
    //
    // The reason is the platform's whole purpose applied to plugins: with a dependency
    // missing, a graph built against it produces numbers that are wrong rather than
    // absent, and the user watches a simulation run to completion. No session is
    // better than a session that lies.
    const LoadReport report = load_plugins({QP_FIXTURE(good), QP_FIXTURE(no_entry)}, nullptr);

    REQUIRE_FALSE(report.all_ok());
    REQUIRE(report.plugins.empty());
    REQUIRE(report.requested == 2);
    // Every outcome is reported, not just the first: the user fixes their plugin
    // directory in one pass instead of one rebuild per plugin.
    REQUIRE(report.outcomes.size() == 2);

    const LoadOutcome* failure = report.first_failure();
    REQUIRE(failure != nullptr);
    REQUIRE(failure->failure == LoadFailure::entry_point_missing);
    REQUIRE(failure->path == QP_FIXTURE(no_entry));

    // The good plugin is named too, with its manifest, so a reader can see that it
    // was fine and that the set failed because of its neighbour.
    const bool good_reported =
        std::any_of(report.outcomes.begin(), report.outcomes.end(), [](const LoadOutcome& o) {
            return o.ok() && o.manifest.id == "org.qp.fixture.good";
        });
    REQUIRE(good_reported);
}

TEST_CASE("plugin.loader.reports_every_failure_not_just_the_first", "[plugin][loader]") {
    // Three broken plugins of three different kinds. A loader that stopped at the
    // first failure would report one of them and leave the user to rebuild twice more.
    const LoadReport report = load_plugins(
        {QP_FIXTURE(bad_tag), QP_FIXTURE(no_manifest), QP_FIXTURE(no_unregister)}, nullptr);

    REQUIRE_FALSE(report.all_ok());
    REQUIRE(report.plugins.empty());
    REQUIRE(report.outcomes.size() == 3);

    std::vector<LoadFailure> seen;
    for (const LoadOutcome& o : report.outcomes) seen.push_back(o.failure);
    REQUIRE(std::find(seen.begin(), seen.end(), LoadFailure::wrong_tag) != seen.end());
    REQUIRE(std::find(seen.begin(), seen.end(), LoadFailure::manifest_missing) != seen.end());
    REQUIRE(std::find(seen.begin(), seen.end(), LoadFailure::unregister_missing) != seen.end());
}

TEST_CASE("plugin.loader.empty_set_loads_nothing_successfully", "[plugin][loader]") {
    // The boundary case. An empty request succeeded at doing nothing, which is not the
    // same as a failure -- and `all_ok()` on an empty set is vacuously true, which is
    // the correct answer rather than a bug to guard against.
    const LoadReport report = load_plugins({}, nullptr);
    REQUIRE(report.all_ok());
    REQUIRE(report.plugins.empty());
    REQUIRE(report.outcomes.empty());
    REQUIRE(report.requested == 0);
    REQUIRE(report.first_failure() == nullptr);
}
