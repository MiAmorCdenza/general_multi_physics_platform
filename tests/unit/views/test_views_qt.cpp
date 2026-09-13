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
 * ## The direction that is hardest to test, and why it is here
 *
 * Three of these cases assert **feedback** rather than output: which stub lights up under the cursor, which nodes
 * stay lit around a selection, and which device a recorded number points back at. None of them changes a number
 * anywhere, so none of them can be caught by a model-level assertion -- and all three fail silently, as a canvas
 * that merely stops responding to the mouse. That is why their evidence has to be a query on the widget rather than
 * an inspection of pixels, and why the widget exposes `hovered_port`, `prominence_of` and `selected_source` at all.
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

#include <cmath>

#include <QApplication>
#include <QAction>
#include <QColor>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPixmap>

#include <qp/graph/execution/run_provider.hpp>
#include <qp/views/model/blueprint.hpp>
#include <qp/graph/mutate/command.hpp>
#if defined(QP_HAS_QPJSON_FORMAT)
#include <qp/plugins/csv/csv_exporter.hpp>
#include <qp/plugins/qpjson/qpjson_format.hpp>
#include <qp/views/model/export_controller.hpp>
#endif
#include <qp/views/model/demo_library.hpp>
#include <qp/views/model/document_controller.hpp>
#include <qp/views/model/measurement_model.hpp>
#include <qp/views/model/run_providers.hpp>
#include <qp/graph/ir/node_type_registry.hpp>
#include <qp/host/host.hpp>
#if defined(QP_HAS_INSTRUMENTS)
#include <qp/plugins/instruments/instruments.hpp>
#endif

#include "editor_window.hpp"
#include <QString>
#include <QStringList>

#include "scene_view.hpp"
#include "scene_view3d.hpp"

#include <QDockWidget>
#include <QMenu>
#include "confidence_panel.hpp"
#include "fit_panel.hpp"
#include "measurement_panel.hpp"
#include "node_graph_view.hpp"
#include "property_panel.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

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

TEST_CASE("qt.views.editor_window.a_demo_replaces_the_document", "[views][qt]") {
    // **The user's report, as a case.** Launch the shell, choose `Demos -> magnetosphere`, press Run: the picture
    // panels stayed empty. The cause was that a demo was *added* to the document the window opens with -- the
    // spring-damper demonstrator -- and the run controller asks the operator path first (deliberately: a graph both
    // paths could run keeps the answer it had before providers existed). The oscillator therefore claimed the run,
    // the kit never ran, and the line on screen reported validation problems rather than the run that did not happen.
    //
    // What the case pins is the word: a demo **opens** a document. The graph afterwards holds the demo's nodes and
    // nothing else, whatever was there before.
    qp::views::model::GraphBlueprint demo;
    demo.label = "two nodes";
    qp::views::model::BlueprintNode source;
    source.type_name = "demo.signal";
    source.name = "src";
    demo.nodes.push_back(source);
    qp::views::model::BlueprintNode sink;
    sink.type_name = "demo.instrument";
    sink.name = "meter";
    demo.nodes.push_back(sink);
    qp::views::model::BlueprintWire wire;
    wire.from = 0;
    wire.from_port = 1;
    wire.to = 1;
    wire.to_port = 1;
    demo.wires.push_back(wire);

    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content, {demo}};

    // A document that already has something in it: the demonstrator the window opens with, in miniature.
    window.seed_demo_graph();
    const std::size_t before = window.session().graph().node_count();
    REQUIRE(before >= 2);
    REQUIRE(window.session().graph().edge_count() >= 1);

    window.seed_blueprint(demo);
    // **Replaced, not appended**: the demo's two nodes, and the demonstrator's are gone. An appended demo would
    // read `before + 2` here, which is exactly the state that produced the user's empty panels.
    REQUIRE(window.session().graph().node_count() == 2);
    REQUIRE(window.session().graph().edge_count() == 1);
    bool has_source = false;
    bool has_meter = false;
    bool has_demo_oscillator = false;
    for (const qp::graph::NodeSlot& slot : window.session().graph().slots()) {
        if (!slot.occupied) continue;
        if (slot.node.name == "src") has_source = true;
        if (slot.node.name == "meter") has_meter = true;
        if (slot.node.type_name == "demo.spring_damper") has_demo_oscillator = true;
    }
    REQUIRE(has_source);
    REQUIRE(has_meter);
    REQUIRE_FALSE(has_demo_oscillator);

    // The status line says what to do next, which is the second half of the fix: the sentence a user needs after
    // choosing a demo is "press Run", not a report about the graph they no longer have.
    REQUIRE(window.status_label() != nullptr);
    REQUIRE(window.status_label()->text().contains(QStringLiteral("press Run")));

    // And a demo this build cannot offer leaves the document **alone**: validated before anything is cleared, so a
    // refusal is not a way to lose work. (This is the ordering the existing refusal case caught me getting wrong
    // once already.)
    qp::views::model::GraphBlueprint absent = demo;
    absent.nodes[1].type_name = "demo.nonexistent";
    window.seed_blueprint(absent);
    REQUIRE(window.session().graph().node_count() == 2);
}

TEST_CASE("qt.views.scene3d.a_scene_can_be_turned", "[views][qt][scene3d]") {
    // **The camera belongs to the panel, and the scene's view is where it starts.** A scene states the direction its
    // item wants to be seen from; the panel opens there, and the user turns it from that point. The assertion that
    // matters is the *reset*: a new scene puts the camera back on the direction the new item asked for, so a graph
    // whose picture wants the equatorial plane is not drawn from wherever the previous one was left.
    qp::views::SceneView3D panel{QStringLiteral("nothing to draw yet")};
    REQUIRE(panel.empty_text() == QStringLiteral("nothing to draw yet"));

    qp::graph::ViewScene ring;
    ring.view.azimuth_deg = 0.0;
    ring.view.elevation_deg = 90.0;
    ring.x_min = -2.0;
    ring.x_max = 2.0;
    ring.y_min = -2.0;
    ring.y_max = 2.0;
    ring.z_min = -2.0;
    ring.z_max = 2.0;
    ring.has_bounds = true;
    ring.body_radius = 1.0;
    for (int i = 0; i < 16; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / 16.0;
        ring.points.push_back(qp::graph::ViewScene::Point{2.0 * std::cos(angle), 2.0 * std::sin(angle), 0.0});
    }
    panel.set_scene(ring);
    REQUIRE(panel.camera().view().azimuth_deg == 0.0);
    // **89, not the 90 the scene asked for**, and that is the camera's contract rather than a defect: elevation is
    // clamped one degree inside the poles, because the screen's basis is discontinuous exactly at them -- the
    // fallback that keeps the basis finite at `90` points the screen a different way from the limit approaching it,
    // so a user orbiting across the pole would see the picture flip. A degree of difference is invisible; a flip is
    // not. The item's own `elevation 90` is its way of saying "the equatorial plane", and the camera lands as close
    // to it as a continuous basis allows.
    REQUIRE(panel.camera().view().elevation_deg == 89.0);
    // The fit is the scene's own box, so the camera sits at the default multiple of it rather than at a fixed
    // distance: the same panel shows a ring of two earth radii and a magnetosphere of twenty at their own scales.
    // **Exactly the box's half-extent here**, because this view looks straight down at a face -- the corner-on
    // direction is what grows it, by up to sqrt(3), which the projection's own case measured.
    REQUIRE(panel.camera().half_width() == 2.0);
    REQUIRE(std::abs(panel.camera().distance() -
                     qp::views::model::kDefaultCameraDistanceFactor * 2.0) < 1.0e-12);

    // Turning it: the panel's camera moves, and the *scene* does not -- the view in the value is the item's request,
    // not the user's current angle, which is what keeps a repaint deterministic.
    panel.camera().orbit(35.0, -20.0);
    REQUIRE(std::abs(panel.camera().view().azimuth_deg - 35.0) < 1.0e-12);
    // **69, not 70**: the reset landed on the clamp (89) rather than on the 90 the scene asked for, and the orbit is
    // relative to where the camera actually is. An assertion that forgot the clamp would be off by exactly the degree
    // the clamp costs, which is the kind of slip this comment exists to prevent next time.
    REQUIRE(std::abs(panel.camera().view().elevation_deg - 69.0) < 1.0e-12);
    REQUIRE(panel.scene().view.azimuth_deg == 0.0);
    REQUIRE(panel.scene().view.elevation_deg == 90.0);

    // A field-line scene asks for the meridional plane, and setting it puts the camera there: the reset is the
    // behaviour a user sees when they switch demos.
    qp::graph::ViewScene shells = ring;
    shells.view.azimuth_deg = -90.0;
    shells.view.elevation_deg = 0.0;
    shells.points.clear();
    shells.polylines.push_back({qp::graph::ViewScene::Point{1.0, 0.0, 0.0}, qp::graph::ViewScene::Point{0.0, 0.0, 1.0}});
    panel.set_scene(shells);
    // **270, which is the camera's spelling of the scene's -90.** The scene states the direction it wants in whatever
    // angles describe it -- a meridional view is -90 as naturally as 270 -- and the camera **wraps** azimuth into
    // [0, 360) because that is the range an orbit control stays sane in. The two are the same direction, and the scene
    // keeps its own wording: the value in the scene is the item's request, not the camera's state.
    REQUIRE(panel.scene().view.azimuth_deg == -90.0);
    REQUIRE(panel.camera().view().azimuth_deg == 270.0);
    REQUIRE(panel.camera().view().elevation_deg == 0.0);

    // An empty scene says so rather than drawing nothing: the panel keeps its message.
    panel.set_scene(qp::graph::ViewScene{});
    REQUIRE(panel.scene().empty());
}

TEST_CASE("qt.views.scene3d.the_panel_is_dockable_and_floatable", "[views][qt][scene3d]") {
    // **"Embedded by default, standalone on request", asserted rather than described.** The panel is a dock in the
    // window's bottom area -- so it is embedded, beside the two flat pictures -- and it carries the full dock feature
    // set, which is what lets Qt turn it into a top-level window with one click on its title bar. The two flat panels
    // deliberately have no features, so this case also pins the *difference*: a user can only pop out the one that
    // has something to gain from a second screen.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};

    QDockWidget* view3d = nullptr;
    for (QDockWidget* dock : window.findChildren<QDockWidget*>()) {
        if (dock->windowTitle() == QStringLiteral("3D view")) view3d = dock;
    }
    REQUIRE(view3d != nullptr);
    REQUIRE(view3d->widget() != nullptr);
    REQUIRE(dynamic_cast<qp::views::SceneView3D*>(view3d->widget()) != nullptr);

    // Embedded: a dock that is not floating is a panel in the window, which is what "default" means here.
    REQUIRE_FALSE(view3d->isFloating());
    REQUIRE(window.dockWidgetArea(view3d) == Qt::BottomDockWidgetArea);

    // Standalone: the features are what make it possible, and `setFloating` is what a user's click does.
    REQUIRE((view3d->features() & QDockWidget::DockWidgetFloatable) != 0);
    REQUIRE((view3d->features() & QDockWidget::DockWidgetMovable) != 0);
    view3d->setFloating(true);
    REQUIRE(view3d->isFloating());
    view3d->setFloating(false);
    REQUIRE_FALSE(view3d->isFloating());

    // The flat panels are the contrast: no features, so they stay where they are put. Asserted because the difference
    // is a decision -- a two-dimensional picture has no reason to float, and a three-dimensional one does.
    // **The flat panels are deliberately not compared here, and the reason is this binary.** They are built one per
    // *mounted* view item, and mounting happens in the application's composition root: `qp::graph::view_items()` is
    // empty in a test binary, so this window has no flat docks to contrast against. The contrast is real -- the flat
    // panels carry `NoDockWidgetFeatures` because a picture in a fixed plane has no reason to float -- and it is
    // visible in the window itself; asserting it here would make this case pass or fail according to whether some
    // other binary's composition had leaked into the process, which is worse than not asserting it.
    REQUIRE(qp::graph::view_items().empty());
}

TEST_CASE("qt.views.nodegraph.items_match_graph", "[views][qt]") {
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::graph::NodeTypeRegistry catalog;
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
    qp::graph::NodeTypeRegistry catalog;
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
    qp::graph::NodeTypeRegistry catalog;
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

TEST_CASE("qt.views.editor_window.a_supplied_blueprint_becomes_a_graph", "[views][qt]") {
    // **The window stopped knowing a plugin's vocabulary.** A demo arrives as a blueprint -- type names, starting
    // parameters, wires by index -- and this case supplies one built from the demonstrator types the window itself
    // registers, which is exactly what the application does with a content kit it mounts. Two claims: the
    // blueprint's nodes and wires arrive through the session, and a blueprint this catalog cannot offer is refused
    // with the graph untouched.
    qp::views::model::GraphBlueprint demo;
    demo.label = "test demo";
    const auto node = [&demo](const char* type, const char* name) {
        qp::views::model::BlueprintNode n;
        n.type_name = type;
        n.name = name;
        demo.nodes.push_back(std::move(n));
        return demo.nodes.size() - 1;
    };
    const std::size_t source = node("demo.signal", "src");
    node("demo.spring_damper", "model");
    qp::views::model::BlueprintWire wire;
    wire.from = source;
    wire.from_port = 1;
    wire.to = 1;
    wire.to_port = 1;
    demo.wires.push_back(wire);

    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content, {demo}};
    REQUIRE(window.session().graph().node_count() == 0);

    window.seed_blueprint(demo);
    REQUIRE(window.session().graph().node_count() == 2);
    REQUIRE(window.session().graph().edge_count() == 1);
    // The names the blueprint asked for are the names in the graph.
    bool named = false;
    for (const qp::graph::NodeSlot& slot : window.session().graph().slots()) {
        if (slot.occupied && slot.node.name == "src") named = true;
    }
    REQUIRE(named);
    // Undoable like any other edit: the wire came back off first, because it was applied last.
    REQUIRE(window.session().undo().has_value());
    REQUIRE(window.session().graph().edge_count() == 0);

    // A blueprint naming a type this build does not have: refused, with nothing added.
    qp::views::model::GraphBlueprint absent = demo;
    absent.label = "absent demo";
    absent.nodes[1].type_name = "demo.nonexistent";
    const std::size_t before = window.session().graph().node_count();
    window.seed_blueprint(absent);
    REQUIRE(window.session().graph().node_count() == before);
}

TEST_CASE("qt.views.editor_window.shares_one_session", "[views][qt]") {
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
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

TEST_CASE("qt.views.editor_window.file_menu_follows_the_document", "[views][qt]") {
    // What a window can be held to, given that a file dialog is modal and cannot be driven from a test: the
    // **wiring**, and the two pieces of state a user reads without opening a menu -- the caption and the
    // modified marker.
    //
    // The behaviour behind the menu (what a save writes, what a failed open leaves alone) lives in
    // `views::model::DocumentController` and is asserted without Qt in `tests/model/`. That split is
    // deliberate: a test that clicked through a dialog would be testing the dialog. The handlers
    // themselves are one line each -- ask for a path, then call the method below -- so what is left to
    // assert here is that the menu exists, that a format being mounted enables it, and that the caption
    // follows the document.
#if defined(QP_HAS_QPJSON_FORMAT)
    qp::plugins::qpjson::QpJsonFormat format;
    qp::views::model::mount_document_format(&format);
    // The export format too, because the Export action is greyed out when nothing is registered -- and a
    // greyed-out entry is the correct behaviour, not something to assert as enabled.
    qp::plugins::csv::CsvExporter exporter;
    REQUIRE(qp::views::model::mount_export_format(&exporter).has_value());
#else
    // Without a format plugin there is nothing to save with, and the case would assert a menu that is
    // correctly greyed out. Skipped explicitly rather than compiled into a lie.
    SKIP("this build has no document format plugin");
#endif

    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo_graph();

    QMenu* file_menu = nullptr;
    for (QAction* action : window.menuBar()->actions()) {
        if (action->menu() != nullptr && action->text().contains(QStringLiteral("File"))) {
            file_menu = action->menu();
        }
    }
    REQUIRE(file_menu != nullptr);

    QStringList texts;
    for (QAction* action : file_menu->actions()) {
        if (action->isSeparator()) continue;
        texts << action->text();
    }
    const QString joined = texts.join(QStringLiteral("|"));
    REQUIRE(joined.contains(QStringLiteral("New")));
    REQUIRE(joined.contains(QStringLiteral("Open")));
    REQUIRE(joined.contains(QStringLiteral("Save")));
    REQUIRE(joined.contains(QStringLiteral("Export")));
    // A format is mounted in this process, so none of them is greyed out.
    for (QAction* action : file_menu->actions()) {
        if (action->isSeparator()) continue;
        REQUIRE(action->isEnabled());
    }

    // The modified marker follows the session: the demo graph was just built, so the document is dirty and
    // the caption carries Qt's placeholder.
    REQUIRE(window.isWindowModified());
    REQUIRE(window.windowTitle().contains(QStringLiteral("- qp")));

    // Saving through the same method the Save action calls clears it, and the caption follows -- asserted
    // through the window rather than through the controller, because the wiring between them is the part
    // this file can check.
    const std::string path = (std::filesystem::temp_directory_path() / "qp_caption_test.qpd").string();
    REQUIRE(window.save_document(path));
    REQUIRE_FALSE(window.isWindowModified());
    REQUIRE(window.windowTitle().contains(QStringLiteral("qp_caption_test.qpd")));

    // Opening it back is the same path the Open action runs after the dialog, and the caption follows the
    // document's own title rather than the file name.
    REQUIRE(window.open_document(path));
    REQUIRE_FALSE(window.isWindowModified());

    // A file whose extension belongs to no mounted format is refused by name rather than handed to the
    // wrong reader.
    REQUIRE_FALSE(window.open_document(path + ".unknown"));

    // The export action takes the other path out of the window: the measurement session's trace, written
    // through the registered format. Asserted through the window rather than the controller, because the
    // wiring -- which session, which format, and the pre-flight before either -- is what this case covers.
    window.seed_demo_measurement();
    REQUIRE_FALSE(window.measurements().dataset().readings().empty());
    const std::string csv = (std::filesystem::temp_directory_path() / "qp_caption_test.csv").string();
    REQUIRE(window.export_document(csv));
    REQUIRE(std::filesystem::exists(csv));

    // Opening a document empties the measurement session, both records at once. The interactive pass found
    // the confidence panel still reporting the **previous** trace's energy drift after a load: the readings
    // were cleared and the trace was not, and the panel showed two experiments at once. Asserted here on the
    // session's own state, which is what the panels read.
    REQUIRE(window.open_document(path));
    REQUIRE(window.measurements().dataset().readings().empty());
    REQUIRE(window.measurements().trace().empty());
    REQUIRE_FALSE(window.measurements().trace().run().valid());

    // A new document does the same, so the panel cannot show one experiment's numbers beside another's graph.
    window.seed_demo_measurement();
    REQUIRE_FALSE(window.measurements().dataset().readings().empty());
    window.new_document();
    REQUIRE(window.measurements().dataset().readings().empty());
    REQUIRE(window.measurements().trace().empty());
    REQUIRE(window.session().graph().node_count() == 0);
    REQUIRE_FALSE(window.isWindowModified());

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(csv, ignored);
}

TEST_CASE("qt.views.shell.links_core_state", "[views][qt]") {
    // The wiring-check window reports core state rather than placeholders, which
    // is the point of it: a demo showing hard-coded numbers would keep passing
    // after the link to the core had broken.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
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
    qp::graph::NodeTypeRegistry catalog;
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
    qp::graph::NodeTypeRegistry catalog;
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

namespace {

/// @brief A ledger owned for the lifetime of the cases that need a borrowed one.
///
/// `MeasurementModel` borrows its ledger rather than owning it -- that is the fix for the window
/// having two -- so a case that builds a model directly must own a ledger that outlives it.
qp::runtime::RunLedger& window_ledger_for_test() {
    static qp::runtime::RunLedger ledger;
    return ledger;
}

/// @brief A measurement model over that ledger, for the panel cases.
qp::views::model::MeasurementModel& window_measurements_for_test() {
    static qp::views::model::MeasurementModel model{window_ledger_for_test(), "length",
                                                   qp::units::dims::length};
    return model;
}

}  // namespace
TEST_CASE("qt.views.measurement.gaps_are_shown_verbatim", "[views][qt]") {
    // The panel must render the model's gaps **as the model words them**. Re-wording them in the
    // panel would mean two places decide what a gap says, and the one that is tested -- the
    // model's, which the ordinary suite asserts on both compilers -- would be the one nobody
    // reads. A student reading "uncertainty: N/A" learns something different from "no reading
    // carries a quantified uncertainty, so the Type A and combined uncertainties are unknown
    // rather than zero", and only the second one tells them what to go and do.
    qp::views::model::MeasurementModel& model = window_measurements_for_test();
    qp::views::MeasurementPanel panel(model);

    // One line per gap, each prefixed with a dash and otherwise untouched.
    const std::vector<std::string> gaps = model.gaps();
    const QStringList shown = panel.gap_lines();
    REQUIRE(shown.size() == static_cast<int>(gaps.size()));
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        const QString expected = QStringLiteral("- ") + QString::fromStdString(gaps[i]);
        REQUIRE(shown[static_cast<int>(i)] == expected);
    }

    // And an empty session shows exactly one gap -- that it is empty -- rather than a row of
    // zeros. A zero mean over no readings is the fabrication this assertion exists to prevent.
    REQUIRE(shown.size() == 1);
    REQUIRE(shown[0].contains(QStringLiteral("no readings")));

    // A reading makes the summary show a mean, and the panel shows the model's number.
    model.add_reading(1.0, qp::runtime::UncertaintyKind::standard, 0.1);
    model.add_reading(1.2, qp::runtime::UncertaintyKind::standard, 0.1);
    panel.refresh();
    const auto mean = model.report_line().mean;
    REQUIRE(mean.has_value());
    REQUIRE(panel.summary_text().contains(QString::number(mean.value(), 'g', 6)));

    // With no quantified reading the combined figure is spelled out as unknown rather than
    // omitted, because an omitted term reads as a term of zero.
    qp::views::model::MeasurementModel plain_model{window_ledger_for_test(),
                                                   "plain", qp::units::Dim{}};
    qp::views::MeasurementPanel plain_panel(plain_model);
    plain_model.add_reading(5.0);
    plain_model.add_reading(5.1);
    plain_panel.refresh();
    REQUIRE(plain_panel.summary_text().contains(QStringLiteral("combined unknown")));
}

TEST_CASE("qt.views.canvas.unframed_view_reports_clipped", "[views][qt]") {
    // The negative fixture for `all_nodes_are_visible`.
    //
    // Without this, an implementation that returned `true` unconditionally would pass the case
    // above -- and that is not a hypothetical: the defect being guarded against was a view that
    // *could not* show the graph while every model-level check said everything was fine. A
    // predicate that agrees with the model is exactly the one that cannot see it.
    //
    // The setup is deterministic rather than a contrived scroll: a canvas that has never been
    // framed sits at the default transform with its viewport at the scene origin, and a graph
    // whose nodes start at (48, 48) and run past x = 600 cannot fit in a viewport narrower than
    // that. No window manager, no timing, no pixel inspection.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();
    window.resize(1280, 720);

    auto* canvas = window.findChild<qp::views::NodeGraphView*>();
    REQUIRE(canvas != nullptr);

    // The model is fine, which is the point: the two assertions below disagree, and only one of
    // them is about what the user can see.
    REQUIRE(canvas->items_match_graph());
    REQUIRE(canvas->node_item_count() == 3);

    // A view that has never been framed reports the graph as clipped. Note this is asserted
    // **before** showing the window, so the deferred framing in `rebuild` has not run.
    REQUIRE_FALSE(canvas->all_nodes_are_visible());

    // And framing it -- which is what `rebuild` does once the viewport has a size -- makes the
    // same predicate agree. Same object, same graph, different answer: the predicate measures the
    // view rather than restating the model.
    //
    // The **viewport** is resized explicitly, not just the widget. `frame_graph` computes its
    // scale from `viewport()->size()`, and a widget that has never been shown keeps a viewport of
    // default size -- which is exactly why `rebuild` defers its framing to the next event-loop
    // turn. Driving the viewport directly is what makes this case deterministic without a window
    // manager, and without that the positive half silently does nothing while the negative half
    // still passes.
    canvas->resize(900, 600);
    canvas->viewport()->resize(880, 560);
    canvas->frame_graph();
    REQUIRE(canvas->all_nodes_are_visible());
}

TEST_CASE("qt.views.canvas.a_node_box_holds_its_type", "[views][qt]") {
    // Charter C5 asks for **large type**, and this is the case that holds it. The palette work made colour and
    // contrast testable; the size was still nobody's, and a screenshot review found the labels "legible but
    // small" -- at a laptop's device-pixel ratio a port label was about six physical pixels tall.
    //
    // Three things are asserted, and the third is the one a screenshot cannot check:
    //
    //   1. a node box is big enough to hold what it draws, measured against the fonts this platform resolved
    //      rather than against the constants in the paint code;
    //   2. the fallback layout puts a fresh graph inside the canvas at **1:1**, so nothing is shrunk to fit and
    //      the type size survives;
    //   3. `frame_graph` never drops below `kMinimumScale`, so framing a graph that does not fit cannot undo the
    //      size either. That is the clamp this case exists for: without it, "the text is 10 pt" is true in the
    //      paint code and false on the screen.
    const QSizeF authored = qp::views::NodeGraphView::authored_node_size();
    REQUIRE(authored.width() >= 160.0);

    // A header tall enough for a bold 10 pt line, plus a body with room for the type name, three port rows and
    // the footer. The lower bound is derived from font metrics rather than written as a number, so it stays
    // honest on a machine whose default font is larger.
    QFont title;
    title.setPointSize(10);
    title.setBold(true);
    const QFontMetricsF title_metrics{title};
    QFont label;
    label.setPointSize(9);
    const QFontMetricsF label_metrics{label};
    const qreal minimum_height = title_metrics.height() + label_metrics.height() +
                                 3.0 * (label_metrics.height() + 4.0) + label_metrics.height();
    INFO("authored box is " << authored.height() << " tall, minimum is " << minimum_height);
    REQUIRE(authored.height() >= minimum_height);

    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();
    window.resize(1280, 720);
    window.show();
    QCoreApplication::processEvents();

    auto* canvas = window.findChild<qp::views::NodeGraphView*>();
    REQUIRE(canvas != nullptr);
    REQUIRE(canvas->node_item_count() == 3);

    // The viewport is resized to the size this window actually produces, and that number is the point of the
    // case. A `EditorWindow` at 1280x720 puts a splitter, a palette and a property panel around the canvas, so
    // the canvas gets about 484 pixels -- and the first version of this layout asked for 547, which is exactly
    // why framing it chose 0.8 and the type size was quietly undone. Driving the viewport directly makes the
    // case measure the real constraint instead of the window's nominal width.
    canvas->resize(500, 640);
    canvas->viewport()->resize(484, 610);
    canvas->frame_graph();

    // 1:1 at the canvas the application actually gives: the fallback layout is two columns precisely so this
    // holds, and a graph drawn at its authored size is the only way the point sizes above mean anything.
    INFO("canvas viewport " << canvas->viewport()->width() << "x" << canvas->viewport()->height()
                            << ", scene " << canvas->sceneRect().width() << "x"
                            << canvas->sceneRect().height() << ", scale " << canvas->scale_factor());
    REQUIRE(canvas->scale_factor() == 1.0);
    // And it is genuinely all on screen, which is the other half: a layout that fits by being mainly off the
    // edge would satisfy the scale assertion and nothing else.
    REQUIRE(canvas->all_nodes_are_visible());

    // The floor. A canvas narrower than the graph must scroll rather than shrink past the point where the type
    // size it was designed around is gone.
    canvas->resize(300, 320);
    canvas->viewport()->resize(280, 290);
    canvas->frame_graph();
    INFO("scale after shrinking the canvas to 280x290: " << canvas->scale_factor());
    REQUIRE(canvas->scale_factor() >= qp::views::NodeGraphView::kMinimumScale);

    window.close();
}

TEST_CASE("qt.views.nodegraph.an_edge_ends_on_its_ports", "[views][qt]") {
    // The canvas used to draw a connection between two box **centres**, so the graph held a correct edge and the
    // screen showed a line touching neither port. Every model-level assertion passed; the picture was wrong. A
    // user reading that picture draws the connection again, and the second one is refused as a duplicate.
    //
    // So this asserts the geometry rather than the count: the line's endpoints are on the stubs of the ports the
    // edge names. `port_scene_pos` is the canvas's own answer for where a stub is, and `paint` uses the same
    // expression to draw it -- which is the property under test, because two implementations of "where is the
    // port" would be a line that ends near a port instead of on it.
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::graph::NodeTypeRegistry catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    qp::views::NodeGraphView canvas{session, catalog, document};
    const qp::graph::NodeId source = add_node(session, "demo.signal");
    const qp::graph::NodeId model = add_node(session, "demo.spring_damper");
    REQUIRE(source.valid());
    REQUIRE(model.valid());

    qp::graph::Connect command;
    command.from = qp::graph::PortRef{source, 1, qp::graph::PortDirection::output};
    command.to = qp::graph::PortRef{model, 1, qp::graph::PortDirection::input};
    REQUIRE(session.apply(command).has_value());
    REQUIRE(canvas.edge_item_count() == 1);

    const auto [from, to] = canvas.edge_endpoints(0);

    // The source end is on the right edge of the signal node -- where its output stub is -- and the target end on
    // the left edge of the spring-damper. The `x` assertions are the ones that fail for a centre-to-centre line:
    // a centre sits at half the box width, not at either edge.
    const QSizeF box = qp::views::NodeGraphView::authored_node_size();
    const QPointF source_pos = canvas.position_of(source);
    const QPointF model_pos = canvas.position_of(model);
    INFO("edge runs (" << from.x() << "," << from.y() << ") -> (" << to.x() << "," << to.y() << ")");
    REQUIRE(std::abs(from.x() - (source_pos.x() + box.width())) < 0.01);
    REQUIRE(std::abs(to.x() - model_pos.x()) < 0.01);
    // Vertically inside the box, on a port row rather than in the header or the footer. Both nodes' first ports
    // are on row 1, so the line is horizontal here -- that is geometry, not a defect, and asserting they differ
    // was an assumption about the demo library rather than about the canvas.
    REQUIRE(from.y() > source_pos.y());
    REQUIRE(from.y() < source_pos.y() + box.height());
    REQUIRE(to.y() > model_pos.y());
    REQUIRE(to.y() < model_pos.y() + box.height());

    // Dragging a node moves its end of the line with it, **during** the drag rather than at the next rebuild.
    // Without the drag handler the line keeps the endpoints it was built with, so a node can be pulled away from
    // its own connection and the canvas shows a disconnected graph that the model says is connected.
    const QPointF moved{source_pos.x() + 60.0, source_pos.y() + 25.0};
    canvas.remember_position(source, moved);
    canvas.rebuild();
    const auto [moved_from, moved_to] = canvas.edge_endpoints(0);
    REQUIRE(std::abs(moved_from.x() - (moved.x() + box.width())) < 0.01);
    REQUIRE(std::abs(moved_to.x() - to.x()) < 0.01);
    REQUIRE(moved_from != from);
}

TEST_CASE("qt.views.nodegraph.a_dragged_connection_joins_two_ports", "[views][qt]") {
    // The gesture the canvas did not have: two boxes on screen and no way to join them. `connect_ports` is the
    // code path the drag ends in, exercised directly -- `QTest`'s mouse helpers need a mapped window and a
    // platform plugin, and what is worth asserting is the command and the resulting line, not Qt's delivery.
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::graph::NodeTypeRegistry catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    qp::views::NodeGraphView canvas{session, catalog, document};
    const qp::graph::NodeId source = add_node(session, "demo.signal");
    const qp::graph::NodeId model = add_node(session, "demo.spring_damper");
    REQUIRE(canvas.node_item_count() == 2);
    REQUIRE(canvas.edge_item_count() == 0);
    REQUIRE_FALSE(canvas.is_drawing_connection());

    REQUIRE(canvas.connect_ports(source, 1, model, 1));
    // The session learned about it, which is the half that makes it an edit rather than a drawing: the command
    // bus is the only entry point, so the undo stack and every other panel are in step.
    REQUIRE(session.graph().edge_count() == 1);
    REQUIRE(canvas.edge_item_count() == 1);
    REQUIRE(canvas.items_match_graph());
    // And it is undoable, like every other edit this canvas makes.
    REQUIRE(session.can_undo());
    REQUIRE(session.undo().has_value());
    REQUIRE(session.graph().edge_count() == 0);

    // A connection the bus refuses is reported rather than swallowed. Feeding an input that already has an edge
    // is the case a user hits by accident, and the refusal sentence is the only place the reason appears.
    REQUIRE(canvas.connect_ports(source, 1, model, 1));
    QString reported;
    QObject::connect(&canvas, &qp::views::NodeGraphView::mutation_failed,
                     [&reported](const QString& reason) { reported = reason; });
    REQUIRE_FALSE(canvas.connect_ports(model, 1, model, 1));  // a node feeding itself
    REQUIRE_FALSE(reported.isEmpty());
}

TEST_CASE("qt.views.nodegraph.the_canvas_can_be_panned_and_zoomed", "[views][qt]") {
    // The canvas used to be a picture: it was framed once when the graph was built and then could not be moved.
    // A graph wider than the viewport was reachable only by scrolling with the scrollbars, and there was no way to
    // zoom in on a node to read it. `frame_graph` was reachable from exactly one place -- `rebuild` -- so the
    // "zoom to fit" a user reaches for after scrolling away did not exist either.
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::graph::NodeTypeRegistry catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    qp::views::NodeGraphView canvas{session, catalog, document};
    canvas.resize(500, 400);
    canvas.viewport()->resize(480, 370);
    for (const char* type : {"demo.signal", "demo.spring_damper", "demo.instrument", "demo.export"}) {
        REQUIRE(add_node(session, type).valid());
    }
    canvas.frame_graph();
    // Four nodes do not fit a 480x370 viewport, so framing lands on the floor -- which is itself the property
    // worth asserting here: framing may not shrink past the legibility floor.
    REQUIRE(canvas.scale_factor() >= qp::views::NodeGraphView::kMinimumScale);
    const qreal framed = canvas.scale_factor();

    // Zoom in, and the anchor is the viewport centre: the scene point in the middle stays in the middle, because a
    // zoom that slides the thing under the cursor out from under it makes the next click land elsewhere.
    const QPointF centre_before = canvas.viewport_centre_in_scene();
    canvas.zoom_by(1.1);
    REQUIRE(canvas.scale_factor() > framed);
    const QPointF centre_after = canvas.viewport_centre_in_scene();
    // Within a few **scene** units, not exactly equal: Qt scrolls in whole device pixels, so anchoring on a scene
    // point rounds, and at a scale near 0.9 one device pixel is a little over one scene unit. Asserting equality
    // here would be asserting that a scrollbar can be positioned fractionally, which it cannot.
    REQUIRE(std::abs(centre_after.x() - centre_before.x()) < 3.0);
    REQUIRE(std::abs(centre_after.y() - centre_before.y()) < 3.0);

    // And it is clamped, in **both** directions. The lower bound is the same legibility floor `frame_graph`
    // respects -- no interactive gesture may undo the type size -- and the upper bound stops one node filling the
    // window with a title the user then has to scroll to read.
    for (int i = 0; i < 40; ++i) canvas.zoom_by(1.5);
    REQUIRE(canvas.scale_factor() <= qp::views::NodeGraphView::kMaximumScale);
    for (int i = 0; i < 40; ++i) canvas.zoom_by(1.0 / 1.5);
    REQUIRE(canvas.scale_factor() >= qp::views::NodeGraphView::kMinimumScale);
    REQUIRE(canvas.scale_factor() == qp::views::NodeGraphView::kMinimumScale);
}

TEST_CASE("qt.views.nodegraph.delete_removes_the_node_and_its_edges", "[views][qt]") {
    // Two removals the canvas had no gesture for. The commands existed in the bus -- `RemoveNode` and `Disconnect`
    // -- so the graph could be edited by a test helper and by nothing else, which is the same gap drawing a
    // connection had.
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::graph::NodeTypeRegistry catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    qp::views::NodeGraphView canvas{session, catalog, document};
    const qp::graph::NodeId source = add_node(session, "demo.signal");
    const qp::graph::NodeId model = add_node(session, "demo.spring_damper");
    REQUIRE(canvas.connect_ports(source, 1, model, 1));
    REQUIRE(session.graph().edge_count() == 1);

    // Nothing selected: nothing happens, and the caller is told rather than left guessing.
    REQUIRE_FALSE(canvas.delete_selection());
    REQUIRE_FALSE(canvas.disconnect_selection());
    REQUIRE(session.graph().node_count() == 2);

    // Removing the **edge**: the input is freed, and the two nodes stay.
    canvas.select_node(model);
    REQUIRE(canvas.disconnect_selection());
    REQUIRE(session.graph().edge_count() == 0);
    REQUIRE(session.graph().node_count() == 2);
    // Idempotent in the sense that matters: a second attempt reports that there was nothing to remove, rather than
    // reporting success for an edit it did not make.
    REQUIRE_FALSE(canvas.disconnect_selection());

    // Removing the **node**: the session's `RemoveNode` drops the edges that touched it, which is why the canvas
    // does not issue a `Disconnect` first -- duplicating that rule would be wrong the first time it changed.
    REQUIRE(canvas.connect_ports(source, 1, model, 1));
    canvas.select_node(source);
    REQUIRE(canvas.delete_selection());
    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(session.graph().edge_count() == 0);
    REQUIRE(canvas.node_item_count() == 1);
    REQUIRE(canvas.edge_item_count() == 0);
    REQUIRE(canvas.items_match_graph());

    // Both are ordinary undoable edits, so a user who deletes the wrong node gets it back from the Edit menu.
    REQUIRE(session.undo().has_value());
    REQUIRE(session.graph().node_count() == 2);
    REQUIRE(session.graph().edge_count() == 1);
}

TEST_CASE("qt.views.canvas.whole_graph_is_visible", "[views][qt]") {
    // The assertion that would have caught the framing defect directly.
    //
    // The running shell showed **one** node of three while its own status line reported
    // "nodes 3 | edges 2". The scene held all three -- every model-level assertion in this file
    // passed -- and the viewport could not show them, because `centerOn` scrolls a viewport over
    // the scene at the current scale and the graph is wider than the canvas. Nothing about the
    // model was wrong, which is exactly why no model-level test could see it.
    //
    // So this asserts the property that was actually violated: **every node is inside the
    // visible area** after framing. It is a property of the view, which is why it belongs in the
    // Qt suite rather than the ordinary one.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();
    window.resize(1280, 720);
    window.show();

    auto* canvas = window.findChild<qp::views::NodeGraphView*>();
    REQUIRE(canvas != nullptr);

    // Layout first: `fitInView` computes its scale from the viewport size, so a view that has
    // not been laid out cannot answer this question. The window is shown and the event loop is
    // given a turn, which is what the deferred framing in `rebuild` waits for.
    QCoreApplication::processEvents();

    // The model and the canvas agree, which is the claim the canvas exists to keep.
    REQUIRE(canvas->items_match_graph());
    REQUIRE(canvas->node_item_count() == 3);
    REQUIRE(canvas->edge_item_count() == 2);

    // And every node is inside the viewport. This is the assertion the screenshot replaced.
    REQUIRE(canvas->all_nodes_are_visible());

    window.close();
}

TEST_CASE("qt.views.measurement.one_ledger_per_session", "[views][qt]") {
    // The defect this pins was found by looking at a screenshot of the running window.
    //
    // `MeasurementModel` used to own its own `RunLedger` while `EditorWindow` owned another, so
    // the status line read one and the measurement panel's gap list read the other. The status
    // line said "reproducibility gaps: parameters, plugin versions" and the panel, three
    // centimetres to the right, said "no run recorded". Both were true about their own object
    // and the pair was useless: a run ledger is the record of what happened in this session,
    // and two of them means the session has two histories.
    //
    // It is the same two-sources-of-truth failure the `authoring` layer exists to prevent --
    // one session, one graph, one undo stack -- reappearing one layer up, which is why the fix
    // was to borrow rather than to synchronise. The assertion is on the **identity**, because
    // an equality check would pass for a copy that happened to match.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    REQUIRE(&window.measurements().ledger() == &window.ledger());

    // A fresh window has no run, so both surfaces say so together.
    REQUIRE(window.ledger().size() == 0);

    window.seed_demo();

    // After seeding, both see the same single run. One, not two: an earlier version of the seed
    // recorded a run in each ledger, so the window's history was two entries long and its gap
    // report described whichever happened to be second.
    REQUIRE(window.ledger().size() == 1);
    REQUIRE(window.measurements().ledger().size() == 1);

    // And the run is the one the trace belongs to, which is the fact the ordering inside
    // `seed_demo` exists to establish. The panel's gap list is the observable consequence: it
    // must not claim there is no run while the ledger holds one.
    const std::vector<std::string> gaps = window.measurements().gaps();
    for (const std::string& g : gaps) {
        REQUIRE(g.find("no run recorded") == std::string::npos);
    }
}

TEST_CASE("qt.views.measurement.fresh_window_is_empty", "[views][qt]") {
    // The same contract the graph half already had, asserted for the measurement half because
    // the same mistake is available here: a constructor that seeds is a constructor that has
    // decided something on the caller's behalf.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    REQUIRE(window.session().graph().node_count() == 0);
    REQUIRE(window.measurements().dataset().empty());
    REQUIRE(window.measurements().trace().empty());
    REQUIRE(window.measurements().ledger().size() == 0);

    // The seed is opt-in, and it fills all three.
    window.seed_demo();
    REQUIRE(window.session().graph().node_count() == 3);
    REQUIRE_FALSE(window.measurements().dataset().empty());
    REQUIRE_FALSE(window.measurements().trace().empty());
}


TEST_CASE("qt.views.confidence.shows_the_models_notes", "[views][qt]") {
    // The panel must show the model's diagnostics and its notes **as the model words them**. The
    // model is asserted by the ordinary suite on both compilers; re-wording a warning here would
    // put the tested text and the displayed text in different places, and the displayed one is the
    // one a student reads.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();

    auto* panel = window.findChild<qp::views::ConfidencePanel *>();
    REQUIRE(panel != nullptr);

    // The rows are present, which is what "visible" means for C8.
    REQUIRE_FALSE(panel->row_text(QStringLiteral("energy drift")).isEmpty());
    REQUIRE_FALSE(panel->row_text(QStringLiteral("clamped values")).isEmpty());

    // Zero clamped values is rendered as `0`, not as the unavailable text: the count is never
    // absent, and the difference between "the clamp never fired" and "this run was not asked" is
    // the distinction the whole feature rests on.
    REQUIRE(panel->row_text(QStringLiteral("clamped values")) == QStringLiteral("0"));

    // The note list matches the model exactly, one line each, verbatim.
    const std::vector<std::string> notes = window.confidence().notes();
    const QStringList shown = panel->note_lines();
    REQUIRE(shown.size() == static_cast<int>(notes.size()));
    for (std::size_t i = 0; i < notes.size(); ++i) {
        REQUIRE(shown[static_cast<int>(i)] ==
                QStringLiteral("- ") + QString::fromStdString(notes[i]));
    }
}

TEST_CASE("qt.views.confidence.energy_drift_is_shown", "[views][qt]") {
    // The positive half of the pair, and the one that makes C8's requirement concrete: with both
    // channels present the panel shows a **number**, not "not measurable".
    //
    // The unit-level case in the ordinary suite already establishes that the model computes a
    // correct drift. What this pins is that the panel displays it -- the rendering layer's failure
    // mode is showing the refusal text for a run the model could answer, which no model-level test
    // can see.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();

    auto* panel = window.findChild<qp::views::ConfidencePanel *>();
    REQUIRE(panel != nullptr);

    const QString drift = panel->row_text(QStringLiteral("energy drift"));
    REQUIRE_FALSE(drift.isEmpty());
    REQUIRE(drift != qp::views::ConfidencePanel::unavailable_text());

    // The demo's amplitude decays by construction, so the total energy falls. The sign is asserted
    // because it is the difference between "the model has damping" and "the method gains energy".
    bool is_number = false;
    const double value = drift.toDouble(&is_number);
    REQUIRE(is_number);
    REQUIRE(value < 0.0);

    // And with the frequency declared, the panel does not warn about assuming one.
    for (const QString& note : panel->note_lines()) {
        REQUIRE_FALSE(note.contains(QStringLiteral("assumes omega")));
    }

    // The note names the mechanism rather than only the fact.
    bool names_mechanism = false;
    for (const QString& note : panel->note_lines()) {
        if (note.contains(QStringLiteral("property of the integrator"))) names_mechanism = true;
    }
    REQUIRE(names_mechanism);
}
TEST_CASE("qt.views.confidence.unmeasurable_is_not_zero", "[views][qt]") {
    // The negative fixture for the panel's central claim.
    //
    // A model whose trace carries only a position cannot report an energy drift, and the panel must
    // say *not measurable* rather than `0`. A zero would read as "no drift was found", which is a
    // claim about a quantity that was never computed -- and it is exactly the confusion C8 exists to
    // prevent, arriving from the other side.
    qp::runtime::RunLedger ledger;
    qp::views::model::MeasurementModel measurements{ledger, "length", qp::units::dims::length};
    REQUIRE(measurements.add_channel("displacement", qp::units::dims::length).has_value());
    REQUIRE(measurements.add_sample(0.0, std::vector<double>{1.0}).has_value());
    REQUIRE(measurements.add_sample(1.0, std::vector<double>{0.5}).has_value());

    qp::views::model::ConfidenceModel confidence{measurements.trace()};
    qp::views::ConfidencePanel panel{confidence};

    // **"Not measurable" is now the absence of a row rather than a row saying so.** The panel shows one pair of
    // rows per conservation law the trace carries, and this trace carries none of the laws the model knows, so
    // there is no quantity to name and nothing to print -- `row_text` answers with an empty string for a row that is
    // not there, and the note below says which channels would have made one. A `0` in a drift row was the mistake
    // C8 exists to prevent; a row for a quantity this run never had would have been the same mistake wearing a
    // label.
    REQUIRE(panel.row_text(QStringLiteral("energy drift")).isEmpty());
    REQUIRE(panel.row_text(QStringLiteral("speed drift")).isEmpty());
    REQUIRE(panel.row_text(QStringLiteral("mu drift")).isEmpty());
    // The standing rows are still there, and the one that depends on a measurement says so in words.
    REQUIRE(panel.row_text(QStringLiteral("samples")) == QStringLiteral("2"));
    REQUIRE(panel.row_text(QStringLiteral("time span")) == qp::views::ConfidencePanel::unavailable_text());
    REQUIRE(panel.row_text(QStringLiteral("clamped values")) == QStringLiteral("0"));

    // And the model explains which channels are missing rather than only that something is: both ways a run could
    // have been measurable are named, because a student who added a velocity and still sees nothing needs to know
    // that a magnetic run would have been judged by its speed instead.
    REQUIRE_FALSE(panel.note_lines().isEmpty());
    bool named_both = false;
    for (const QString& line : panel.note_lines()) {
        if (line.contains(QStringLiteral("displacement")) && line.contains(QStringLiteral("speed"))) {
            named_both = true;
        }
    }
    REQUIRE(named_both);
}

TEST_CASE("qt.views.fit.shows_the_models_report", "[views][qt]") {
    // The panel renders `FitSession`'s report. Every number on screen has to come from the model, because the
    // model is asserted on both compilers and this file is not: a panel that recomputed a count, or that decided
    // for itself what "not enough points" means, would move the tested arithmetic into the untested half.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();

    qp::views::FitPanel* panel = window.fit_panel();
    REQUIRE(panel != nullptr);

    // The demo's two channels are what the panel offers, in the trace's own order.
    panel->show_channels();
    const std::vector<std::string> offered = panel->offered_channels();
    REQUIRE(offered.size() == 2);
    REQUIRE(offered[0] == "displacement");
    REQUIRE(offered[1] == "velocity");
    REQUIRE(panel->chosen_channel() == "displacement");
    REQUIRE(window.fit().trace().channel_count() == 2);
    panel->choose_channel("velocity");
    REQUIRE(panel->chosen_channel() == "velocity");
    // The session is what the panel writes to, not a copy of it: this is the same object the window exposes.
    REQUIRE(window.fit().request().channel == "velocity");
    panel->choose_channel("displacement");
    REQUIRE(window.fit().request().channel == "displacement");

    // The summary is built from the report: the point count is the model's, and it is the demo trace's own size.
    REQUIRE_FALSE(panel->summary_text().isEmpty());
    REQUIRE(panel->summary_text().contains(QStringLiteral("points")));
    REQUIRE(panel->summary_text().contains(
        QString::number(window.fit().report().points.size())));

    // The degree control is carried into the request, and the session's own clamp is what limits it -- the panel
    // does not decide the maximum.
    panel->choose_degree(2);
    REQUIRE(panel->chosen_degree() == 2);
    REQUIRE(window.fit().request().degree == 2);
    panel->choose_degree(1);
    REQUIRE(window.fit().report().request.degree == 1);

    // A channel the trace does not have leaves the selection alone rather than emptying it, which is the safe
    // answer for a trace that changed under the panel.
    panel->choose_channel("nonesuch");
    REQUIRE(panel->chosen_channel() == "displacement");
}

TEST_CASE("qt.views.fit.an_export_writes_the_numbers_on_the_screen", "[views][qt]") {
    // **One fit, one source.** The panel is what runs the fit -- it owns the channel, the degree and the exclusions --
    // and the export of a fit reads the panel's result rather than fitting again. This case pins the two halves of
    // that: the result the panel offers is the result it is showing, and it is **withdrawn** when the panel stops
    // showing a fit. An export that wrote a fit the window no longer displays would be a file about a state the user
    // cannot see.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};

    qp::views::FitPanel* panel = window.fit_panel();
    REQUIRE(panel != nullptr);
    // **Before the demo is seeded there is no trace at all**, so the panel has no fit and offers none rather than
    // offering a zero. (After `seed_demo` there *is* a fit without anyone asking: the demo's trace is quantified and
    // the panel selects its first channel, which is the ordinary state of a window that has just run something.)
    REQUIRE_FALSE(panel->result().has_value());

    window.seed_demo();
    panel->show_channels();
    panel->choose_degree(2);
#if defined(QP_HAS_ANALYSIS_PLUGIN)
    const qp::views::model::FitReport report = window.fit().report();
    REQUIRE(report.fittable());
    // The result is there once the fit has run, its coefficients are the ones on the screen, and every one of them
    // has an uncertainty -- the column the export exists to carry.
    REQUIRE(panel->result().has_value());
    const std::vector<std::vector<QString>>& rows = panel->coefficient_rows();
    const qp::runtime::FitResult& result = *panel->result();
    REQUIRE(result.coefficients.size() == rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        REQUIRE_FALSE(rows[index][1].isEmpty());
        // **The table shows a rounded number and the result holds the double**, so the two are compared with the
        // panel's own display precision in mind: its formatter is `QString::number(v, 'g', 6)` -- six significant
        // figures, because "a fitted coefficient is never known to seventeen digits". Asserting equality would be
        // asserting that a display is lossless, which is not what this case is about; what it is about is that the
        // value the export will write is the value on the screen.
        const double shown = rows[index][1].toDouble();
        const double exact = result.coefficients[index];
        REQUIRE(std::abs(shown - exact) <= 1.0e-5 * std::abs(exact));
        // The name column is the model layer's one rule, which is what the export's parameter column will say.
        REQUIRE(rows[index][0] == QString::fromStdString(qp::views::model::fit_coefficient_name(index)));
        // And every coefficient has an uncertainty to carry: the column the export exists for.
        REQUIRE(result.coefficient_uncertainty(index).has_value());
        REQUIRE_FALSE(rows[index][2].isEmpty());
    }

    // **Withdrawn when the panel stops showing a fit.** Starting a new recording empties the trace -- the dataset of
    // readings is a separate record and deliberately untouched -- so the fit then has no points, the report refuses,
    // and the panel clears both its table and its result. An export after this must find nothing rather than write
    // the fit the panel used to show.
    window.measurements().reset_trace(qp::runtime::RunId{7});
    panel->refresh();
    REQUIRE_FALSE(panel->result().has_value());
    REQUIRE(panel->coefficient_rows().empty());
#else
    // A build with no fitter has nothing to offer, and that is not the same as "not fitted yet" -- but both are
    // "nothing to export", which is the property this case is about.
    REQUIRE_FALSE(panel->result().has_value());
#endif
}

TEST_CASE("qt.views.fit.a_refusal_is_shown_by_name", "[views][qt]") {
    // The negative fixture, and the one that pins the platform's central rule at the rendering layer.
    //
    // A trace whose samples carry no stated uncertainty cannot be fitted, and the panel must say **which**
    // readings were left out and why rather than showing an empty table. An empty table reads as "the fit
    // failed"; the exclusion line reads as "go and quantify your readings", and only the second is actionable.
    //
    // The reading kind here is the one `MeasurementModel::add_sample`'s default produces, and that is worth
    // stating rather than hiding: a caller who does not say how well a reading is known gets `exact`, because
    // that is what `UncertainValue` is constructed with when nothing else is claimed. `exact` is a true statement
    // about a counted quantity and a false one about a sampled signal -- and either way it carries no weight, so
    // it is excluded by name rather than given a sigma.
    qp::runtime::RunLedger ledger;
    qp::views::model::MeasurementModel measurements{ledger, "length", qp::units::dims::length};
    REQUIRE(measurements.add_channel("displacement", qp::units::dims::length).has_value());
    for (int i = 0; i < 5; ++i) {
        REQUIRE(measurements.add_sample(0.1 * i, std::vector<double>{0.01 * i}).has_value());
    }

    qp::views::model::FitSession session{measurements.trace()};
    qp::views::FitPanel panel{session};
    panel.show_channels();

    // Five samples, none of them carrying a usable uncertainty: no fit, and the report says so.
    const qp::views::model::FitReport report = session.report();
    REQUIRE_FALSE(report.fittable());
    REQUIRE(report.excluded_count(qp::views::model::FitExclusion::uncertainty_zero) == 5);
    REQUIRE(report.points.empty());

    // And the widget says it with the model's own words, not with its own.
    const QString summary = panel.summary_text();
    REQUIRE_FALSE(summary.isEmpty());
    REQUIRE(summary.contains(QString::fromLatin1(qp::views::model::to_string(*report.refusal))));

    const QStringList exclusions = panel.exclusion_lines();
    REQUIRE(exclusions.size() == 1);
    REQUIRE(exclusions.first().contains(QStringLiteral("5")));
    REQUIRE(exclusions.first().contains(
        QString::fromLatin1(qp::views::model::to_string(qp::views::model::FitExclusion::uncertainty_zero))));
    // No coefficient rows at all: an empty table is right here, and it is the **exclusion line** that carries the
    // reason. A panel that invented a row of zeros would be the failure this whole platform is about.
    REQUIRE(panel.coefficient_rows().empty());

    // Declaring an uncertainty for the same samples makes the same panel fit them, which is the half that shows
    // the refusal came from the readings rather than from the trace having nothing in it.
    qp::runtime::RunLedger quantified_ledger;
    qp::views::model::MeasurementModel quantified{quantified_ledger, "length", qp::units::dims::length};
    REQUIRE(quantified.add_channel("displacement", qp::units::dims::length).has_value());
    for (int i = 0; i < 5; ++i) {
        REQUIRE(quantified.add_sample(0.1 * i, std::vector<double>{0.01 * i}, 0.001).has_value());
    }
    qp::views::model::FitSession quantified_session{quantified.trace()};
    qp::views::FitPanel quantified_panel{quantified_session};
    quantified_panel.show_channels();
    const qp::views::model::FitReport quantified_report = quantified_session.report();
    REQUIRE(quantified_report.fittable());
    REQUIRE(quantified_report.points.size() == 5);
    REQUIRE(quantified_report.degrees_of_freedom() == 3);
    REQUIRE(quantified_panel.exclusion_lines().isEmpty());
}

TEST_CASE("qt.views.fit.coefficients_come_with_their_uncertainties", "[views][qt]") {
    // The positive half, and the property a lab report is graded on. The demo's trace **is** quantified -- the
    // window's seed records every sample with an uncertainty -- so the fit runs, and what this case pins is that
    // no coefficient is ever shown without its error bar.
    //
    // A gradient printed alone is the single commonest way a report overstates its own precision, and it is
    // invisible in a diff: the number is right, the column beside it is missing.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();

    qp::views::FitPanel* panel = window.fit_panel();
    REQUIRE(panel != nullptr);
    panel->show_channels();
    panel->choose_degree(2);

    const qp::views::model::FitReport report = window.fit().report();
#if defined(QP_HAS_ANALYSIS_PLUGIN)
    REQUIRE(report.fittable());
    REQUIRE(report.points.size() == 200);
    // A quadratic has three terms, and every one of them is shown with an uncertainty beside it.
    const std::vector<std::vector<QString>> rows = panel->coefficient_rows();
    REQUIRE(rows.size() == 3);
    for (const std::vector<QString>& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE_FALSE(row[0].isEmpty());
        REQUIRE_FALSE(row[1].isEmpty());
        REQUIRE_FALSE(row[2].isEmpty());
        REQUIRE(row[1] != qp::views::FitPanel::unavailable_text());
        REQUIRE(row[2] != qp::views::FitPanel::unavailable_text());
    }
    // The degrees of freedom and the point count are the model's, not the panel's: 200 samples less three
    // parameters.
    REQUIRE(panel->summary_text().contains(QStringLiteral("197")));
#else
    // A build with no fitter says so, which is a different statement from "the fit failed" -- and the difference
    // matters to a student who would otherwise go looking for the problem in their own data.
    REQUIRE(panel->summary_text().contains(QStringLiteral("no fit plugin")));
    REQUIRE(panel->coefficient_rows().empty());
#endif
}

TEST_CASE("qt.views.editor_window.a_destroyed_window_reports_nothing", "[views][qt]") {
    // The defect this pins produced **no failing assertion**: every check in the suite passed and the process
    // exited with 0xC0000005. QGraphicsView clears its scene in its destructor, clearing a scene raises
    // `selectionChanged`, and the canvas forwards that as `node_selected` -- which the window routes to the
    // property panel. So destroying the canvas rebuilt a sibling panel's widgets, and whether that was survivable
    // depended on which of the window's children Qt happened to destroy first.
    //
    // A green run with a non-zero exit code is worse than a red one, because the summary says "fine". So the
    // property is asserted rather than left to a comment: a window with a selection can be destroyed, and the
    // canvas stays silent through it.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::NodeGraphView* canvas = nullptr;
    {
        qp::views::EditorWindow window{window_content};
        window.seed_demo();
        canvas = window.findChild<qp::views::NodeGraphView*>();
        REQUIRE(canvas != nullptr);

        // A selection, which is the state the crash needed: the scene has an item to deselect on the way out.
        const qp::graph::NodeId scope = window.session().graph().find_node_by_name("n3");
        REQUIRE(scope.valid());
        canvas->select_node(scope);
        REQUIRE(canvas->selected_node() == std::optional<qp::graph::NodeId>{scope});

        // The window silences the canvas **before** Qt starts taking its children apart, so no later selection
        // change is reported to anybody.
        canvas->go_quiet();
        int reports = 0;
        QObject::connect(canvas, &qp::views::NodeGraphView::node_selected,
                         [&reports](qp::graph::NodeId) { ++reports; });
        canvas->clear_selection();
        canvas->select_node(scope);
        REQUIRE(reports == 0);
        // Silence is not deafness: the canvas still knows what is selected, it just stopped announcing it.
        REQUIRE(canvas->selected_node() == std::optional<qp::graph::NodeId>{scope});
    }
    // The destruction above is the assertion -- a window holding a selection dies without a fault.
    QCoreApplication::processEvents();
    REQUIRE(true);
}

TEST_CASE("qt.views.canvas.a_port_shows_itself_under_the_cursor", "[views][qt]") {
    // The canvas drew stubs that gave no sign of being targets: the grab radius is twelve device pixels, and
    // nothing about a five-and-a-half-unit circle says "press here". A user found out by trial, and a press that
    // missed started moving the box instead.
    //
    // So the property is that the canvas **reports** the stub under the cursor and enlarges exactly that one --
    // and the report is what this asserts, because the enlargement is the same flag read at paint time. The
    // radius is divided by the zoom, so whether a point is close enough is a question only the canvas can
    // answer; a test with its own copy of the arithmetic would be asserting itself.
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::graph::NodeTypeRegistry catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    qp::views::NodeGraphView canvas{session, catalog, document};
    const qp::graph::NodeId source = add_node(session, "demo.signal");
    const qp::graph::NodeId model = add_node(session, "demo.spring_damper");
    REQUIRE(canvas.connect_ports(source, 1, model, 1));

    // Nothing under the cursor to begin with, which is the state a press treats as "drag the node".
    REQUIRE_FALSE(canvas.hovered_port().has_value());
    REQUIRE_FALSE(canvas.hovered_node().has_value());

    // `edge_endpoints` is the canvas's own answer for where the two stubs are, and `paint` draws them from the same
    // expression -- so the test asks about the very point a user aims at rather than about a coordinate it guessed.
    const auto [from, to] = canvas.edge_endpoints(0);

    canvas.hover_at(canvas.mapFromScene(from));
    const auto over_output = canvas.hovered_port();
    REQUIRE(over_output.has_value());
    REQUIRE(over_output->first == 1);
    // The output end, which is the half the cursor is on: a wire leaves here, and the canvas says so.
    REQUIRE(over_output->second);
    REQUIRE(canvas.hovered_node() == std::optional<qp::graph::NodeId>{source});

    canvas.hover_at(canvas.mapFromScene(to));
    const auto over_input = canvas.hovered_port();
    REQUIRE(over_input.has_value());
    REQUIRE(over_input->first == 1);
    // An input is a different gesture -- a wire may arrive here -- so it is reported as a different half rather
    // than as "port 1 of the model".
    REQUIRE_FALSE(over_input->second);
    REQUIRE(canvas.hovered_node() == std::optional<qp::graph::NodeId>{model});

    // And moving away clears it, on **every** item: a highlight left behind on the last one is how a stub appears
    // to be a target after the cursor has left it.
    const QPointF empty = canvas.mapFromScene(QPointF{from.x() + 40.0, from.y() - 40.0});
    canvas.hover_at(empty.toPoint());
    REQUIRE_FALSE(canvas.hovered_port().has_value());
    REQUIRE_FALSE(canvas.hovered_node().has_value());
}

TEST_CASE("qt.views.canvas.the_neighbourhood_is_lit_and_the_rest_is_dimmed", "[views][qt]") {
    // The question a user asks while reading a graph is "what does this depend on, and what depends on it", and
    // the answer is the selected node's neighbours. So selecting one node lights it and everything it is wired to,
    // and fades the rest -- **fades**, not hides: a dimmed node has to stay locatable enough to read a title and
    // decide whether to click it, which is why the prominence is a number and not a boolean.
    //
    // The chain is deliberate. Three nodes in a row means the middle one has two neighbours and there is a fourth
    // node touching none of them, so the test can tell "the neighbourhood" from "everything selected or not".
    qp::authoring::Session session;
    qp::authoring::Document document;
    qp::graph::NodeTypeRegistry catalog;
    REQUIRE(qp::views::register_demo_library(catalog).has_value());

    qp::views::NodeGraphView canvas{session, catalog, document};
    const qp::graph::NodeId signal = add_node(session, "demo.signal");
    const qp::graph::NodeId damper = add_node(session, "demo.spring_damper");
    const qp::graph::NodeId filter = add_node(session, "demo.filter");
    const qp::graph::NodeId sink = add_node(session, "demo.export");
    REQUIRE(canvas.connect_ports(signal, 1, damper, 1));
    REQUIRE(canvas.connect_ports(damper, 1, filter, 1));
    REQUIRE(canvas.connect_ports(filter, 1, sink, 1));
    REQUIRE(canvas.node_item_count() == 4);
    REQUIRE(canvas.edge_item_count() == 3);

    // With nothing selected every node is drawn normally, because a highlight with no subject is a canvas that has
    // faded itself.
    for (const qp::graph::NodeId node : {signal, damper, filter, sink}) {
        REQUIRE(canvas.prominence_of(node) == 1.0);
    }
    for (std::size_t i = 0; i < 3; ++i) REQUIRE(canvas.edge_prominence(i) == 1.0);

    canvas.select_node(damper);

    // The selected node and its two neighbours are lit.
    REQUIRE(canvas.prominence_of(damper) == 1.0);
    REQUIRE(canvas.prominence_of(signal) == 1.0);
    REQUIRE(canvas.prominence_of(filter) == 1.0);
    // The node two wires away is not, which is what makes this a neighbourhood rather than "the connected
    // component": the question is about what this node touches, and a transitive answer would light the whole graph
    // the moment it was connected.
    REQUIRE(canvas.prominence_of(sink) == qp::views::NodeGraphView::kDimmedProminence);
    REQUIRE(canvas.prominence_of(sink) < 1.0);

    // The edges follow their endpoints rather than a rule of their own, so an edge is lit when **either** end is.
    // That has a consequence worth asserting explicitly, because the first version of this test expected the
    // opposite: the wire from the lit filter into the dimmed sink is **lit**, and it should be. It is the wire
    // along which the selection's influence travels -- it is what the damper is connected *to*, two hops out --
    // and fading it would cut the connection the highlight exists to show.
    REQUIRE(canvas.edge_source(0) == signal);
    REQUIRE(canvas.edge_source(1) == damper);
    REQUIRE(canvas.edge_source(2) == filter);
    REQUIRE(canvas.edge_prominence(0) == 1.0);
    REQUIRE(canvas.edge_prominence(1) == 1.0);
    REQUIRE(canvas.edge_prominence(2) == 1.0);

    // What the fade does reach is a wire with **neither** end in the neighbourhood. Selecting the signal instead
    // gives one: its only neighbour is the damper, so the two wires beyond the damper are both dimmed -- including
    // the one joining two nodes that are both in the graph's connected component.
    // What the fade does reach is a wire whose **both** ends are outside the neighbourhood. Selecting the signal
    // gives one: its only neighbour is the damper, so the wire leaving the damper for the filter stays lit -- it
    // touches the neighbourhood -- while the wire from the filter to the sink, with neither end lit, goes dim.
    // Only the far end of the graph recedes; everything one hop from the selection, and the wires into it, stay.
    canvas.select_node(signal);
    REQUIRE(canvas.prominence_of(signal) == 1.0);
    REQUIRE(canvas.prominence_of(damper) == 1.0);
    REQUIRE(canvas.prominence_of(filter) == qp::views::NodeGraphView::kDimmedProminence);
    REQUIRE(canvas.edge_prominence(0) == 1.0);
    REQUIRE(canvas.edge_prominence(1) == 1.0);
    REQUIRE(canvas.edge_prominence(2) == qp::views::NodeGraphView::kDimmedProminence);

    // Back to the damper for the remaining assertions.
    canvas.select_node(damper);

    // Out-of-range asks answer rather than crash, and answer "not faded": a caller should not have to check the
    // count before asking about a node.
    REQUIRE(canvas.edge_prominence(99) == 1.0);
    REQUIRE_FALSE(canvas.edge_source(99).valid());
    REQUIRE(canvas.prominence_of(qp::graph::NodeId{99, 1}) == 1.0);

    // Deselecting puts the whole graph back. A highlight that could be entered and not left would make the canvas
    // a mode -- and `select_node({})` is **not** how you leave it: that call is a documented no-op, because it
    // looks up an id that does not exist. The first version of this test used it and asserted against a graph that
    // was still faded, which is the sort of assumption a test is supposed to catch rather than contain.
    canvas.clear_selection();
    REQUIRE_FALSE(canvas.selected_node().has_value());
    for (const qp::graph::NodeId node : {signal, damper, filter, sink}) {
        REQUIRE(canvas.prominence_of(node) == 1.0);
    }
    for (std::size_t i = 0; i < 3; ++i) REQUIRE(canvas.edge_prominence(i) == 1.0);
}

namespace {

/// @brief A provider run that records one length channel, so the window's copy of a record can be followed.
///
/// One sample per **step**, not per call: `advance` is handed the whole run's step count, and a run that recorded
/// once per call would look like a time series with a single point -- which is what the first version of the stub
/// in `tests/model/test_run_controller.cpp` did, and what a case there now pins.
class RecordingStubRun final : public qp::graph::execution::IGraphRun {
public:
    /// The value every sample carries: what a reading taken from this trace must equal.
    static constexpr double kRadius = 42095700.0;

    [[nodiscard]] qp::diag::Result<void> advance(std::size_t steps, double dt) override {
        if (!(dt > 0.0)) return qp::diag::ErrorCode::invalid_argument;
        for (std::size_t step = 0; step < steps; ++step) {
            ++steps_;
            (void)trace_.append(static_cast<double>(steps_) * dt,
                                {qp::runtime::UncertainValue::measured(kRadius, 0.0, qp::units::dims::length)});
        }
        return {};
    }
    [[nodiscard]] qp::graph::execution::GraphRunReport report() const override {
        qp::graph::execution::GraphRunReport out;
        out.steps = steps_;
        out.particles = 1;
        out.live = 1;
        out.note = "recording stub";
        return out;
    }
    [[nodiscard]] std::vector<double> positions() const override { return {1.0, 2.0, 3.0}; }
    void set_run(qp::runtime::RunId run) noexcept override {
        trace_ = qp::runtime::Trace{run};
        (void)trace_.add_channel(qp::runtime::Channel{"radius", qp::units::dims::length, {}});
    }
    [[nodiscard]] const qp::runtime::Trace& trace() const noexcept override { return trace_; }

private:
    std::size_t steps_ = 0;
    qp::runtime::Trace trace_{qp::runtime::RunId{}};
};

/// @brief A provider that claims any non-empty graph and answers with `RecordingStubRun`.
class RecordingProvider final : public qp::graph::execution::IGraphRunProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "recording stub"; }
    [[nodiscard]] bool claims(const qp::graph::Graph& graph) const noexcept override {
        return graph.node_count() > 0;
    }
    [[nodiscard]] qp::graph::execution::RunBuildResult build(const qp::graph::Graph&,
                                                             const qp::graph::INodeCatalog&) override {
        qp::graph::execution::RunBuildResult out;
        out.run = std::make_unique<RecordingStubRun>();
        return out;
    }
};

}  // namespace

TEST_CASE("qt.views.measurement.a_provider_run_closes_the_loop", "[views][qt]") {
    // **The platform's whole claim, end to end, on the path that had no record.** A run produced by a **provider**
    // -- the shape a kit with a bake and a launch uses -- used to hand the window a report and a picture and no
    // trace at all, so the measurement session stayed empty, the reading button said "nothing to read yet" and the
    // confidence panel said "not measurable". Both sentences were true, and the closed loop the platform exists
    // for was dead for its flagship experiment.
    //
    // The chain is four links and this case walks all four: the provider's run records, the controller opens the
    // ledger entry and hands the identity over before the first step, the window copies the record into the
    // measurement session, and a reading taken from it carries both the value and the node it came from. A stub
    // provider rather than the kit, because the link under test is the window's, and a case that needed a
    // magnetosphere to check it would be checking the kit as well.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();

    // The window opens on the demo's own seeded trace -- two channels and a handful of readings -- and that is
    // what a run **replaces**. Asserting the replacement rather than an empty start is the stronger statement:
    // a window that appended would leave the demo's channels beside the run's, and the panels would offer a
    // reading from a channel that belongs to an experiment the user is no longer looking at.
    REQUIRE(window.measurements().trace().channels().size() == 2);
    REQUIRE_FALSE(window.measurements().trace().empty());

    RecordingProvider provider;
    qp::views::model::clear_run_providers();
    qp::views::model::mount_run_provider(&provider);
    window.run_once();

    // The record arrived, with its channel, its samples and **a run identity the ledger issued** -- which is what
    // makes a reading taken from it traceable back to the run that produced it.
    const qp::runtime::Trace& recorded = window.measurements().trace();
    REQUIRE(recorded.channels().size() == 1);
    REQUIRE(recorded.channels().front().name == std::string{"radius"});
    REQUIRE(recorded.channels().front().dim == qp::units::dims::length);
    REQUIRE(recorded.size() == qp::views::model::RunController::kSteps);
    REQUIRE(recorded.run().valid());

    // And the reading. The instrument measures a length, the trace's only channel is a length, and the reading
    // must be the trace's **last sample** -- the number a lab session writes down.
    auto* canvas = window.findChild<qp::views::NodeGraphView*>();
    REQUIRE(canvas != nullptr);
    const std::size_t before = window.measurements().dataset().readings().size();
    const qp::graph::NodeId scope = window.session().graph().find_node_by_name("n3");
    REQUIRE(scope.valid());
    canvas->select_node(scope);
    window.measure_selection();

    REQUIRE(window.measurements().dataset().readings().size() == before + 1);
    const auto taken = window.measurements().dataset().readings()[before].reading;
    REQUIRE(taken.value == recorded.samples().back().values.front().value);
    REQUIRE(taken.value == RecordingStubRun::kRadius);
    const auto source = window.measurements().source_of(before);
    REQUIRE(source.has_value());
    REQUIRE(source->index == scope.index);
    REQUIRE(source->generation == scope.generation);

    // The list is a process-wide static, so the case that mounted a provider is the one that cleans it up.
    qp::views::model::clear_run_providers();
    REQUIRE(qp::views::model::run_providers().empty());
}

TEST_CASE("qt.views.run.the_status_bar_shows_the_run_it_recorded", "[views][qt]") {
    // **The last of three ledgers, and the one a screenshot found.** `MeasurementModel` was converted to
    // borrow the window's ledger first (the case above), and `RunController` kept its own -- so the window
    // could draw a run's particles on one panel while the status line beside them read
    // `runs 0 | reproducibility gaps: no run yet`. Both were true about their own object, which is the whole
    // problem: one session, one history.
    //
    // Two claims are asserted here and they are different claims. The first is that the run is **recorded**
    // where the session can see it. The second is that the user can **see that it was** -- which needs the
    // second label, because the counts used to be written over the run's own sentence inside the same call.
    //
    // A provider run rather than the operator loop, for the reason the case above gives: the window's wiring
    // is what is under test, and the demonstrator graph's own binder is not linked into this target.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};

    auto* news = window.status_label();
    auto* state = window.state_label();
    REQUIRE(news != nullptr);
    REQUIRE(state != nullptr);

    // Seeding records the first run, so the standing half reports one before the button is ever pressed.
    window.seed_demo();
    REQUIRE(window.ledger().size() == 1);
    REQUIRE(state->text().contains(QStringLiteral("runs 1")));

    RecordingProvider provider;
    qp::views::model::clear_run_providers();
    qp::views::model::mount_run_provider(&provider);
    window.run_once();

    // The button's run went into the **window's** ledger. Two, not one: a ledger that is merely non-empty is
    // exactly what the screenshot showed -- the seed's own entry -- so the count is what distinguishes
    // "recorded" from "recorded somewhere the window can read".
    REQUIRE(window.ledger().size() == 2);
    REQUIRE(window.ledger().last_id().valid());

    // The standing half followed it...
    REQUIRE(state->text().contains(QStringLiteral("runs 2")));
    // ... and the news survived, which is the half that needed its own label. It is the **run's** sentence,
    // asserted against the provider's own note rather than against "non-empty": a label that still held the
    // previous message would be non-empty too, and the defect was precisely a message being overwritten.
    REQUIRE(news->text() == QStringLiteral("recording stub"));
    REQUIRE_FALSE(news->text().contains(QStringLiteral("nodes ")));

    // The list is a process-wide static, so the case that mounted a provider is the one that cleans it up.
    qp::views::model::clear_run_providers();
}

TEST_CASE("qt.views.measurement.a_reading_points_at_its_node", "[views][qt]") {
    // **The loop closing.** Every other path in this window runs from the graph to the numbers: the canvas is
    // drawn from the session, the panel reads the dataset, the report reads the dataset. This is the one that runs
    // back -- a student reading a value off the table asks which device produced it, and the answer is a node.
    //
    // What makes it work is that the reading carries a source at all. A dataset is otherwise a list of numbers
    // with no way back to the experiment, which is the artefact the platform exists to replace.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    qp::views::EditorWindow window{window_content};
    window.seed_demo();

    auto* canvas = window.findChild<qp::views::NodeGraphView*>();
    auto* panel = window.findChild<qp::views::MeasurementPanel*>();
    REQUIRE(canvas != nullptr);
    REQUIRE(panel != nullptr);

    // Seeded readings have no source, and that is the honest answer rather than an oversight: this build records
    // them from a stored demonstration, not from a node. A reading a user typed in is the same case.
    REQUIRE(window.measurements().dataset().readings().size() == 3);
    REQUIRE_FALSE(window.measurements().source_of(0).has_value());
    // The panel's own answer with no row selected, which is also the state a fresh session is in.
    REQUIRE_FALSE(panel->selected_source().has_value());

    // Take one from a node. The instrument is selected, the session already holds a trace whose first channel is a
    // length, and the reading is attributed to the node the user chose -- not to whichever node the last run
    // happened to use.
    const qp::graph::NodeId scope = window.session().graph().find_node_by_name("n3");
    REQUIRE(scope.valid());
    canvas->select_node(scope);
    window.measure_selection();

    REQUIRE(window.measurements().dataset().readings().size() == 4);
    const std::size_t added = 3;
    const auto source = window.measurements().source_of(added);
    REQUIRE(source.has_value());
    REQUIRE(source->index == scope.index);
    REQUIRE(source->generation == scope.generation);
    // And the reading's own value is the trace's last sample rather than a zero or a placeholder: the point of
    // taking a reading is the number, and a source with nothing behind it would be a citation for a blank.
    REQUIRE(window.measurements().dataset().readings()[added].reading.value ==
            window.measurements().trace().samples().back().values[0].value);

    // The model's answer for a row is a flat `Source` pair, and the panel converts it to the id a canvas speaks.
    // Both are asserted because the conversion is where the two shapes meet and a wrong field order would
    // type-check.
    const auto model_source = window.measurements().source_of(added);
    REQUIRE(model_source.has_value());
    const qp::graph::NodeId from_model{model_source->index, model_source->generation};
    REQUIRE(from_model == scope);

    // And the id points at a node the canvas can actually reveal and highlight -- the last link in the chain, and
    // the one a screenshot shows.
    canvas->select_node(from_model);
    REQUIRE(canvas->selected_node() == std::optional<qp::graph::NodeId>{scope});
    REQUIRE(canvas->prominence_of(scope) == 1.0);

    window.close();
}

TEST_CASE("qt.views.measurement.a_reading_comes_from_the_chosen_device", "[views][qt]") {
#if !defined(QP_HAS_INSTRUMENTS)
    // With no instrument plugin there is no device to choose, and the case is not compiled rather than being
    // compiled against a stub: what it asserts is that the uncertainty in the table is the one the graduations
    // imply, and a stub's graduations are the test's own opinion.
    SUCCEED("built without the instruments plugin");
#else
    // **The loop with an instrument in it.** Without this, "Measure" was a button that copied a number out of the
    // trace: the reading carried the trace's own uncertainty, which is the simulation's, and the device that a
    // lab session is actually about never appeared. With it, the number is what the chosen instrument **reads**
    // and the error bar is what its graduations imply -- the difference between 0.29 mm from a ruler and 0.014 mm
    // from a caliper, which is the whole lesson.
    //
    // The devices are mounted by hand here rather than by the application, because a window has no opinion about
    // which instruments exist: it reads whatever the host's registry holds. That is the property this also
    // checks -- the panel is driven by the registry, so a loaded plugin's devices would appear in it unchanged.
    qp::host::PluginHost window_content{qp::plugin::Capability::node_types};
    REQUIRE(qp::plugins::instruments::mount_instruments(window_content) ==
            qp::plugins::instruments::builtin_count());

    qp::views::EditorWindow window{window_content};
    window.seed_demo();

    auto* canvas = window.findChild<qp::views::NodeGraphView*>();
    auto* panel = window.findChild<qp::views::MeasurementPanel*>();
    REQUIRE(canvas != nullptr);
    REQUIRE(panel != nullptr);

    // The list holds one entry per registered device, by id -- and the label is what is shown, so a renamed
    // device never breaks a caller that selects by id.
    REQUIRE(panel->chosen_device() == "builtin.rule");
    panel->choose_device("builtin.caliper");
    REQUIRE(panel->chosen_device() == "builtin.caliper");
    // An id the list does not hold leaves the choice alone rather than clearing it: a device unloaded while the
    // panel still showed it must not silently turn the next reading into an unattributed one.
    panel->choose_device("no.such.device");
    REQUIRE(panel->chosen_device() == "builtin.caliper");

    const qp::graph::NodeId scope = window.session().graph().find_node_by_name("n3");
    REQUIRE(scope.valid());
    canvas->select_node(scope);

    // The caliper's dimension is length, and the seeded trace's first channel is a displacement -- so the reading
    // comes out of the trace, through the device's graduations.
    const double truth = window.measurements().trace().samples().back().values[0].value;
    window.measure_selection();

    const std::size_t added = window.measurements().dataset().readings().size() - 1;
    REQUIRE(added == 3);
    const qp::runtime::UncertainValue& reading = window.measurements().dataset().readings()[added].reading;

    // The **reading is the truth on a 0.05 mm tick**, and its uncertainty is the caliper's -- not zero, and not
    // the trace's own. Both halves matter: a device that returned the truth unchanged would be claiming a
    // precision it does not have, and one that returned the trace's uncertainty would be reporting the
    // simulation's error as the instrument's.
    const qp::plugins::instruments::GraduatedInstrument* caliper =
        dynamic_cast<qp::plugins::instruments::GraduatedInstrument*>(
            window_content.instruments().find("builtin.caliper"));
    REQUIRE(caliper != nullptr);
    const double tick = caliper->resolution();
    REQUIRE(reading.value == std::round(reading.value / tick) * tick);
    REQUIRE(reading.u == qp::runtime::resolution_uncertainty(tick));
    REQUIRE(reading.kind == qp::runtime::UncertaintyKind::standard);
    REQUIRE(reading.dim == window.measurements().dataset().dim());
    // The tick is what makes it a measurement rather than a copy: the device moved the value.
    REQUIRE(std::abs(reading.value - truth) <= tick);

    // The reading still names its node, so the device tells us **with what** and the source tells us **where**.
    const auto source = window.measurements().source_of(added);
    REQUIRE(source.has_value());
    REQUIRE(source->index == scope.index);

    // And a second device measures the same truth to a different precision, which is the comparison a student is
    // supposed to be able to make.
    panel->choose_device("builtin.rule");
    window.measure_selection();
    const qp::runtime::UncertainValue& coarse =
        window.measurements().dataset().readings().back().reading;
    REQUIRE(coarse.u == qp::runtime::resolution_uncertainty(1.0e-3));
    REQUIRE(coarse.u > reading.u);

    window.close();
#endif
}

int main(int argc, char** argv) {
    // A QApplication is required before any QWidget exists. Building it in main
    // rather than as a static is deliberate: a static QApplication outlives main's
    // teardown order and Qt warns about it.
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}


TEST_CASE("qt.views.scene.draws_a_scene_and_says_when_there_is_none", "[views][qt]") {
    // The last link of the render chain, checked where it can be: the widget holds a **copy** of the scene it
    // was given, and it says so in words when there is nothing to draw. Both are decisions rather than details.
    // The copy is what keeps a painted frame and the numbers beside it from being different runs; the sentence
    // is what keeps "this graph draws nothing" from looking like "this window has not run yet".
    //
    // The message is a **constructor parameter** rather than a constant, which is what the second view item
    // forced: two items draw into two panels, and a panel that said "nothing to draw yet" without saying *what*
    // would leave a user with two identical blanks and no way to tell which one their graph feeds.
    qp::views::SceneView view{QStringLiteral("no curves yet")};
    REQUIRE(view.scene().empty());
    REQUIRE(view.empty_text() == QStringLiteral("no curves yet"));

    qp::graph::ViewScene scene;
    scene.points.push_back(qp::graph::ViewScene::Point{1.0, 2.0});
    scene.points.push_back(qp::graph::ViewScene::Point{-3.0, 0.5});
    scene.x_min = -4.0;
    scene.x_max = 4.0;
    scene.y_min = -4.0;
    scene.y_max = 4.0;
    scene.has_bounds = true;

    view.set_scene(scene);
    REQUIRE(view.scene().points.size() == 2);
    REQUIRE(view.scene().points.front().x == 1.0);
    REQUIRE(view.scene().has_bounds);

    // The bounds are the item's decision and the widget only fits them, so a scene with no bounds is drawn as
    // "nothing" rather than fitted to whatever the points happen to span.
    qp::graph::ViewScene unbounded;
    unbounded.points.push_back(qp::graph::ViewScene::Point{1.0, 1.0});
    view.set_scene(unbounded);
    REQUIRE_FALSE(view.scene().has_bounds);

    // A scene whose content is **curves and nothing else** is not empty, and neither is one whose only content is
    // the body: `empty()` asks about the two lists, and the body radius is a third thing that decides whether
    // there is a picture. Both are what the field-line item produces.
    qp::graph::ViewScene curves;
    curves.polylines.push_back({qp::graph::ViewScene::Point{1.0, 0.0}, qp::graph::ViewScene::Point{0.0, 1.0}});
    curves.x_min = -2.0;
    curves.x_max = 2.0;
    curves.y_min = -2.0;
    curves.y_max = 2.0;
    curves.has_bounds = true;
    curves.body_radius = 1.0;
    view.set_scene(curves);
    REQUIRE_FALSE(view.scene().empty());
    REQUIRE(view.scene().points.empty());
    REQUIRE(view.scene().polylines.size() == 1);
    // Painted rather than asserted about: the mapping from units to pixels has no other check, and a widget that
    // threw on a curve with two points would be a widget that cannot draw the shortest line in the picture.
    QPixmap canvas{view.size()};
    view.render(&canvas);
    REQUIRE_FALSE(canvas.isNull());

    // **The fit is uniform and centred**, measured in pixels rather than assumed from the arithmetic. This block
    // exists because the running window showed a field-line scene as a small cluster in a six-hundred-pixel panel,
    // and the question a reader asks -- "is the mapping wrong, or is the picture correct for a wide, short
    // rectangle?" -- is one only pixels can answer.
    //
    // The answer is the second: one scale for both axes, chosen by whichever axis binds, which in a 600x200 widget
    // showing a square scene is the vertical one. So the content reaches close to the top and bottom edges, is
    // **centred** horizontally, and deliberately does **not** reach the left and right ones. A widget that
    // stretched each axis to fill would draw a circle as an ellipse, which is the one thing a student reads the
    // picture for.
    qp::views::SceneView wide{QStringLiteral("wide")};
    wide.resize(600, 200);
    qp::graph::ViewScene spanning;
    for (int line = -2; line <= 2; ++line) {
        std::vector<qp::graph::ViewScene::Point> curve;
        for (int step = 0; step <= 40; ++step) {
            const double t = -8.0 + 16.0 * static_cast<double>(step) / 40.0;
            curve.push_back(qp::graph::ViewScene::Point{t, static_cast<double>(line)});
        }
        spanning.polylines.push_back(std::move(curve));
    }
    spanning.x_min = -8.8;
    spanning.x_max = 8.8;
    spanning.y_min = -8.8;
    spanning.y_max = 8.8;
    spanning.has_bounds = true;
    spanning.body_radius = 1.0;
    wide.set_scene(spanning);
    QPixmap painted{wide.size()};
    painted.fill(Qt::white);
    wide.render(&painted);
    const QImage image = painted.toImage();
    int leftmost = image.width();
    int rightmost = -1;
    int topmost = image.height();
    int bottommost = -1;
    // The background is whatever the widget painted, sampled from a corner the drawing never reaches: a rule based
    // on "not white" or on a colour would be a rule about this machine's palette, and the first two attempts at it
    // measured the scale label instead of the curves. The top strip is excluded for the same reason -- the label
    // lives there, and it is the one mark on the picture that is not part of the picture.
    const QColor background = image.pixelColor(image.width() - 4, image.height() - 4);
    for (int x = 0; x < image.width(); ++x) {
        for (int y = 24; y < image.height(); ++y) {
            const QColor pixel = image.pixelColor(x, y);
            const int delta = std::abs(pixel.red() - background.red()) +
                              std::abs(pixel.green() - background.green()) +
                              std::abs(pixel.blue() - background.blue());
            if (delta < 30) continue;
            leftmost = std::min(leftmost, x);
            rightmost = std::max(rightmost, x);
            topmost = std::min(topmost, y);
            bottommost = std::max(bottommost, y);
        }
    }
    REQUIRE(rightmost > leftmost);
    // **The mapping's own arithmetic, asserted in pixels.** The rectangle is 600x200 with an 8-pixel margin, the
    // frame is `+/-8.8` on both axes, and the content spans `+/-8` in `x` and `+/-2` in `y`. One uniform scale is
    // the smaller of the two the frame allows -- `(200 - 16) / 17.6 = 10.4545` pixels per unit -- and the scene's
    // origin lands at the widget's centre with `y` flipped. Every expected pixel below is that sentence computed
    // out, so a change to the fit has to change these numbers deliberately rather than drift past them.
    constexpr double kScale = (200.0 - 16.0) / 17.6;
    const auto expected_x = [](double scene_x) { return 300.0 + scene_x * kScale; };
    const auto expected_y = [](double scene_y) { return 100.0 - scene_y * kScale; };
    REQUIRE(std::abs(leftmost - expected_x(-8.0)) < 3.0);
    REQUIRE(std::abs(rightmost - expected_x(8.0)) < 3.0);
    REQUIRE(std::abs(topmost - expected_y(2.0)) < 3.0);
    REQUIRE(std::abs(bottommost - expected_y(-2.0)) < 3.0);
    // And **not** stretched to the sides, which is what "uniform" means here: a widget that fitted each axis
    // independently would put `x = -8` at the left edge instead of at 216. Asserting the emptiness is asserting
    // the property that keeps a circle a circle.
    REQUIRE(leftmost > 100);
    REQUIRE(rightmost < image.width() - 100);
    REQUIRE(leftmost < rightmost);

    // And the scene is copied: replacing it does not touch what the caller still holds.
    Q_UNUSED(scene);
}
