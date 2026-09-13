/**
 * @file editor_window.cpp
 * @brief Implementation of the editing window.
 */
#include "editor_window.hpp"

#include "node_graph_view.hpp"

#include "theme.hpp"
#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QSize>
#include <QToolBar>

#include <qp/views/model/demo_library.hpp>
#include <qp/views/model/execution_binders.hpp>
#include <qp/views/model/export_controller.hpp>

#include "scene_view.hpp"
#include "confidence_panel.hpp"
#include "fit_panel.hpp"
#include "measurement_panel.hpp"
#include "property_panel.hpp"

#include <qp/graph/mutate/command.hpp>
#include <qp/ports/value.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QDockWidget>
#include <QListWidget>
#include <QSplitter>
#include <QStatusBar>
#include <QString>
#include <QVBoxLayout>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <string>

namespace qp::views {
namespace {

/// @brief Initial width of the measurement dock, in logical pixels.
///
/// Wide enough for the readings table's four columns at QFont's default size, and no wider: the canvas is the
/// panel that cannot do its job without width, and a dock that takes more than it needs takes it from the
/// canvas.
///
/// It used to be `320`, which was not wide enough, and a screenshot of the running window is what said so: the
/// table's "status" column was outside the visible dock, so a session that had rejected a reading showed three
/// columns and no sign of it. The status column is the one that says whether a number counts -- the panel's
/// whole point one layer down -- so a width that hides it is a width that hides the feature.
constexpr int kDockWidth = 420;

/// @brief How much of the window's height the scene panels' area is given, in logical pixels.
///
/// The same kind of number as `kDockWidth` and measured the same way. It is a **request to `resizeDocks`** rather
/// than a minimum: a user who drags the splitter keeps what they dragged, and the value only decides what the
/// window opens with. 260 of a 720-pixel window leaves the canvas about four hundred, which is where a graph of
/// five nodes is still readable.
constexpr int kSceneHeight = 260;
}  // namespace

namespace {

/// @brief An error code as text, for the status line.
QString describe(qp::diag::ErrorCode code) {
    const std::string_view text = qp::diag::to_string(code);
    return QString::fromLatin1(text.data(), static_cast<int>(text.size()));
}

}  // namespace

/// @brief Keeps the status line in step with the session.
class EditorWindow::StatusBridge final : public qp::authoring::IChangeListener {
public:
    explicit StatusBridge(EditorWindow& window) noexcept : window_(&window) {}
    void on_change(const qp::authoring::Change&) noexcept override {
        window_->refresh_state();
    }

private:
    EditorWindow* window_;
};

EditorWindow::EditorWindow(qp::host::PluginHost& content, std::vector<qp::views::model::GraphBlueprint> demos,
                           QWidget* parent)
    : QMainWindow(parent), content_(&content), demos_(std::move(demos)) {
    setWindowTitle(QStringLiteral("qp -- experiment editor"));
    // The built-in demonstrators, registered through the host rather than into a catalog of our own. They are
    // this build's content, so they are attributed and removable like anything else: a type that existed
    // without the record knowing about it is exactly the hole the record was added to close.
    //
    // The types that matter are content plugins; these are the minimum that exercises every editor path. See
    // demo_library.hpp for what is deliberately absent.
    for (qp::graph::NodeDesc& desc : demo_library()) {
        // A refusal is not fatal: an incomplete library still gives a usable window, and the status line names
        // the missing types once it is built.
        (void)content_->add_builtin_node_type(std::move(desc));
    }

    canvas_ = new NodeGraphView(session_, content_->node_types(), document_controller_.document(), this);
    properties_ = new PropertyPanel(session_, content_->node_types(), port_ui_, this);
    // Without a minimum the splitter collapses this panel to nothing when the
    // other two want more room -- and the first screenshot of this window showed
    // the panel simply absent, which reads as a missing feature rather than as a
    // layout problem.
    properties_->setMinimumWidth(280);
    // The splitter hands out space by stretch factor, and a panel with stretch 0
    // and no size hint gets none of it. The canvas is the part that should absorb
    // the slack, so the panel is given an explicit initial width instead.
    properties_->resize(300, properties_->height());

    auto* palette = new QListWidget(this);
    palette->setMinimumWidth(150);
    // The registry returns descriptors by value-of-container, in registration order, and the palette uses that
    // order rather than sorting: a palette that reordered between runs would move a user's node out from under
    // their muscle memory.
    for (const qp::graph::NodeDesc& desc : content_->node_types().all()) {
        palette->addItem(QString::fromStdString(desc.label.empty() ? desc.type_name : desc.label));
    }
    connect(palette, &QListWidget::itemDoubleClicked, this, [this, palette](QListWidgetItem* it) {
        const std::size_t index = static_cast<std::size_t>(palette->row(it));
        const auto& all = content_->node_types().all();
        if (index < all.size()) {
            // Through add_node, which goes through the session. The palette is not
            // allowed a shortcut the canvas does not get.
            (void)add_node(all[index].type_name);
        }
    });

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(palette);
    splitter->addWidget(canvas_);
    splitter->addWidget(properties_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    // Explicit widths, applied once the window has been laid out. `setSizes` called during
    // construction is immediately overwritten when the splitter is first resized to fit the
    // window, which is how the property panel ended up with no width at all -- and a panel that
    // is not visible reads as a missing feature rather than as a layout problem.
    //
    // The numbers are chosen to **fit**, and that is a correction rather than a preference. The
    // previous request was `{200, 680, 340}`: 1320 logical pixels. The central widget is about
    // 860 of the window's 1280 once the right-side measurement dock takes its share, and Qt
    // honours a splitter's ratios rather than its absolute sizes -- so every panel came out at
    // roughly two thirds of what it asked for. The canvas, which is the one panel that needs
    // width, lost the most: it was left a narrow strip, and `fitInView` then shrank three nodes
    // until their labels could not be read. Asking for less than is available is the fix;
    // asking for more and being scaled is the bug.
    QTimer::singleShot(0, this, [splitter] { splitter->setSizes({170, 430, 260}); });

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter);
    setCentralWidget(central);

    measurements_panel_ = new MeasurementPanel(measurements_, this);
    // The device list comes from the host's registry, which is the composition root's answer to "what can
    // measure" -- the same registry `plugins/instruments` registers into and a loaded plugin's devices would
    // arrive in. The panel is handed the list rather than reaching for it, so there is one authority.
    measurements_panel_->show_devices(content_->instruments());
    // An explicit **maximum** as well as the initial width below, and the maximum is what actually decides it: a
    // `QDockWidget` sizes itself from its child's size hint, and a `QTableWidget`'s hint grows with its content --
    // so a window that was resized wider handed the extra width to the dock rather than to the canvas, and the
    // canvas (the one panel that cannot do its job without width) lost the argument. Measured on the running
    // window: at 1680 logical pixels the dock had taken 892 of them and the canvas was left with 692, which is
    // not a layout that any `resizeDocks` call can fix from the outside.
    //
    // `kDockWidth` is a maximum rather than a preference, and it is the narrowest width at which the readings
    // table shows all four of its columns.
    measurements_panel_->setMaximumWidth(kDockWidth);
    auto* dock = new QDockWidget(tr("Measurement"), this);
    dock->setWidget(measurements_panel_);
    // Not closable and not floatable. The panel is the reporting half of the loop the
    // platform exists for, and a window that can hide it can present a measurement session
    // with no uncertainty visible -- which is precisely the artefact being replaced.
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    confidence_panel_ = new ConfidencePanel(confidence_, this);
    // The same maximum, for the same reason. Both of these panels are tables of a fixed number of columns; neither
    // gets more useful when it is wider, and the space comes straight out of the canvas.
    confidence_panel_->setMaximumWidth(kDockWidth);
    auto* confidence_dock = new QDockWidget(tr("Confidence"), this);
    confidence_dock->setWidget(confidence_panel_);
    confidence_dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    // Stacked below the measurement dock rather than floated elsewhere. The two are read together:
    // the readings say what was measured, and this says whether the numbers behind them can be
    // trusted. Putting them in different corners is how a user reads one and not the other.
    addDockWidget(Qt::RightDockWidgetArea, confidence_dock);
    // **One panel per view item**, built from the list the application mounted rather than from a hard-coded
    // widget type. That is the change the second item forced: with one item, "find the particle panel" and "give
    // this item a panel" were the same sentence, and with two they are not -- two items drawing into one canvas
    // would overwrite each other, and the order they ran in would decide which picture the user saw.
    //
    // The list is walked **once, here**, and the panels are kept in a member: `run()` has to hand each item its own
    // panel, and looking a panel up by position in `view_items()` at that point would be reading a list that a
    // plugin could have changed. A map from item to widget is the honest form of the same thing, and it also keeps
    // the pair visible in one place.
    //
    // ## Side by side at the bottom, one panel per item
    //
    // The rule is the simplest one that keeps every item visible: **N items, N panels, side by side across the
    // bottom of the window**, each getting an equal share of the width. Three layouts were tried against the
    // running window and the first two are worth recording because each looked right in the code:
    //
    //   1. stacked in the right-hand column. Six docks do not fit a 720-pixel window, and Qt's answer is not a
    //      scroll bar -- it gives the later docks **zero** height and they vanish while the window looks fine and
    //      every check passes. The diagnostic printed five docks with `visible=0 size=100x30` beside it.
    //   2. tabbed with each other in that column (`tabifyDockWidget`). They fitted, and left the picture about
    //      twenty pixels tall: visible and useless. `tabifyDockWidget` also turned out not to take effect for
    //      docks that had just been added to the same area -- the tab bar showed one label and the second dock
    //      kept its unlaid-out 100x30 -- which is a Qt behaviour this window should not be relying on either way.
    //
    // Side by side needs no tab machinery and cannot half-apply: every item gets its own widget in its own dock,
    // and the number of panels is the number of items. The reopening condition is a **third** item, at which
    // point three panels across one window is too many and the tabs (or a chooser) become worth the machinery.
    QDockWidget* first_scene_dock = nullptr;
    for (qp::graph::IViewItem* item : qp::graph::view_items()) {
        if (item == nullptr) continue;
        const QString title = QString::fromUtf8(item->name().data(), static_cast<int>(item->name().size()));
        auto* dock = new QDockWidget(title, this);
        auto* view = new SceneView(tr("%1: nothing to draw yet -- press Run.").arg(title), dock);
        dock->setWidget(view);
        dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        addDockWidget(Qt::BottomDockWidgetArea, dock);
        if (first_scene_dock == nullptr) first_scene_dock = dock;
        scene_panels_.emplace_back(item, view);
    }
    // Give the pictures a measured share of the height, once the layout exists. A request to `resizeDocks` rather
    // than a minimum: a user who drags the splitter keeps what they dragged, and this only decides what the window
    // opens with. 260 of 720 leaves the canvas about four hundred, where a five-node graph is still readable.
    if (first_scene_dock != nullptr) {
        QTimer::singleShot(0, this, [this, first_scene_dock] {
            resizeDocks({first_scene_dock}, {kSceneHeight}, Qt::Vertical);
        });
    }
    splitDockWidget(dock, confidence_dock, Qt::Vertical);

    fit_panel_ = new FitPanel(fit_, this);
    // The same maximum, for the same reason: the fit is a three-column table and a summary line, and neither
    // gets more useful when it is wider. The width comes straight out of the canvas.
    fit_panel_->setMaximumWidth(kDockWidth);
    auto* fit_dock = new QDockWidget(tr("Fit"), this);
    fit_dock->setWidget(fit_panel_);
    fit_dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    // Tabbed behind the confidence dock rather than stacked a third time: three panels in one column leaves the
    // canvas too narrow to frame a graph at the size its fonts need (see the note above), and the fit is read
    // after a run rather than during one. Tabs keep it one click away and give the canvas its width back.
    addDockWidget(Qt::RightDockWidgetArea, fit_dock);
    tabifyDockWidget(confidence_dock, fit_dock);
    confidence_dock->raise();

    // An explicit width, applied after the first layout pass. A table of readings has no opinion
    // about how wide it should be, and letting `sizeHint` decide is how the dock took 420 of the
    // window's 1280 while the canvas had 185 and could not show the graph it was drawing.
    //
    // **Both** docks are sized, not just the measurement one. The first version named one dock, and the
    // confidence dock below it then took whatever its own `sizeHint` asked for -- which is the same defect the
    // comment above records, one dock further down. Sizing them as a pair is also what makes the two tabs line
    // up: they are read together, and two panels of different widths in one column read as two unrelated tools.
    //
    // And the canvas is framed **after** the docks have taken their share. `NodeGraphView::rebuild` defers its
    // first framing to the next event-loop turn, which used to land before the docks were resized -- so the fit
    // was computed against a canvas that was about to get narrower, and the running window showed a graph
    // scrolled off the right-hand edge with "Fit graph" as the only way to find it.
    QTimer::singleShot(0, this, [this, dock, confidence_dock] {
        resizeDocks({dock, confidence_dock}, {kDockWidth, kDockWidth}, Qt::Horizontal);
    });

    // The Run action. Built after the panels, because `run_once` refreshes them.
    // The binders come from wherever the application mounted them. An empty list is a legitimate
    // state -- a build with no plugins has none -- and the Run action's message for that case is
    // already true and actionable.
    run_controller_ = std::make_unique<qp::views::model::RunController>(
        session_, ledger_,
        qp::views::model::execution_binders(),
        qp::graph::ResolveContext{&content_->node_types(), &qp::ports::builtin_registry()});
    auto* tools = addToolBar(tr("Experiment"));
    tools->setMovable(false);
    // An explicit icon size, because the default is not one. An `IconBitmap` is an 8x8 grid and `to_icon`
    // rasterises it at a **whole-number** scale, so a toolbar at Qt's usual 24-pixel default gets the 8-pixel
    // glyph replicated three times -- and `QToolBar` then scales that bitmap back down to fit the button, which
    // is what turned the "measure" glyph into an unreadable smudge in a screenshot of the running window.
    // `ToolButtonTextBesideIcon` is the other half: a tooltip is invisible until the pointer rests on the button,
    // and two icon-only buttons in a row are two buttons nobody can name.
    tools->setIconSize(QSize(16, 16));
    tools->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    run_action_ = tools->addAction(tr("Run"));
    run_action_->setToolTip(QString::fromStdString(run_controller_->description()));
    run_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    // The icons are pixel art authored as bitmaps in `icons.cpp` and coloured from the palette, so an action's
    // glyph follows a palette change like every other colour in the window. See that file for why they are not
    // image files.
    run_action_->setIcon(qt::to_icon(qt::icons::Glyph::run));
    connect(run_action_, &QAction::triggered, this, &EditorWindow::run_once);

    // The measurement action, next to Run because it is the other half of the same act: a run produces a
    // series and this writes one number of it down, attributed to the node the user has selected. Toolbar
    // rather than menu for the same reason Run is -- both are about the experiment, and the File and View
    // menus are about the document and the picture.
    measure_action_ = tools->addAction(tr("Measure"));
    measure_action_->setToolTip(
        tr("Record the selected node's latest value as a reading, attributed to that node"));
    measure_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    measure_action_->setIcon(qt::to_icon(qt::icons::Glyph::measure));
    connect(measure_action_, &QAction::triggered, this, &EditorWindow::measure_selection);

    // **The demos this build offers.** One action per blueprint the application supplied, and the window does not
    // know what any of them contain: a demo whose types this catalog lacks is reported in the menu's tooltip and
    // refused when it is asked for, rather than offered and then failing halfway.
    if (!demos_.empty()) {
        QMenu* demos = menuBar()->addMenu(tr("&Demos"));
        for (const qp::views::model::GraphBlueprint& blueprint : demos_) {
            const QString label = QString::fromStdString(blueprint.label);
            QAction* action = demos->addAction(label);
            const qp::views::model::BlueprintCheck offered =
                qp::views::model::check_blueprint(catalog(), qp::ports::builtin_registry(), blueprint);
            action->setEnabled(offered.ok);
            if (!offered.ok) {
                action->setToolTip(tr("this build cannot offer it: %1")
                                       .arg(QString::fromStdString(offered.refusal)));
            }
            connect(action, &QAction::triggered, this, [this, blueprint] { seed_blueprint(blueprint); });
        }
    }

    build_file_menu();
    build_view_menu();

    // **Two labels, because there are two kinds of text.** The left one carries news -- the sentence a run,
    // a save or a refusal produced -- and is replaced by the next piece of news. The right one carries the
    // standing state of the session as counts, and is replaced by nothing but a change to that state.
    //
    // They shared one label until a run made the difference measurable: `run_once` writes the run's sentence
    // and then refreshed the counts over it, inside the same synchronous call, so the answer to the button
    // press was on screen for no time at all. `addPermanentWidget` is Qt's own name for the right-hand side of
    // a status bar, which is the side readers learn to consult for state.
    status_ = new QLabel(this);
    statusBar()->addWidget(status_);
    state_ = new QLabel(this);
    statusBar()->addPermanentWidget(state_);

    // The canvas tells the panel what to show. Note the direction: the panel does
    // not ask the canvas, and neither asks the graph. One signal, and both keep
    // reading the same session.
    connect(canvas_, &NodeGraphView::node_selected, this, &EditorWindow::on_node_selected);
    connect(canvas_, &NodeGraphView::mutation_failed, this, &EditorWindow::on_mutation_failed);
    connect(properties_, &PropertyPanel::edit_failed, this, &EditorWindow::on_mutation_failed);

    // **The loop closes here.** Every other connection in this window carries a graph change out to the
    // panels; this one carries a reading back to the graph, so a student who highlights a row sees the
    // device that produced it. Both halves are needed and neither is optional: `reveal` scrolls a node
    // that may be off screen into view, and `select_node` is what raises the neighbourhood highlight --
    // a reveal without a selection would move the viewport and point at nothing.
    //
    // It goes through the same selection path a click does, so the property panel follows: the parameters
    // of the node behind a number are exactly what a reader wants next.
    connect(measurements_panel_, &MeasurementPanel::reading_selected, this,
            [this](qp::graph::NodeId node) {
                canvas_->reveal(node);
                canvas_->select_node(node);
            });

    // The status line follows the session, not just the edits this window starts.
    // A line that only updated on the paths this class knows about would go stale
    // the moment a panel changed something -- and a stale count is how a user
    // stops trusting the whole window.
    status_bridge_ = std::make_unique<StatusBridge>(*this);
    (void)session_.add_listener(*status_bridge_);

    refresh_state();
    // **Fit the window to the screen it opens on**, and the measurement that forced it: at this machine's 144%
    // scaling a 1280x720 window occupies 1843x1037 physical pixels while the display has 1707x1067, so the window
    // opened 95 logical pixels wider than the desktop. What lives in those 95 pixels is the right-hand column --
    // the readings, the confidence report and the picture panels -- which is to say the window opened with its
    // right-hand edge off the screen and no way to scroll to it. A default size is a guess about somebody else's
    // monitor, so it is bounded by what this one actually has.
    const QSize wanted{1280, 720};
    const QScreen* here = screen();
    const QRect available = here != nullptr ? here->availableGeometry() : QRect{};
    resize(available.isEmpty() ? wanted : wanted.boundedTo(available.size() - QSize{40, 80}));
}

void EditorWindow::build_file_menu() {
    // A File menu rather than more toolbar buttons: the window's toolbar is about the *experiment* (run
    // it, watch it), and saving is about the document. Mixing the two is how a toolbar becomes a list.
    QMenu* file = menuBar()->addMenu(tr("&File"));

    new_action_ = file->addAction(tr("&New"));
    new_action_->setShortcut(QKeySequence::New);
    new_action_->setIcon(qt::to_icon(qt::icons::Glyph::new_document));
    connect(new_action_, &QAction::triggered, this, &EditorWindow::file_new);

    open_action_ = file->addAction(tr("&Open..."));
    open_action_->setShortcut(QKeySequence::Open);
    open_action_->setIcon(qt::to_icon(qt::icons::Glyph::open));
    connect(open_action_, &QAction::triggered, this, &EditorWindow::file_open);

    save_action_ = file->addAction(tr("&Save"));
    save_action_->setShortcut(QKeySequence::Save);
    save_action_->setIcon(qt::to_icon(qt::icons::Glyph::save));
    connect(save_action_, &QAction::triggered, this, &EditorWindow::file_save);

    save_as_action_ = file->addAction(tr("Save &As..."));
    save_as_action_->setShortcut(QKeySequence::SaveAs);
    connect(save_as_action_, &QAction::triggered, this, &EditorWindow::file_save_as);

    file->addSeparator();
    export_action_ = file->addAction(tr("&Export trace..."));
    export_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    export_action_->setToolTip(tr("Write the measurement session's trace as a table"));
    export_action_->setIcon(qt::to_icon(qt::icons::Glyph::export_trace));
    connect(export_action_, &QAction::triggered, this, &EditorWindow::file_export);

    readings_action_ = file->addAction(tr("Export &readings..."));
    readings_action_->setToolTip(tr("Write the session's readings, with their uncertainties and their sources"));
    connect(readings_action_, &QAction::triggered, this, &EditorWindow::file_export_readings);

    export_fit_action_ = file->addAction(tr("Export &fit..."));
    export_fit_action_->setToolTip(tr("Write the fitted parameters, with their standard uncertainties"));
    connect(export_fit_action_, &QAction::triggered, this, &EditorWindow::file_export_fit);

    // Greyed out rather than hidden when nothing is mounted: a menu entry that disappears teaches the
    // user nothing, and one that is present but unavailable says "this build has no format for that".
    const bool has_format = document_controller_.default_format() != nullptr;
    save_action_->setEnabled(has_format);
    save_as_action_->setEnabled(has_format);
    export_action_->setEnabled(!qp::views::model::export_formats().all().empty());
}

void EditorWindow::build_view_menu() {
    // A View menu rather than more toolbar buttons, and the split is the same one the File menu makes: the toolbar
    // is about the **experiment** (run it, watch it), while framing and zooming are about the picture. A toolbar
    // that grew a zoom control would be a list again.
    //
    // Every entry here is a call the canvas already exposes. A menu item that reimplemented zooming would be a
    // second rule for how far the view may shrink, and the legibility floor is worth having exactly one of.
    QMenu* view = menuBar()->addMenu(tr("&View"));

    fit_action_ = view->addAction(tr("&Fit graph"));
    fit_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    fit_action_->setToolTip(tr("Frame the whole graph, at no less than the legibility floor"));
    connect(fit_action_, &QAction::triggered, this, [this] { canvas_->frame_graph(); });

    zoom_in_action_ = view->addAction(tr("Zoom &in"));
    zoom_in_action_->setShortcut(QKeySequence::ZoomIn);
    connect(zoom_in_action_, &QAction::triggered, this, [this] { canvas_->zoom_by(1.25); });

    zoom_out_action_ = view->addAction(tr("Zoom &out"));
    zoom_out_action_->setShortcut(QKeySequence::ZoomOut);
    connect(zoom_out_action_, &QAction::triggered, this, [this] { canvas_->zoom_by(1.0 / 1.25); });

    view->addSeparator();
    // The two removals the canvas had no gesture for. Both go through the session like every other edit, so both
    // are undoable -- and these menu entries exist as well as the Delete key because a shortcut nobody can find is
    // a feature that does not exist for most users.
    delete_action_ = view->addAction(tr("&Delete node"));
    delete_action_->setShortcut(QKeySequence::Delete);
    connect(delete_action_, &QAction::triggered, this, [this] {
        if (!canvas_->delete_selection()) {
            status_->setText(tr("Select a node first"));
        }
    });

    unlink_action_ = view->addAction(tr("Remove &connection"));
    unlink_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+D")));
    unlink_action_->setToolTip(tr("Removes the edge feeding the selected node's first connected input"));
    connect(unlink_action_, &QAction::triggered, this, [this] {
        if (!canvas_->disconnect_selection()) {
            status_->setText(tr("Nothing to unlink on the selected node"));
        }
    });
}

void EditorWindow::report_document(const qp::views::model::DocumentReport& report) {
    status_->setText(QString::fromStdString(report.message));
    refresh_caption();
}

void EditorWindow::refresh_caption() {
    const qp::authoring::Document& document = document_controller_.document();

    // The title is what the user called it; the path is where it lives; the marker is Qt's `[*]`, which
    // `setWindowModified` fills in. Both halves matter: a document with a title but no path has never been
    // saved, and one with a path but no title is a file the user opened.
    std::string shown = document.title();
    if (shown.empty()) {
        shown = document.is_untitled() ? "untitled" : document.source_path();
    }
    QString caption = QString::fromStdString(shown) + QStringLiteral("[*] -- qp");
    setWindowTitle(caption);
    setWindowModified(document_controller_.is_dirty());
}

void EditorWindow::file_new() {
    new_document();
}

void EditorWindow::new_document() {
    report_document(document_controller_.new_document());
    next_node_index_ = 1;
    // Same reasoning as a load: the readings and the trace were taken while another experiment was on screen.
    measurements_.clear_session(qp::runtime::RunId{});
    refresh_panels();
    refresh_state();
}

bool EditorWindow::save_document(const std::string& path) {
    qp::authoring::IDocumentFormat* format = document_controller_.default_format();
    if (format == nullptr) {
        status_->setText(tr("no document format is available in this build"));
        return false;
    }
    const qp::views::model::DocumentReport report = document_controller_.save(*format, path);
    report_document(report);
    return report.ok;
}

bool EditorWindow::open_document(const std::string& path) {
    // The format is chosen by the file's own extension rather than by a dialog's filter: a user who typed a
    // name the dialog did not suggest still gets the format that name belongs to, and a file whose extension
    // belongs to no mounted format is refused by name instead of being handed to the wrong reader.
    const std::size_t dot = path.find_last_of('.');
    const std::string extension = dot == std::string::npos ? std::string{} : path.substr(dot + 1);

    qp::authoring::IDocumentFormat* selected = nullptr;
    for (qp::authoring::IDocumentFormat* candidate : document_controller_.formats()) {
        if (candidate == nullptr) continue;
        const std::vector<std::string>& extensions = candidate->format().extensions;
        if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end()) {
            selected = candidate;
            break;
        }
    }
    if (selected == nullptr) {
        status_->setText(
            tr("no mounted document format reads \"%1\"").arg(QString::fromStdString(extension)));
        return false;
    }

    const qp::views::model::DocumentReport report = document_controller_.open(*selected, path);
    report_document(report);

    // The readings belong to the experiment that produced them, not to the file that was just opened, so a
    // freshly opened document starts from an empty measurement session rather than showing another
    // experiment's numbers beside its graph. The run identity goes with it: those samples were not this
    // document's, and a trace labelled with somebody else's run is worse than an empty one.
    //
    // Readings **and** trace, through one call: emptying one and not the other is how a panel ends up showing
    // two experiments at once -- which is what the interactive pass caught, the confidence panel still
    // reporting the previous trace's energy drift after a load.
    if (report.ok) {
        measurements_.clear_session(qp::runtime::RunId{});
        next_node_index_ = static_cast<int>(report.nodes) + 1;
    }
    refresh_panels();
    refresh_state();
    return report.ok;
}

bool EditorWindow::export_document(const std::string& path) {
    const std::vector<qp::runtime::IExporter*> formats = qp::views::model::export_formats().all();
    if (formats.empty() || formats.front() == nullptr) {
        status_->setText(tr("no export format is available in this build"));
        return false;
    }
    qp::runtime::IExporter* format = formats.front();

    // The pre-flight, before the path was any use. The menu handler calls this only after the dialog, so a
    // caller that already knows where to write gets the same answer the dialog path would have given.
    const qp::runtime::ExportRefusal ready = measurements_.export_readiness(*format);
    if (ready != qp::runtime::ExportRefusal::ok) {
        status_->setText(QString::fromStdString(qp::views::model::describe_export_refusal(
            ready, qp::runtime::ExportSubject::trace)));
        return false;
    }

    const qp::views::model::ExportReport report =
        qp::views::model::export_trace(measurements_, *format, path);
    status_->setText(QString::fromStdString(report.message));
    return report.ok;
}

void EditorWindow::file_save() {
    // A document that has never been written needs a path, and asking for one is Save As. Doing it here
    // rather than refusing keeps Ctrl+S doing what a user expects the first time they press it.
    if (document_controller_.is_untitled() || document_controller_.default_format() == nullptr) {
        file_save_as();
        return;
    }
    (void)save_document(document_controller_.document().source_path());
}

void EditorWindow::file_save_as() {
    qp::authoring::IDocumentFormat* format = document_controller_.default_format();
    if (format == nullptr) {
        status_->setText(tr("no document format is available in this build"));
        return;
    }

    // The filter lists every mounted format, so a build with two of them offers both and a build with one
    // offers one. The chosen filter decides which format writes; with a single format the mapping is exact,
    // and with several the first extension of the matching entry is what the dialog appends.
    QStringList filters;
    for (qp::authoring::IDocumentFormat* candidate : document_controller_.formats()) {
        if (candidate == nullptr) continue;
        const qp::authoring::DocumentFormatDesc& desc = candidate->format();
        QStringList patterns;
        for (const std::string& extension : desc.extensions) {
            patterns << QStringLiteral("*.") + QString::fromStdString(extension);
        }
        filters << QString::fromStdString(desc.label) + QStringLiteral(" (") +
                       patterns.join(QStringLiteral(" ")) + QStringLiteral(")");
    }
    QString chosen = filters.isEmpty() ? QString{} : filters.first();

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save document"),
        QString::fromStdString(document_controller_.document().source_path()), filters.join(";;"),
        &chosen);

    if (path.isEmpty()) {
        status_->setText(tr("save cancelled"));
        return;
    }
    (void)save_document(path.toStdString());
}

void EditorWindow::file_open() {
    if (document_controller_.formats().empty()) {
        status_->setText(tr("no document format is available in this build"));
        return;
    }

    QStringList filters;
    for (qp::authoring::IDocumentFormat* candidate : document_controller_.formats()) {
        if (candidate == nullptr) continue;
        const qp::authoring::DocumentFormatDesc& desc = candidate->format();
        QStringList patterns;
        for (const std::string& extension : desc.extensions) {
            patterns << QStringLiteral("*.") + QString::fromStdString(extension);
        }
        filters << QString::fromStdString(desc.label) + QStringLiteral(" (") +
                       patterns.join(QStringLiteral(" ")) + QStringLiteral(")");
    }

    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open document"), QString{}, filters.join(";;"));
    if (path.isEmpty()) {
        status_->setText(tr("open cancelled"));
        return;
    }
    (void)open_document(path.toStdString());
}

void EditorWindow::file_export() {
    // The panel's own trace, the registered formats, and -- first -- the answer to "can this format keep
    // what this session has". Asking before the dialog is the point: a user who picks a file name and only
    // then learns that the error bars were dropped has already lost them.
    const std::vector<qp::runtime::IExporter*> formats = qp::views::model::export_formats().all();
    if (formats.empty() || formats.front() == nullptr) {
        status_->setText(tr("no export format is available in this build"));
        return;
    }
    qp::runtime::IExporter* format = formats.front();

    const qp::runtime::ExportRefusal ready = measurements_.export_readiness(*format);
    if (ready != qp::runtime::ExportRefusal::ok) {
        status_->setText(QString::fromStdString(qp::views::model::describe_export_refusal(
            ready, qp::runtime::ExportSubject::trace)));
        return;
    }

    const qp::runtime::FormatDesc& desc = format->format();
    QStringList patterns;
    for (const std::string& extension : desc.extensions) {
        patterns << QStringLiteral("*.") + QString::fromStdString(extension);
    }
    const QString filter = QString::fromStdString(desc.label) + QStringLiteral(" (") +
                           patterns.join(QStringLiteral(" ")) + QStringLiteral(")");

    const QString path = QFileDialog::getSaveFileName(this, tr("Export trace"), QString{}, filter);
    if (path.isEmpty()) {
        status_->setText(tr("export cancelled"));
        return;
    }
    (void)export_document(path.toStdString());
}

void EditorWindow::file_export_readings() {
    // The same shape as the trace's export, for the same reasons: the format list first, the pre-flight **before**
    // the dialog -- a user who picks a file name and only then learns that the table cannot be written has wasted the
    // choice -- and the refusal reported as a sentence.
    const std::vector<qp::runtime::IExporter*> formats = qp::views::model::export_formats().all();
    if (formats.empty() || formats.front() == nullptr) {
        status_->setText(tr("no export format is available in this build"));
        return;
    }
    qp::runtime::IExporter* format = formats.front();
    const qp::runtime::ExportRefusal ready = measurements_.readings_readiness(*format);
    if (ready != qp::runtime::ExportRefusal::ok) {
        status_->setText(QString::fromStdString(qp::views::model::describe_export_refusal(
            ready, qp::runtime::ExportSubject::readings)));
        return;
    }

    const qp::runtime::FormatDesc& desc = format->format();
    QStringList patterns;
    for (const std::string& extension : desc.extensions) {
        patterns << QStringLiteral("*.") + QString::fromStdString(extension);
    }
    const QString filter = QString::fromStdString(desc.label) + QStringLiteral(" (") +
                           patterns.join(QStringLiteral(" ")) + QStringLiteral(")");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export readings"), QString{}, filter);
    if (path.isEmpty()) {
        status_->setText(tr("export cancelled"));
        return;
    }
    (void)export_readings_document(path.toStdString());
}

void EditorWindow::file_export_fit() {
    // **Three exports, one shape.** The same order as the other two -- the format list, the pre-flight, then the
    // dialog -- and with one difference that belongs to a fit: there has to *be* a fit. The panel owns the result
    // (it is what ran it), so this reads the panel's result rather than fitting again, and "not fitted" is refused
    // with a sentence instead of writing an empty file for it.
    const std::vector<qp::runtime::IExporter*> formats = qp::views::model::export_formats().all();
    if (formats.empty() || formats.front() == nullptr) {
        status_->setText(tr("no export format is available in this build"));
        return;
    }
    qp::runtime::IExporter* format = formats.front();

    const std::optional<qp::runtime::FitResult>& fit = fit_panel_->result();
    if (!fit.has_value()) {
        // A sentence about the *session*, not about the format: nothing has been fitted, so there is nothing to
        // choose a format for.
        status_->setText(tr("nothing is fitted yet: choose a channel and a degree in the fit panel first"));
        return;
    }

    const qp::runtime::ExportRefusal ready = qp::views::model::fit_readiness(*format, *fit);
    if (ready != qp::runtime::ExportRefusal::ok) {
        status_->setText(QString::fromStdString(qp::views::model::describe_export_refusal(
            ready, qp::runtime::ExportSubject::fit)));
        return;
    }

    const qp::runtime::FormatDesc& desc = format->format();
    QStringList patterns;
    for (const std::string& extension : desc.extensions) {
        patterns << QStringLiteral("*.") + QString::fromStdString(extension);
    }
    const QString filter = QString::fromStdString(desc.label) + QStringLiteral(" (") +
                           patterns.join(QStringLiteral(" ")) + QStringLiteral(")");
    const QString path = QFileDialog::getSaveFileName(this, tr("Export fit"), QString{}, filter);
    if (path.isEmpty()) {
        status_->setText(tr("export cancelled"));
        return;
    }
    (void)export_fit_document(path.toStdString());
}

std::vector<std::string> EditorWindow::fit_coefficient_labels() const {
    // The names the panel's own rows carry, from the model layer's one rule: a table whose `parameter` column said
    // `k0` where the window says `a` is a table a reader cannot line up with what they saw.
    std::vector<std::string> labels;
    if (!fit_panel_->result().has_value()) return labels;
    const std::size_t count = fit_panel_->result()->coefficients.size();
    labels.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        labels.push_back(qp::views::model::fit_coefficient_name(index));
    }
    return labels;
}

bool EditorWindow::export_fit_document(const std::string& path) {
    const std::vector<qp::runtime::IExporter*> formats = qp::views::model::export_formats().all();
    const std::optional<qp::runtime::FitResult>& fit = fit_panel_->result();
    if (formats.empty() || formats.front() == nullptr || !fit.has_value()) {
        status_->setText(tr("nothing is fitted yet"));
        return false;
    }
    const qp::views::model::ExportReport report =
        qp::views::model::export_fit(*fit, *formats.front(), fit_coefficient_labels(), path);
    status_->setText(QString::fromStdString(report.message));
    return report.ok;
}

std::vector<std::string> EditorWindow::reading_labels() const {
    const qp::runtime::Dataset& dataset = measurements_.dataset();
    std::vector<std::string> labels;
    labels.reserve(dataset.readings().size());
    for (std::size_t index = 0; index < dataset.readings().size(); ++index) {
        const std::optional<qp::runtime::Measurement::Source> source = measurements_.source_of(index);
        // **Empty rather than guessed.** A reading with no source is a number somebody typed in, which is a
        // legitimate kind of reading in a lab session; naming it after whatever node happens to be selected would be
        // a provenance that cannot be followed back. A source that no longer resolves -- the node was deleted after
        // the reading was taken -- is the same case, and the reading is still a reading.
        if (!source.has_value()) {
            labels.emplace_back();
            continue;
        }
        const qp::graph::Node* node = session_.graph().find_node(qp::graph::NodeId{source->index, source->generation});
        labels.push_back(node != nullptr ? node->name : std::string{});
    }
    return labels;
}

bool EditorWindow::export_readings_document(const std::string& path) {
    const std::vector<qp::runtime::IExporter*> formats = qp::views::model::export_formats().all();
    if (formats.empty() || formats.front() == nullptr) {
        status_->setText(tr("no export format is available in this build"));
        return false;
    }
    const qp::views::model::ExportReport report =
        qp::views::model::export_readings(measurements_, *formats.front(), reading_labels(), path);
    status_->setText(QString::fromStdString(report.message));
    return report.ok;
}

void EditorWindow::seed_blueprint(const qp::views::model::GraphBlueprint& blueprint) {
    const qp::views::model::BlueprintReport report = qp::views::model::apply_blueprint(session_, catalog(), blueprint);
    if (!report.ok) {
        on_mutation_failed(QString::fromStdString(report.refusal));
        return;
    }
    refresh_state();
    status_->setText(QStringLiteral("%1: %2 nodes, %3 wires")
                         .arg(QString::fromStdString(blueprint.label))
                         .arg(report.nodes.size())
                         .arg(report.wires_connected));
}

void EditorWindow::seed_demo() {
    // Order is the whole content of this function. See the header.
    seed_demo_graph();
    seed_demo_measurement();
    refresh_state();
}

EditorWindow::~EditorWindow() {
    // **First statement in the destructor, and it is load-bearing.**
    //
    // Qt destroys the window's children after this body runs, in an order this class does not control. Tearing
    // down the canvas clears its `QGraphicsScene`, and clearing a scene raises `selectionChanged` -- which the
    // canvas forwards as `node_selected`, which this window routes to the property panel. So the canvas's
    // destructor rebuilt a sibling panel's widgets, and if the panel had already been destroyed the process died
    // with a segmentation fault **after** every assertion in the suite had passed. A green test run with a
    // non-zero exit code is the worst shape a defect can take: the report is wrong in the direction of "fine".
    //
    // The canvas is also the only child that reports anything, so silencing it here is enough; the measurement and
    // confidence panels are pure readers with no outgoing signals.
    if (canvas_ != nullptr) canvas_->go_quiet();
}

qp::graph::NodeId EditorWindow::add_node(const std::string& type_name) {
    if (content_->node_types().find(type_name) == nullptr) {
        on_mutation_failed(QStringLiteral("no such type: %1")
                               .arg(QString::fromStdString(type_name)));
        return {};
    }

    const auto reserved = session_.reserve_node();
    if (!reserved.has_value()) {
        on_mutation_failed(describe(reserved.error()));
        return {};
    }

    qp::graph::AddNode command;
    command.id = reserved.value();
    command.type_name = type_name;
    command.name = "n" + std::to_string(next_node_index_++);

    const auto applied = session_.apply(command);
    if (!applied.has_value()) {
        on_mutation_failed(describe(applied.error()));
        return {};
    }

    refresh_state();
    return reserved.value();
}

void EditorWindow::seed_demo_graph() {
    // A small graph that connects, so the canvas has an edge to draw and the
    // panel has a node with parameters. Chosen to exercise the paths: a source
    // with an enum and bounded numbers, a model with an unbounded parameter, and
    // an instrument whose resolution is what turns a reading into an uncertainty.
    const qp::graph::NodeId source = add_node("demo.signal");
    const qp::graph::NodeId model = add_node("demo.spring_damper");
    const qp::graph::NodeId scope = add_node("demo.instrument");
    if (!source.valid() || !model.valid() || !scope.valid()) return;

    // The model's parameters, set through the session like every other edit -- so they show up in the
    // property panel, are undoable, and bump the graph version the run record pins.
    //
    // Without this the Run action reports "no node in this graph has an operator yet", which is true:
    // the binder needs `k`, `m`, `c` and `integrator`, and a node nobody has filled in is a node it
    // cannot run. That was the first thing the Run button did on launch, and it is the correct answer to
    // an incomplete demo rather than a defect in the run loop.
    //
    // `omega = sqrt(k/m) = 20 rad/s`, matching `RunController::kOmega` so the trace's frequency
    // component and the kernel's parameter block agree. Damping is zero **on purpose**: the kernel
    // integrates `x'' = -omega^2 x` and has no damping term, and the binder refuses a non-zero `c` rather
    // than silently dropping it. Seeding a non-zero value would open the demo on a refusal, which
    // teaches the wrong thing about what the tool can do.
    const auto set_parameter = [this](qp::graph::NodeId node, qp::graph::PortNumber port,
                                      qp::ports::Value value) {
        qp::graph::SetParam command;
        command.id = node;
        command.port = port;
        command.value = std::move(value);
        const auto applied = session_.apply(command);
        if (!applied.has_value()) on_mutation_failed(describe(applied.error()));
    };
    set_parameter(model, 2, qp::ports::Value{200.0});                     // stiffness, N/m
    set_parameter(model, 3, qp::ports::Value{0.0});                       // damping, N*s/m
    set_parameter(model, 4, qp::ports::Value{0.5});                       // mass, kg
    set_parameter(model, 5, qp::ports::Value{std::int64_t{1}});           // integrator: rk4

    // Connections go through the session too, so undo reverses them and both
    // panels are told. A failure is reported rather than swallowed: a graph that
    // silently lost an edge would look like a layout problem.
    const auto connect_ports = [this](qp::graph::NodeId from, qp::graph::NodeId to) {
        qp::graph::Connect command;
        command.from = qp::graph::PortRef{from, 1, qp::graph::PortDirection::output};
        command.to = qp::graph::PortRef{to, 1, qp::graph::PortDirection::input};
        const auto applied = session_.apply(command);
        if (!applied.has_value()) on_mutation_failed(describe(applied.error()));
    };
    connect_ports(source, model);
    connect_ports(model, scope);

    // A run, so the status line reports something real rather than a placeholder.
    //
    // ## This record is deliberately incomplete, and that is the honest choice
    //
    // The window pins what it genuinely knows: the seed, the graph version, the toolchain, the
    // optimisation level and the time. It does **not** pin the parameter set or the plugin
    // versions, because it has neither -- this build loads no plugins, and no parameter set has
    // been chosen to record.
    //
    // The alternative is to write plausible values into the empty fields so the status line
    // reads "reproducibility gaps: none". That would be a lie of exactly the kind the run ledger
    // exists to prevent. A record that **claims** reproducibility while missing its inputs is
    // worse than one that admits the gap: the first sends a student hunting for the discrepancy
    // inside their own physics, while the second tells them to re-run on the same machine.
    //
    // So the gap list is a to-do list for whoever deploys this, not a defect. A build with
    // plugins loaded drops `plugin_versions` from it; a course that pins its compiler drops
    // `toolchain`. Nothing here should be made to disappear by editing this function.
    qp::runtime::RunSpec spec;
    spec.seed = 20260911;
    spec.graph_version = session_.graph().version();
    spec.toolchain = qp::runtime::toolchain_id();
    spec.optimisation = qp::runtime::optimisation_id();
    spec.started_at = qp::runtime::now_unix_seconds();
    spec.fields = qp::runtime::ReproField::seed | qp::runtime::ReproField::graph_version |
                  qp::runtime::ReproField::toolchain | qp::runtime::ReproField::optimisation;
    (void)ledger_.begin(std::move(spec));

    refresh_state();
}

void EditorWindow::on_node_selected(qp::graph::NodeId node) {
    properties_->show_node(node);
}

void EditorWindow::on_mutation_failed(const QString& reason) {
    status_->setText(QStringLiteral("refused: %1").arg(reason));
}

void EditorWindow::refresh_state() {
    // Every number is read from the core. A status line with a hard-coded value
    // would keep looking correct after the wiring behind it had broken.
    //
    // It goes to `state_` and not to `status_`: this text is the standing description of the
    // session, and writing it over the sentence a run just produced is how the answer to the Run
    // button came to be invisible.
    const std::string gaps = [this] {
        if (ledger_.records().empty()) return std::string{"no run yet"};
        const std::vector<std::string> missing = ledger_.records().back().spec.missing_names();
        if (missing.empty()) return std::string{"complete"};
        std::string text;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i != 0) text += ", ";
            text += missing[i];
        }
        return text;
    }();

    state_->setText(QStringLiteral("nodes %1 | edges %2 | graph v%3 | changes %4 | undo %5 | "
                                    "runs %6 | reproducibility gaps: %7")
                         .arg(session_.graph().node_count())
                         .arg(session_.graph().edge_count())
                         .arg(session_.graph().version())
                         .arg(session_.sequence())
                         .arg(session_.can_undo() ? "yes" : "no")
                         .arg(ledger_.size())
                         .arg(QString::fromStdString(gaps)));

    // The caption carries the same state in the place a user looks when the window is not focused, and the
    // modified marker is Qt's `[*]`, filled in by `setWindowModified`. It is updated from here because this
    // is the one function every change passes through, so the marker cannot go stale behind an edit the
    // window did not start.
    refresh_caption();
}

void EditorWindow::build_palette() {
    // Reserved for when the palette becomes a view of its own; the constructor
    // currently fills it inline. Present so the intent is recorded rather than the
    // filling being scattered.
}

void EditorWindow::seed_demo_measurement() {
    namespace rt = qp::runtime;
    using rt::UncertaintyKind;

    // A repeat measurement of one length, in metres. The two quantified readings carry the
    // scale's resolution as their uncertainty; the third is a reading whose error nobody
    // worked out -- the normal state of a lab notebook, and the case the panel must not
    // round to zero.
    measurements_.add_reading(0.0241, UncertaintyKind::standard, 0.0005);
    measurements_.add_reading(0.0238, UncertaintyKind::standard, 0.0005);
    measurements_.add_reading(0.0243, UncertaintyKind::unknown);

    // A short trace, so the time axis is exercised rather than merely present.
    //
    // **Two channels, and the velocity is not optional.** The confidence panel's energy diagnostic
    // needs a position and a velocity: a quadratic potential is `0.5 w^2 x^2 + 0.5 v^2`, and with
    // only the first the panel reports "energy drift cannot be measured: the trace is missing
    // velocity". That is the honest answer and it was the demo's answer until this channel was
    // added -- which meant the demo showed C8's **refusal** path and never its diagnostic.
    //
    // The velocity is the analytic derivative of the position rather than a second independent
    // series, so the two channels describe one motion. The functions are
    //
    //     x(t) = A e^{-g t} cos(w t)
    //     v(t) = A e^{-g t} (-g cos(w t) - w sin(w t))
    //
    // with `A = 0.02`, `g = 0.8`, `w = 12`. The amplitude decays by construction, so the total
    // energy decays too, and the panel's note names the mechanism: this **model** has damping. That
    // is deliberately not the RK4 dissipation case -- the two are different findings, and the note
    // distinguishes them ("a property of the integrator and not of the model unless the model has
    // damping"). A demo whose energy were perfectly conserved would show the panel agreeing and
    // never show it disagreeing, which is the less useful half.
    (void)measurements_.add_channel("displacement", qp::units::dims::length);
    (void)measurements_.add_channel("velocity", qp::units::dims::velocity);
    constexpr double kAmplitude = 0.02;
    constexpr double kDecay = 0.8;
    constexpr double kOmega = 12.0;
    for (int i = 0; i < 200; ++i) {
        const double t = 0.01 * static_cast<double>(i);
        const double envelope = kAmplitude * std::exp(-kDecay * t);
        const double x = envelope * std::cos(kOmega * t);
        const double v = envelope * (-kDecay * std::cos(kOmega * t) - kOmega * std::sin(kOmega * t));
        (void)measurements_.add_sample(t, std::vector<double>{x, v}, 0.0005);
    }

    // No run is recorded here. `seed_demo_graph` already recorded one, and a second record
    // describing nothing would make the ledger's own report describe neither -- its counts
    // and its gap list are about **the last run**, so a second record means the run being
    // described is whichever happened to be second.
    //
    // The trace above therefore belongs to the run `seed_demo_graph` opened. That is the
    // honest arrangement rather than a shortcut: a trace is the record of one run, and this
    // window's is the one it already started.

    measurements_panel_->refresh();

    // Declare the frequency the energy diagnostic should assume, and refresh the panel.
    //
    // The seed's trace is a decaying oscillation rather than a solution of the RK4 operator, so
    // there is no operator whose `clamps_fired()` could be read -- and inventing a count would make
    // the panel assert something about a run that did not happen. `note_clamps` is therefore left
    // at its zero default, which is the truth here: nothing was clamped because nothing integrated.
    // The demo's own frequency, so the energy figure is computed for the potential the data came
    // from. Declaring a different value would produce a drift that is an artefact of the mismatch,
    // and the panel would be reporting its own input as a finding about the run.
    confidence_.set_omega(kOmega);
    clamps_noted_ = true;
    confidence_panel_->refresh();

    // The fit panel reads a **trace** whose channels do not exist until this function declared them, so the
    // channel list is rebuilt here rather than only in `refresh_panels`. Without this the seed left the fit tab
    // offering no channel at all -- which reads as "this run has nothing to fit" about a run with two channels
    // and two hundred samples in it.
    fit_panel_->show_channels();
}


void EditorWindow::run_once() {
    if (run_controller_ == nullptr) return;

    qp::views::model::RunResult result = run_controller_->run();

    // The status line speaks first: it is where every other message in this window goes, and a user who
    // pressed a button is looking for a response in one consistent place.
    status_->setText(QString::fromStdString(result.report.message));

    // What the graph asked to have **drawn**, handed to whichever view item claims a declaration. This comes
    // before the failure branch on purpose: the declarations are filled whether or not the run succeeded --
    // what a graph declares is a property of the graph -- and a refused run should still show the last picture
    // rather than a blank panel.
    //
    // Every declaration is offered to **every** item, and each item that claims it draws into its own panel. That
    // loop is what the second item changed: the `break` after the first claim was right while there was one item
    // and one panel, and with two it would silently give the field-line declaration to the particle item and stop.
    for (const qp::graph::DeclaredOutput& declaration : result.render_declared) {
        const qp::graph::Node* node = session_.graph().find_node(declaration.node);
        if (node == nullptr) continue;
        for (const auto& [item, panel] : scene_panels_) {
            if (item == nullptr || panel == nullptr || !item->draws(node->type_name)) continue;
            // `result.fields` is a member of the result rather than a pointer into the run, because the run is
            // gone by the time this line runs: it is the same reason `particle_positions` is a member.
            const qp::graph::ViewRequest request{&session_.graph(), &result.render_declared,
                                                 &result.particle_positions, result.report.steps,
                                                 &result.fields};
            if (!request.valid()) continue;
            panel->set_scene(item->scene(request));
        }
    }

    if (!result.report.ok) {
        // A failed run leaves the panels alone. Replacing good readings with nothing because a run was
        // refused would destroy the user's work to report a problem that did not touch it.
        //
        // An empty trace after a *successful* run is not possible; after a failed one it means the
        // binder was never reached, and there is nothing to show either way.
        if (result.trace.empty()) return;
        // A **partial** trace is shown: the confidence panel exists to make a diverging run visible, and
        // it cannot do that from an empty panel.
    }

    // The measurement session is **replaced**, not appended to. The seeded demo describes a different
    // experiment from the one that just ran, and appending would make the time axis go backwards at the
    // seam -- which the trace refuses, so the failure would appear as an append error rather than as
    // "you ran a new experiment".
    measurements_.reset_trace(result.report.run);

    // The two channels are copied across by name, so the panels find them through the same constants the
    // run loop wrote them with. A rename on either side breaks here rather than silently producing an
    // unaskable trace.
    for (const qp::runtime::Channel& channel : result.trace.channels()) {
        (void)measurements_.add_channel(channel.name, channel.dim);
    }
    for (const qp::runtime::Sample& sample : result.trace.samples()) {
        std::vector<double> values;
        values.reserve(sample.values.size());
        for (const qp::runtime::UncertainValue& v : sample.values) values.push_back(v.value);
        (void)measurements_.add_sample(sample.t, values, 0.0);
    }

    // The confidence model needs the frequency the run integrated against, or its energy figure would be
    // computed for a different potential than the data came from -- and it would then report drift that
    // is an artefact of the mismatch. `omega_was_declared` is what lets the panel say which it is.
    confidence_.set_omega(qp::views::model::RunController::kOmega);

    refresh_panels();
    refresh_state();
}

void EditorWindow::measure_selection() {
    namespace rt = qp::runtime;

    const std::optional<qp::graph::NodeId> selected = canvas_->selected_node();
    if (!selected.has_value()) {
        status_->setText(tr("Select the node to take a reading from"));
        return;
    }

    const rt::Trace& trace = measurements_.trace();
    if (trace.empty() || trace.channels().empty()) {
        // Named as what to do about it rather than as what is missing. "The trace is empty" is true and
        // leaves the user nowhere to go; the Run button is the answer, so the sentence says so.
        status_->setText(tr("Nothing to read yet -- run the experiment first"));
        return;
    }

    // The channel is chosen by **the instrument's dimension** where there is an instrument, and by the dataset's
    // otherwise. That order is the point of this whole path: a caliper measures a length, so the channel it reads
    // is the one measured in metres -- and choosing by the dataset's dimension instead would let a device whose
    // quantity is time read a displacement, which `Dataset::add` would then silently normalise into metres.
    //
    // With no instrument chosen the dataset's dimension is the only answer available, and the fallback is
    // explicit rather than silent: the status line says which route produced the reading.
    const rt::UncertainValue* value = nullptr;
    std::string measured_by;
    const std::string device_id = measurements_panel_->chosen_device();
    rt::IInstrument* device = device_id.empty() ? nullptr : content_->instruments().find(device_id);
    const qp::units::Dim wanted = device != nullptr ? device->describe().dim : measurements_.dataset().dim();

    const std::vector<rt::Channel>& channels = trace.channels();
    std::size_t channel = channels.size();
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].dim == wanted) {
            channel = i;
            break;
        }
    }
    if (channel == channels.size()) {
        status_->setText(device != nullptr
                             ? tr("The trace has no channel the %1 can measure")
                                   .arg(QString::fromStdString(device->describe().label))
                             : tr("The trace has no channel measured in this session's dimension"));
        return;
    }

    // The last sample: the final state of the run, which is the one a lab session writes down. Every other
    // sample is a step on the way there, and the trace exists so the way there is recoverable.
    const rt::Sample& last = trace.samples().back();
    if (channel >= last.values.size()) {
        status_->setText(tr("The trace's last sample does not carry that channel"));
        return;
    }

    if (device != nullptr) {
        // **Through the fault barrier**, because a device is plugin code and a device that raises must not take
        // the process with it -- the same rule the host applies when it calls one. The truth handed over is the
        // model's own value with its own uncertainty discarded, and that is deliberate rather than sloppy: what
        // this records is *a reading of that value*, so the error bar belongs to the instrument's graduations and
        // not to the simulation. A student measuring a simulated length with a real caliper gets the caliper's
        // uncertainty, which is the thing they are supposed to learn.
        const auto reading = rt::measure_guarded(*device, last.values[channel].value, rt::MeasureContext{},
                                                 device->describe().id, nullptr);
        if (!reading.has_value()) {
            status_->setText(tr("The %1 refused to measure that value")
                                 .arg(QString::fromStdString(device->describe().label)));
            return;
        }
        scratch_reading_ = reading.value();
        value = &scratch_reading_;
        measured_by = device->describe().label;
    } else {
        value = &last.values[channel];
    }

    measurements_.add_reading(value->value, value->kind, value->u,
                              rt::Measurement::Source{selected->index, selected->generation});

    // Refreshed **before** the status line, because the panel is what changed and a table that is
    // repainted a moment after the sentence describing it is the flicker that makes a window feel broken.
    refresh_panels();
    // The sentence names the device when there was one, and says "the trace" when there was not. Which route
    // produced a number is part of what the number claims -- a reading taken with a caliper and one copied out of
    // a trace carry different uncertainties, and a status line that called both "recorded" would hide that.
    if (measured_by.empty()) {
        status_->setText(tr("Recorded %1 from node %2 (%3), as the trace has it")
                             .arg(value->value)
                             .arg(selected->index)
                             .arg(QString::fromStdString(channels[channel].name)));
    } else {
        status_->setText(tr("Recorded %1 from node %2, measured with the %3 (+/- %4)")
                             .arg(value->value)
                             .arg(selected->index)
                             .arg(QString::fromStdString(measured_by))
                             .arg(value->u));
    }
}

void EditorWindow::refresh_panels() {
    // The panels re-read the session; neither is told what changed. That is the rule this window exists to
    // demonstrate, and it is also why this has to be called from every site that changes the session's data:
    // a panel that is not asked keeps showing the numbers it last computed.
    measurements_panel_->refresh();
    confidence_panel_->refresh();
    // The channel list is rebuilt first, because a run is what declares channels: a fit panel that only
    // re-rendered its table would keep offering the channels of the previous run, and after the first run it
    // would offer none at all.
    if (fit_panel_ != nullptr) {
        fit_panel_->show_channels();
        fit_panel_->refresh();
    }
}

}  // namespace qp::views
