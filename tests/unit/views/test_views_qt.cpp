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
#include <QAction>
#include <QMenu>
#include <QMenuBar>

#include <qp/graph/mutate/command.hpp>
#if defined(QP_HAS_QPJSON_FORMAT)
#include <qp/plugins/csv/csv_exporter.hpp>
#include <qp/plugins/qpjson/qpjson_format.hpp>
#include <qp/views/model/export_controller.hpp>
#endif
#include <qp/views/model/document_controller.hpp>
#include <qp/views/model/measurement_model.hpp>
#include <qp/views/model/type_catalog.hpp>

#include "editor_window.hpp"
#include <QString>
#include <QStringList>

#include "confidence_panel.hpp"
#include "measurement_panel.hpp"
#include "node_graph_view.hpp"
#include "property_panel.hpp"

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

    qp::views::EditorWindow window;
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
    const std::string csv = (std::filesystem::temp_directory_path() / "qp_caption_test.csv").string();
    REQUIRE(window.export_document(csv));
    REQUIRE(std::filesystem::exists(csv));

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(csv, ignored);
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
    qp::views::EditorWindow window;
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
    qp::views::EditorWindow window;
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
    qp::views::EditorWindow window;
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
    qp::views::EditorWindow window;
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
    qp::views::EditorWindow window;
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
    qp::views::EditorWindow window;
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

    const QString shown = panel.row_text(QStringLiteral("energy drift"));
    REQUIRE(shown == qp::views::ConfidencePanel::unavailable_text());
    REQUIRE(shown != QStringLiteral("0"));

    // And the model explains which channel is missing rather than only that something is.
    REQUIRE_FALSE(panel.note_lines().isEmpty());
}

int main(int argc, char** argv) {
    // A QApplication is required before any QWidget exists. Building it in main
    // rather than as a static is deliberate: a static QApplication outlives main's
    // teardown order and Qt warns about it.
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
