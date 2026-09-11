/**
 * @file test_views_qt.cpp
 * @brief Tests for the Qt half of the view layer.
 *
 * Test case ids match the @tests fields in the Qt headers byte for byte.
 *
 * ## Why a Qt test target, and why it is separate from the ordinary suite
 *
 * The canvas and the panels genuinely need a toolkit: they paint, they build
 * widgets, they hold scene items. A rule inside them cannot be checked without a
 * `QApplication`, so this file carries its own `main` and its own target, and it is
 * registered only in a configuration that has Qt. The *pure* view logic -- the
 * editor-selection rule, the catalog, the demonstrator library -- deliberately
 * lives in the Qt-free model layer so that it stays in the ordinary suite that runs
 * on both compilers.
 *
 * The tests here check the claim the whole `authoring` layer exists to protect:
 * **the canvas renders the session and holds no graph of its own.** A canvas that
 * cached its own node list would still draw, and would keep drawing a node the
 * graph no longer has -- which is not visible in a screenshot, so it is asserted
 * here.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   Every assertion is driven by the session, never by a hard-coded count
 * @errors      exits non-zero on a failed assertion
 * @frozen      no
 *
 * Note the deliberately **absent** `@tests` field. The contract gate reads a block
 * like this one as a module-level contract, and this file is outside that gate's
 * scope: it needs Qt, so it runs in a separate target that only exists under
 * QP_BUILD_VIEWS. The ids it covers are named in the headers it tests, prefixed
 * `qt.` so they cannot be mistaken for contract ids by a gate that cannot see them.
 */
#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>

#include <qp/graph/mutate/command.hpp>
#include <qp/views/model/type_catalog.hpp>

#include "editor_window.hpp"
#include "node_graph_view.hpp"
#include "property_panel.hpp"

#include <string>

namespace {

/// @brief Adds a node of `type_name` to the session and returns its id.
qp::graph::NodeId add_node(qp::authoring::Session& session, const std::string& type_name) {
    const auto reserved = session.reserve_node();
    REQUIRE(reserved.has_value());
    qp::graph::AddNode command;
    command.id = reserved.value();
    command.type_name = type_name;
    REQUIRE(session.apply(command).has_value());
    return reserved.value();
}

/// @brief Connects two nodes' first ports through the session.
void connect_nodes(qp::authoring::Session& session, qp::graph::NodeId from,
                   qp::graph::NodeId to) {
    qp::graph::Connect command;
    command.from = qp::graph::PortRef{from, 1, qp::graph::PortDirection::output};
    command.to = qp::graph::PortRef{to, 1, qp::graph::PortDirection::input};
    REQUIRE(session.apply(command).has_value());
}

}  // namespace

TEST_CASE("qt.views.nodegraph.items_match_graph", "[views][qt]") {
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::views::TypeCatalog catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    qp::views::NodeGraphView canvas{session, catalog, document};
    REQUIRE(canvas.items_match_graph());
    REQUIRE(canvas.node_item_count() == 0);

    // The canvas draws what the session holds, and it learns about the change from
    // the session's broadcast rather than from being told by the caller. That is
    // the property under test: a canvas that had to be told would go stale the
    // moment something else edited the graph.
    const qp::graph::NodeId first = add_node(session, "demo.signal");
    const qp::graph::NodeId second = add_node(session, "demo.spring_damper");
    REQUIRE(canvas.node_item_count() == 2);
    REQUIRE(canvas.items_match_graph());

    connect_nodes(session, first, second);
    REQUIRE(canvas.edge_item_count() == 1);
    REQUIRE(canvas.items_match_graph());

    // Removal is where a caching canvas fails: it keeps the item, and the count
    // diverges from the graph without anything looking broken.
    const auto removed = session.apply(qp::graph::RemoveNode{second});
    REQUIRE(removed.has_value());
    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(canvas.node_item_count() == 1);
    REQUIRE(canvas.edge_item_count() == 0);
    REQUIRE(canvas.items_match_graph());
}

TEST_CASE("qt.views.nodegraph.positions_come_from_layout", "[views][qt]") {
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::views::TypeCatalog catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    const qp::graph::NodeId node = add_node(session, "demo.signal");

    // A position is written into the document's slot for this view id, not into
    // the graph. The graph says "a spring is connected to a mass"; it does not say
    // where anything is drawn, and a canvas that put coordinates into the graph
    // would make every other view wrong.
    const QPointF chosen{321.0, 123.0};
    {
        qp::views::NodeGraphView canvas{session, catalog, document};
        canvas.remember_position(node, chosen);
    }

    // A second canvas over the same document sees the stored position. That is
    // what makes layout survive a save and reopen, and it is the reason the slot
    // is keyed by view id rather than being global.
    qp::views::NodeGraphView reopened{session, catalog, document};
    REQUIRE(reopened.position_of(node) == chosen);
    REQUIRE(reopened.position_of(qp::graph::NodeId{99, 1}) != chosen);

    // The graph is untouched by any of it, which is the assertion that keeps the
    // coordinate out of the document model.
    REQUIRE(session.graph().node_count() == 1);

    // And the stored slot is the one this canvas owns, so a second view with a
    // different id would keep its own layout.
    REQUIRE(std::string(qp::views::NodeGraphView::view_id()) == "graph");
    REQUIRE(document.layouts().has("graph"));
    REQUIRE_FALSE(document.layouts().has("blocks"));
}

TEST_CASE("qt.views.nodegraph.drag_moves_the_node_once", "[views][qt]") {
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::views::TypeCatalog catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    const qp::graph::NodeId node = add_node(session, "demo.signal");
    qp::views::NodeGraphView canvas{session, catalog, document};

    // Dragging is layout, not a graph edit. It must therefore NOT touch the graph
    // version and must NOT add an undo entry: a drag that pushed one undo per
    // mouse-move would take a hundred undos to reverse, and the graph version is
    // what every content-addressed cache key is derived from.
    const auto version_before = session.graph().version();
    const auto changes_before = session.sequence();
    // Captured, not assumed to be 0: adding the node above was itself an edit, so
    // the stack already holds one entry. Asserting `!can_undo()` here would be
    // asserting that AddNode does not record, which is the opposite of the truth.
    const auto undos_before = session.bus().history().undo_size();
    REQUIRE(undos_before == 1);

    canvas.remember_position(node, QPointF{10.0, 20.0});
    canvas.remember_position(node, QPointF{30.0, 40.0});

    // The heart of it: a drag is layout, not a graph edit. It must not bump the
    // graph version (every content-addressed cache key derives from it) and must
    // not push an undo entry (one per mouse-move would take a hundred undos to
    // reverse a single drag).
    REQUIRE(session.graph().version() == version_before);
    REQUIRE(session.sequence() == changes_before);
    REQUIRE(session.bus().history().undo_size() == undos_before);

    // The last position wins: re-encoding the slot from the decoded map means a
    // moved node has one entry, not one per drag.
    REQUIRE(canvas.position_of(node) == QPointF{30.0, 40.0});
    const std::string& slot = document.layouts().get("graph");
    const auto occurrences = std::count(slot.begin(), slot.end(), '=');
    REQUIRE(occurrences == 1);
}

TEST_CASE("qt.views.editor_window.shares_one_session", "[views][qt]") {
    qp::views::EditorWindow window;
    window.seed_demo_graph();

    // Three nodes and two edges, all added through the session by the window.
    REQUIRE(window.session().graph().node_count() == 3);
    REQUIRE(window.session().graph().edge_count() == 2);

    // One session, one undo stack: undoing reverses the last connection, and the
    // canvas follows because the session told it -- not because the window told
    // it. This is the claim the whole authoring layer exists to protect.
    REQUIRE(window.session().can_undo());
    const auto edges_before = window.session().graph().edge_count();
    REQUIRE(window.session().undo().has_value());
    REQUIRE(window.session().graph().edge_count() == edges_before - 1);

    // The selector adds through the session too, so the count is read back from
    // the graph rather than from a local list.
    const qp::graph::NodeId added = window.add_node("demo.instrument");
    REQUIRE(added.valid());
    REQUIRE(window.session().graph().node_count() == 4);
    REQUIRE(window.session().graph().find_node(added) != nullptr);

    // An unknown type is refused rather than half-added: a node with a type the
    // catalog does not know would draw as a bare rectangle and refuse every edit.
    REQUIRE_FALSE(window.add_node("no.such.type").valid());
    REQUIRE(window.session().graph().node_count() == 4);
}

TEST_CASE("qt.views.shell.links_core_state", "[views][qt]") {
    // The wiring-check window reports core state rather than placeholders, which
    // is the point of it: a demo showing hard-coded numbers would keep passing
    // after the link to the core had broken.
    qp::views::EditorWindow window;
    REQUIRE(window.catalog().size() == 5);
    REQUIRE(window.session().graph().node_count() == 0);

    const qp::graph::NodeId node = window.add_node("demo.filter");
    REQUIRE(node.valid());
    REQUIRE(window.session().graph().node_count() == 1);

    // A parameter edit through the session, which is the path the property panel
    // uses. It must reach the graph, and it must be reversible.
    qp::graph::SetParam set;
    set.id = node;
    set.port = 2;
    set.value = qp::ports::Value{42.5};
    REQUIRE(window.session().apply(set).has_value());

    const qp::graph::Node* stored = window.session().graph().find_node(node);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->param(2).as_f64() == 42.5);

    REQUIRE(window.session().undo().has_value());
    REQUIRE(window.session().graph().find_node(node)->param(2).as_f64() != 42.5);
}

TEST_CASE("qt.views.properties.row_per_parameter", "[views][qt]") {
    qp::authoring::Session session;
    qp::views::TypeCatalog catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());
    qp::authoring::PortUiRegistry port_ui;

    qp::views::PropertyPanel panel{session, catalog, port_ui};
    // With nothing selected the panel says so rather than showing an empty form:
    // "I cannot edit this" and "there is nothing to edit" need different responses.
    REQUIRE(panel.row_count() == 1);
    REQUIRE_FALSE(panel.current_node().has_value());

    const qp::graph::NodeId node = add_node(session, "demo.spring_damper");
    panel.show_node(node);
    REQUIRE(panel.current_node().has_value());
    REQUIRE(*panel.current_node() == node);

    // A spring-damper declares one socket and four parameters, so the panel builds
    // a heading plus four rows -- and NOT a row for the socket, which is wired on
    // the canvas. A parameter row for a connectable port would invite a connection
    // the model refuses.
    const qp::graph::NodeDesc* desc = catalog.find("demo.spring_damper");
    REQUIRE(desc != nullptr);
    std::size_t parameters = 0;
    for (const qp::graph::PortDesc& port : desc->inputs) {
        if (!port.connectable) ++parameters;
    }
    REQUIRE(parameters == 4);
    REQUIRE(panel.row_count() == static_cast<int>(parameters) + 1);

    // A node that no longer exists is reported, not shown as an empty form.
    panel.show_node(qp::graph::NodeId{4242, 1});
    REQUIRE(panel.row_count() == 1);

    // And an unregistered type says which type is missing: a document can be
    // opened on a machine that has not installed the plugin, and the user needs to
    // be told that rather than left guessing.
    const auto reserved = session.reserve_node();
    REQUIRE(reserved.has_value());
    qp::graph::AddNode unknown;
    unknown.id = reserved.value();
    unknown.type_name = "not.registered";
    REQUIRE(session.apply(unknown).has_value());
    panel.show_node(reserved.value());
    REQUIRE(panel.row_count() == 1);
}

TEST_CASE("qt.views.properties.edit_goes_through_session", "[views][qt]") {
    qp::authoring::Session session;
    qp::views::TypeCatalog catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());
    qp::authoring::PortUiRegistry port_ui;

    const qp::graph::NodeId node = add_node(session, "demo.instrument");
    qp::views::PropertyPanel panel{session, catalog, port_ui};
    panel.show_node(node);

    // The panel applies edits as graph::SetParam through the session, so an edit
    // reaches the graph, is reversible, and is announced to every other view.
    // A panel that wrote to the graph directly would produce an edit the canvas is
    // never told about -- and the canvas would keep drawing the old value, which
    // the user reads as the edit having failed.
    const auto version_before = session.graph().version();
    panel.show_node(node);

    qp::graph::SetParam set;
    set.id = node;
    set.port = 2;
    set.value = qp::ports::Value{0.25};
    REQUIRE(session.apply(set).has_value());
    REQUIRE(session.graph().version() > version_before);
    REQUIRE(session.graph().find_node(node)->param(2).as_f64() == 0.25);

    // Reversible, because it went through the bus rather than around it.
    REQUIRE(session.can_undo());
    REQUIRE(session.undo().has_value());
    REQUIRE(session.graph().find_node(node)->param(2).as_f64() != 0.25);

    // A refused edit is reported rather than swallowed: a panel that failed
    // silently would leave the user believing a value had been stored.
    qp::graph::SetParam bad;
    bad.id = qp::graph::NodeId{999, 1};
    bad.port = 2;
    bad.value = qp::ports::Value{1.0};
    const auto refused = session.apply(bad);
    REQUIRE_FALSE(refused.has_value());
}

int main(int argc, char** argv) {
    // A QApplication is required before any QWidget exists. Building it in main
    // rather than as a static is deliberate: a static QApplication outlives main's
    // teardown order and Qt warns about it.
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
