/**
 * @file test_document_controller.cpp
 * @brief Tests for the file menu's behaviour: new, save, open, and the sentences they produce.
 *
 * ## What is worth testing here
 *
 * This is where three things that must not know about each other meet -- the session's live graph, the
 * document's metadata, and a format's bytes -- so the properties worth asserting are the ones that would
 * otherwise be discovered by losing work:
 *
 *   - **a round trip is a round trip**: what comes back is what went out, node for node, parameter for
 *     parameter, including the per-view layout slots the canvas writes;
 *   - **a failure changes nothing**: a refused save does not claim the path and does not clear the dirty
 *     flag; a failed open leaves the graph, the document and the dirty flag exactly as they were. A
 *     half-applied open is the worst outcome available here, because the user then edits a document that
 *     is not the one on disk;
 *   - **opening is not an edit**: the document is clean afterwards, and the undo stack is empty, because
 *     the previous document's edits do not apply to the new graph (their handles collide with it);
 *   - **dirty follows the session**, not the paths this class happens to know about.
 *
 * The format is the real one (`plugins/formats/qpjson`), reached through the interface the way the window
 * reaches it, and the files land in a temporary directory that removes itself.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/runtime/store/store.hpp>
#include <qp/views/model/document_controller.hpp>

#include <qp/plugins/qpjson/qpjson_format.hpp>

#include <qp/authoring/commands.hpp>
#include <qp/runtime/file/file.hpp>

#include <support/temp_dir.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace qp::views::model;
using qp::test::TempDir;

namespace {

namespace authoring = qp::authoring;
namespace rt = qp::runtime;
using qp::graph::Connect;
using qp::graph::NodeId;
using qp::graph::PortDirection;
using qp::graph::PortRef;
using qp::graph::SetParam;


/// @brief The real JSON document format, one instance for the whole file.
[[nodiscard]] qp::authoring::IDocumentFormat& json_format() {
    static qp::plugins::qpjson::QpJsonFormat format;
    return format;
}

/// @brief A format that refuses everything, for the "nothing may change" cases.
///
/// Deliberately not a real format: it exists so a save can be refused for a *format* reason rather than a
/// file reason, which is the branch a losing-work defect would hide in.
class RefusingFormat final : public qp::authoring::IDocumentFormat {
public:
    [[nodiscard]] const qp::authoring::DocumentFormatDesc& format() const noexcept override {
        static const qp::authoring::DocumentFormatDesc desc = [] {
            qp::authoring::DocumentFormatDesc d;
            d.name = "test.refusing";
            d.label = "Refuses everything";
            d.extensions = {"refuse"};
            d.is_text = true;
            return d;
        }();
        return desc;
    }

    [[nodiscard]] qp::authoring::DocumentRefusal to_bytes(
        const qp::authoring::DocumentSource&, std::string& out) const noexcept override {
        out.clear();
        return qp::authoring::DocumentRefusal::value_kind_not_supported;
    }

    [[nodiscard]] qp::authoring::DocumentRefusal from_bytes(
        std::string_view, qp::authoring::DocumentSnapshot&) const noexcept override {
        return qp::authoring::DocumentRefusal::malformed;
    }
};

/// @brief Adds a node through the command bus and returns its handle.
[[nodiscard]] NodeId add_node(authoring::Session& session, const std::string& type,
                             const std::string& name) {
    const auto reserved = session.reserve_node();
    REQUIRE(reserved.has_value());
    const NodeId id = reserved.value();
    REQUIRE(session.apply(qp::graph::AddNode{id, type, name}).has_value());
    return id;
}

/// @brief A session holding the graph the document tests save and reopen.
///
/// Built through commands, so the fixture cannot produce a graph the real code never makes.
struct Fixture final {
    authoring::Session session{};
    DocumentController controller{session, {&json_format()}};

    NodeId spring{};
    NodeId mass{};

    Fixture() {
        spring = add_node(session, "demo.spring_damper", "n1");
        mass = add_node(session, "demo.readout", "n2");

        REQUIRE(session.apply(SetParam{spring, 2, qp::ports::Value{200.0}}).has_value());
        REQUIRE(session.apply(SetParam{spring, 5, qp::ports::Value{std::int64_t{1}}}).has_value());
        REQUIRE(session.apply(SetParam{mass, 1, qp::ports::Value{0.5f}}).has_value());
        REQUIRE(session
                    .apply(Connect{PortRef{spring, 1, PortDirection::output},
                                   PortRef{mass, 1, PortDirection::input}})
                    .has_value());

        controller.document().set_title("Spring-damper, 200 N/m");
        controller.document().layouts().set("graph", "{\"1\": [120, 80]}");
    }
};

}  // namespace

TEST_CASE("document.the_measurements_survive_a_save_and_an_open", "[document]") {
    // **The defect this case exists for**: Save wrote the graph and the layout slots, and a student who took
    // readings, saved, closed the window and reopened the file found the experiment intact and their data gone. The
    // loop this platform is arranged around is measure, record, quantify, report -- and "record" has to survive being
    // put on disk, or every other link in the chain is a demonstration rather than a lab session.
    //
    // The test is deliberately the **user's** sequence and not the format's: a session with readings, a save through
    // the controller, a *fresh* controller over a fresh session, and an open. A round trip through
    // `to_bytes`/`from_bytes` alone would have proved the format symmetric while the window still dropped the data.
    Fixture fixture;         // the graph this file's cases save: two nodes, one wire
    const TempDir dir;
    const std::string path = dir.path("session.qpd");

    qp::runtime::RunLedger ledger;
    qp::views::model::MeasurementModel source{ledger, "length", qp::units::dims::length};
    source.add_reading(0.0101, qp::runtime::UncertaintyKind::standard, 1.4e-6);
    source.add_reading(0.0102, qp::runtime::UncertaintyKind::unknown, 0.0);
    source.add_reading(0.0098, qp::runtime::UncertaintyKind::exact, 0.0);
    // A rejected reading, because the store keeps the judgement and a file that dropped it would lose the same
    // distinction the store paid to keep.
    REQUIRE(source.reject(2).has_value());

    DocumentController saver{fixture.session, {&json_format()}, &source};
    const DocumentReport saved = saver.save(json_format(), path);
    REQUIRE(saved.ok);
    REQUIRE(saved.message.find(path) != std::string::npos);

    // The bytes say what a person would want to see: their measurements, in the file.
    std::string bytes;
    REQUIRE(rt::read_whole_file(path, bytes) == rt::FileOutcome::ok);
    REQUIRE(bytes.find("\"readings\"") != std::string::npos);
    REQUIRE(bytes.find("\"kind\": \"standard\"") != std::string::npos);
    REQUIRE(bytes.find("\"uncertainty\": 1.4e-06") != std::string::npos);

    // A **fresh** session and controller, as a reopened window would have.
    qp::views::model::MeasurementModel loaded{ledger, "length", qp::units::dims::length};
    REQUIRE(loaded.dataset().readings().empty());
    DocumentController opener{fixture.session, {&json_format()}, &loaded};
    const DocumentReport opened = opener.open(json_format(), path);
    INFO("open said: " << opened.message);
    REQUIRE(opened.ok);

    // Every reading, with the three things that make it a measurement rather than a number: its value, its
    // uncertainty **and its kind**, and the judgement about whether it still counts.
    const std::vector<qp::runtime::Measurement>& readings = loaded.dataset().readings();
    REQUIRE(readings.size() == 3);
    REQUIRE(readings[0].reading.value == 0.0101);
    REQUIRE(readings[0].reading.u == 1.4e-6);
    REQUIRE(readings[0].reading.kind == qp::runtime::UncertaintyKind::standard);
    // **Absent is not zero**: the second reading's error was never quantified, and it must come back unquantified
    // rather than as a zero that claims it was measured and found exact.
    REQUIRE(readings[1].reading.kind == qp::runtime::UncertaintyKind::unknown);
    REQUIRE(readings[2].reading.kind == qp::runtime::UncertaintyKind::exact);
    REQUIRE(readings[2].valid == false);
    REQUIRE(readings[0].valid == true);
    // The dataset's own identity travelled too: a load replaces the session, dimension included.
    REQUIRE(loaded.dataset().name() == "length");
    REQUIRE(loaded.dataset().dim() == qp::units::dims::length);

    // A controller with no measurement session still saves a graph, which is the documented graph-only case rather
    // than a document with an empty dataset in it.
    DocumentController graph_only{fixture.session, {&json_format()}};
    const std::string bare_path = dir.path("bare.qpd");
    REQUIRE(graph_only.save(json_format(), bare_path).ok);
    std::string bare_bytes;
    REQUIRE(rt::read_whole_file(bare_path, bare_bytes) == rt::FileOutcome::ok);
    REQUIRE(bare_bytes.find("\"readings\"") == std::string::npos);
}

TEST_CASE("document.formats_are_mounted_once_and_in_order", "[document]") {
    // The inversion that keeps the view layer free of the plugin layer: the application mounts, the window
    // reads. What is asserted is the mounting rule -- idempotent per pointer, ordered, and null-tolerant --
    // plus the default a "save as" starts from.
    std::vector<qp::authoring::IDocumentFormat*>& formats = document_formats();
    REQUIRE(&document_formats() == &formats);

    const RefusingFormat first;
    const RefusingFormat second;
    const std::size_t before = formats.size();

    mount_document_format(const_cast<RefusingFormat*>(&first));
    REQUIRE(formats.size() == before + 1);
    REQUIRE(formats.back() == &first);
    mount_document_format(const_cast<RefusingFormat*>(&first));
    REQUIRE(formats.size() == before + 1);

    mount_document_format(const_cast<RefusingFormat*>(&second));
    REQUIRE(formats.size() == before + 2);
    REQUIRE(formats[before] == &first);
    REQUIRE(formats[before + 1] == &second);
    mount_document_format(nullptr);
    REQUIRE(formats.size() == before + 2);

    // A controller given an empty list answers "no format" rather than dereferencing nothing, and a
    // controller given a list offers the first as its default.
    authoring::Session session;
    DocumentController empty{session, {}};
    REQUIRE(empty.default_format() == nullptr);
    REQUIRE(empty.formats().empty());

    DocumentController mounted{session, {&json_format(), const_cast<RefusingFormat*>(&first)}};
    REQUIRE(mounted.default_format() == &json_format());

    formats.resize(before);
}

TEST_CASE("document.save_writes_a_file_that_reopens", "[document]") {
    // The whole point of the feature: a document survives the trip. Asserted field by field rather than by
    // comparing bytes, because a format that reordered its own output would still be correct and this test
    // is about the *document*, not about the text.
    const TempDir dir;
    const std::string path = dir.path("experiment.qpd");
    const std::size_t nodes_before = 2;
    const std::size_t edges_before = 1;

    Fixture fixture;
    REQUIRE(fixture.session.graph().node_count() == nodes_before);
    REQUIRE(fixture.session.graph().edge_count() == edges_before);
    REQUIRE(fixture.controller.is_dirty());
    REQUIRE(fixture.controller.is_untitled());

    const DocumentReport saved = fixture.controller.save(json_format(), path);
    REQUIRE(saved.ok);
    REQUIRE(saved.path == path);
    REQUIRE(saved.format_name == "qp.document.json");
    REQUIRE(saved.nodes == nodes_before);
    REQUIRE(saved.edges == edges_before);
    REQUIRE(saved.message.find(path) != std::string::npos);
    REQUIRE_FALSE(fixture.controller.is_dirty());
    REQUIRE_FALSE(fixture.controller.is_untitled());
    REQUIRE(fixture.controller.document().source_path() == path);
    // Saving does not rename the document: a user who titled a window should not have it revert because
    // they saved somewhere else.
    REQUIRE(fixture.controller.document().title() == "Spring-damper, 200 N/m");

    // Start over: a genuinely different document, so the reopen is not just the same objects still in
    // memory. The canvas's layout slot goes too -- it is part of the document.
    const DocumentReport fresh = fixture.controller.new_document();
    REQUIRE(fresh.ok);
    REQUIRE(fixture.session.graph().node_count() == 0);
    REQUIRE(fixture.controller.document().layouts().empty());
    REQUIRE(fixture.controller.document().title().empty());
    REQUIRE_FALSE(fixture.controller.is_dirty());

    const DocumentReport opened = fixture.controller.open(json_format(), path);
    REQUIRE(opened.ok);
    REQUIRE(opened.nodes == nodes_before);
    REQUIRE(opened.edges == edges_before);
    REQUIRE(opened.message.find(path) != std::string::npos);
    REQUIRE_FALSE(fixture.controller.is_dirty());

    // The graph is back, and so is everything a node is: identity, type, name and the parameters **with
    // their kinds** (an integrator choice that came back as a double would configure a different run).
    REQUIRE(fixture.session.graph().node_count() == nodes_before);
    REQUIRE(fixture.session.graph().edge_count() == edges_before);
    REQUIRE(fixture.session.graph().find_node_by_name("n1").valid());
    REQUIRE(fixture.session.graph().find_node_by_name("n2").valid());

    const NodeId spring = fixture.session.graph().find_node_by_name("n1");
    const qp::graph::Node* node = fixture.session.graph().find_node(spring);
    REQUIRE(node != nullptr);
    REQUIRE(node->type_name == "demo.spring_damper");
    REQUIRE(node->param(2).kind() == qp::ports::ValueKind::f64);
    REQUIRE(node->param(2).as_f64() == 200.0);
    REQUIRE(node->param(5).kind() == qp::ports::ValueKind::i64);
    REQUIRE(node->param(5).as_i64() == 1);

    const NodeId mass = fixture.session.graph().find_node_by_name("n2");
    REQUIRE(fixture.session.graph().find_node(mass)->param(1).kind() == qp::ports::ValueKind::f32);
    REQUIRE(fixture.session.graph().find_node(mass)->param(1).as_f32() == 0.5f);

    const qp::graph::Edge* edge = fixture.session.graph().incoming(PortRef{mass, 1, PortDirection::input});
    REQUIRE(edge != nullptr);
    REQUIRE(edge->from.node == spring);

    // The document's own parts: the title and the canvas's layout slot, which is what lets a reopened
    // document draw its nodes where the user left them.
    REQUIRE(fixture.controller.document().title() == "Spring-damper, 200 N/m");
    REQUIRE(fixture.controller.document().source_path() == path);
    REQUIRE(fixture.controller.document().layouts().has("graph"));
    REQUIRE(fixture.controller.document().layouts().get("graph") == "{\"1\": [120, 80]}");
}

TEST_CASE("document.new_document_is_empty_and_clean", "[document]") {
    // "New" is one event, not an edit that removes every node: the undo stack must not hold the previous
    // document's removal, or the next Ctrl+Z would be an act of archaeology.
    Fixture fixture;
    REQUIRE(fixture.session.can_undo());
    fixture.controller.document().layouts().set("timeseries", "payload");

    const DocumentReport report = fixture.controller.new_document();
    REQUIRE(report.ok);
    REQUIRE(report.nodes == 0);
    REQUIRE(report.edges == 0);
    REQUIRE(report.message.find("new") != std::string::npos);

    REQUIRE(fixture.session.graph().node_count() == 0);
    REQUIRE(fixture.session.graph().edge_count() == 0);
    REQUIRE_FALSE(fixture.session.can_undo());
    REQUIRE_FALSE(fixture.session.can_redo());
    REQUIRE(fixture.controller.document().title().empty());
    REQUIRE(fixture.controller.document().source_path().empty());
    REQUIRE(fixture.controller.document().layouts().empty());
    REQUIRE_FALSE(fixture.controller.is_dirty());
    REQUIRE(fixture.controller.is_untitled());
}

TEST_CASE("document.dirty_tracks_the_session", "[document]") {
    // The flag that stands between a user and losing work. It must follow the **session**, not the paths
    // this class happens to know about: an edit made by a panel, by a script, or by an undo has to mark the
    // document dirty, and the only object that sees all of them is the session's listener.
    const TempDir dir;
    Fixture fixture;

    REQUIRE(fixture.controller.is_dirty());   // the fixture built a graph
    const DocumentReport saved = fixture.controller.save(json_format(), dir.path("a.qpd"));
    REQUIRE(saved.ok);
    REQUIRE_FALSE(fixture.controller.is_dirty());

    // An edit through the bus marks it dirty again.
    REQUIRE(fixture.session
                .apply(SetParam{fixture.spring, 3, qp::ports::Value{0.25}})
                .has_value());
    REQUIRE(fixture.controller.is_dirty());

    // So does an undo: undoing is also an edit, and a document that went clean because the change arrived
    // through the undo stack would lose the work the user just brought back.
    REQUIRE(fixture.controller.save(json_format(), dir.path("a.qpd")).ok);
    REQUIRE_FALSE(fixture.controller.is_dirty());
    REQUIRE(fixture.session.undo().has_value());
    REQUIRE(fixture.controller.is_dirty());

    // And a failed save leaves it dirty, because the work is still only in memory.
    const RefusingFormat refusing;
    REQUIRE_FALSE(fixture.controller.save(const_cast<RefusingFormat&>(refusing), dir.path("b.refuse")).ok);
    REQUIRE(fixture.controller.is_dirty());
}

TEST_CASE("document.a_refused_save_changes_nothing", "[document]") {
    // A format that cannot carry this document says so, and nothing else moves: no path claimed, no dirty
    // flag cleared, no file written. A document that claims a path it does not occupy would make the next
    // Ctrl+S write somewhere the user did not choose.
    const TempDir dir;
    const std::string path = dir.path("never_written.refuse");
    Fixture fixture;
    const RefusingFormat refusing;

    const DocumentReport report =
        fixture.controller.save(const_cast<RefusingFormat&>(refusing), path);
    REQUIRE_FALSE(report.ok);
    REQUIRE(report.format_name == "test.refusing");
    REQUIRE_FALSE(report.message.empty());
    // The sentence names the reason in words a person can act on, not the code's name.
    REQUIRE(report.message.find("field handle") != std::string::npos);

    REQUIRE(fixture.controller.document().source_path().empty());
    REQUIRE(fixture.controller.is_dirty());
    REQUIRE_FALSE(std::filesystem::exists(path));

    // A file-level failure is reported too, and also changes nothing.
    const DocumentReport unwritable =
        fixture.controller.save(json_format(), dir.path("no_such_directory") + "/x.qpd");
    REQUIRE_FALSE(unwritable.ok);
    REQUIRE(unwritable.message.find("could not write") != std::string::npos);
    REQUIRE(fixture.controller.document().source_path().empty());
    REQUIRE(fixture.controller.is_dirty());
}

TEST_CASE("document.a_failed_open_changes_nothing", "[document]") {
    // The worst available outcome here is a half-applied open: the user would then be editing a document
    // that is not the one on disk. Every failure path is asserted to leave the session, the document and
    // the dirty flag exactly as they were.
    const TempDir dir;
    Fixture fixture;
    fixture.controller.document().layouts().set("graph", "kept");

    const std::size_t nodes = fixture.session.graph().node_count();
    const std::string title = fixture.controller.document().title();
    const bool dirty_before = fixture.controller.is_dirty();

    // A file that is not there.
    const DocumentReport missing = fixture.controller.open(json_format(), dir.path("absent.qpd"));
    REQUIRE_FALSE(missing.ok);
    REQUIRE(missing.message.find("no file") != std::string::npos);

    // A file that is there and is not a document.
    const std::string garbage = dir.path("garbage.qpd");
    REQUIRE(rt::write_whole_file(garbage, "this is not a document, it is a sentence") ==
            rt::FileOutcome::ok);
    const DocumentReport foreign = fixture.controller.open(json_format(), garbage);
    REQUIRE_FALSE(foreign.ok);
    REQUIRE(foreign.message.find("not a document") != std::string::npos);

    // A document that was cut short mid-download.
    Fixture source;
    const std::string whole = dir.path("whole.qpd");
    REQUIRE(source.controller.save(json_format(), whole).ok);
    std::string bytes;
    REQUIRE(rt::read_whole_file(whole, bytes) == rt::FileOutcome::ok);
    const std::string cut = dir.path("cut.qpd");
    REQUIRE(rt::write_whole_file(cut, bytes.substr(0, bytes.size() / 2)) == rt::FileOutcome::ok);
    const DocumentReport truncated = fixture.controller.open(json_format(), cut);
    REQUIRE_FALSE(truncated.ok);
    REQUIRE_FALSE(truncated.message.empty());

    // Through all of it: the session, the document and the flag are untouched.
    REQUIRE(fixture.session.graph().node_count() == nodes);
    REQUIRE(fixture.controller.document().title() == title);
    REQUIRE(fixture.controller.document().source_path().empty());
    REQUIRE(fixture.controller.document().layouts().get("graph") == "kept");
    REQUIRE(fixture.controller.is_dirty() == dirty_before);
}

TEST_CASE("document.opening_replaces_the_graph_and_clears_undo", "[document]") {
    // Opening is not an edit. The previous document's edits must not be undoable against the new graph --
    // and they would apply *silently*, because a fresh graph reuses the same handles. That collision is
    // asserted here rather than assumed away, because it is the reason this path calls
    // `Session::replace_graph` instead of replaying commands.
    const TempDir dir;
    const std::string path = dir.path("other.qpd");

    Fixture source;
    REQUIRE(source.controller.save(json_format(), path).ok);

    Fixture other;   // the same shape of document, built independently
    REQUIRE(other.session.can_undo());
    REQUIRE(other.controller.is_dirty());

    const DocumentReport report = other.controller.open(json_format(), path);
    REQUIRE(report.ok);
    REQUIRE(report.nodes == 2);
    REQUIRE(report.edges == 1);

    // The document that is open is the file's, not the one that was being edited.
    REQUIRE(other.controller.document().source_path() == path);
    REQUIRE(other.controller.document().title() == "Spring-damper, 200 N/m");
    REQUIRE_FALSE(other.controller.is_dirty());

    // Nothing to undo: the history belonged to the graph that is gone.
    REQUIRE_FALSE(other.session.can_undo());
    REQUIRE_FALSE(other.session.can_redo());
    REQUIRE_FALSE(other.session.undo().has_value());
    REQUIRE(other.session.graph().node_count() == 2);

    // The handle from the previous document now names a node of this one -- which is exactly why nothing
    // may survive a load by handle, and why the canvas rebuilds on `reset`.
    REQUIRE(other.session.graph().has_node(other.spring));
    REQUIRE(other.session.graph().find_node(other.spring)->name == "n1");
}
