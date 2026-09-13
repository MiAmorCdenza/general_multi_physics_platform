/**
 * @file editor_window.hpp
 * @brief The editor: a node canvas, a property panel, and one session between them.
 *
 * ## What this window is for
 *
 * It is the first shell that actually edits something. Its job is to prove the
 * view layer's central claim end to end: that **one session, one graph, one undo
 * stack** is enough for two panels that show different facets of the same thing,
 * and that neither panel needs its own copy of the graph to work.
 *
 * The claim is checkable by using it: select a node in the canvas, and the panel
 * shows its parameters; change a parameter, and the canvas redraws because the
 * session told it to rather than because the panel told it to. If either panel
 * held its own graph, the other would go stale -- and a stale panel is the defect
 * the whole `authoring` layer exists to prevent.
 *
 * ## What it is not
 *
 * Not a finished application. There is no plugin loading, no evaluation, no run
 * loop, and no serialisation: those are `plugins/` and `runtime/` concerns, and
 * pretending otherwise in the shell would mean inventing behaviour the core does
 * not have. The status line says so, so that nobody reads absence as a bug.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   Both panels reference the same Session instance
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.editor_window.shares_one_session,
 *              qt.views.editor_window.file_menu_follows_the_document,
 *              qt.views.measurement.one_ledger_per_session,
 *              qt.views.measurement.fresh_window_is_empty,
 *              qt.views.measurement.a_reading_points_at_its_node,
 *              qt.views.measurement.a_provider_run_closes_the_loop
 */
#pragma once

#include <QMainWindow>

#include <qp/authoring/capability/capability.hpp>
#include <qp/authoring/commands/session.hpp>
#include <qp/authoring/document/document.hpp>
#include <qp/authoring/portui/port_ui.hpp>
#include <qp/graph/domain/view_items.hpp>
#include <qp/graph/ir/node_type_registry.hpp>
#include <qp/host/host.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/views/model/blueprint.hpp>
#include <qp/views/model/confidence_model.hpp>
#include <qp/views/model/document_controller.hpp>
#include <qp/views/model/fit_session.hpp>
#include <qp/views/model/run_controller.hpp>
#include <qp/views/model/measurement_model.hpp>

#include <memory>
#include <utility>
#include <vector>

class QAction;
class QLabel;

namespace qp::views {

class ConfidencePanel;
class FitPanel;
class MeasurementPanel;
class NodeGraphView;
class PropertyPanel;
class SceneView;

/**
 * @brief The editing window: canvas on the left, properties on the right.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The canvas and the panel share one session and one document
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.editor_window.shares_one_session
 */
class EditorWindow final : public QMainWindow {
    Q_OBJECT

public:
    /**
     * @brief Opens the editor over `content`'s node catalog.
     *
     * @param content The composition root, **borrowed**. It owns the catalog, and the window borrows it rather
     *                than holding one of its own: a window that owned a catalog would be a second answer to
     *                "which types exist", and the palette would show whichever one it happened to hold. That
     *                duplicate was removed once already, when content plugins needed somewhere to put a node
     *                type and the only enumerable catalog lived in this layer.
     *
     *                The window registers its demonstrator types through `add_builtin_node_type`, so they are
     *                attributed and removable like anything else rather than being a hole in the record.
     * @param demos   The demos this build offers, as blueprints. **The window does not name a plugin's types**:
     *                the application pairs a kit with a demo, because the application is the composition root and
     *                the only place in the build that knows both. Each entry becomes one menu action, and an entry
     *                whose blueprint this catalog cannot offer is reported rather than offered.
     * @param parent  The Qt parent, as usual.
     *
     * @ownership   observes the host and its catalog
     * @thread      ui
     * @pre         `content` outlives this window
     * @post        The demonstrator types are in the catalog, attributed to `PluginHost::kBuiltinOrigin`
     * @invariant   The catalog is not modified after construction
     * @errors      A type the host refuses is skipped rather than fatal. The constructor itself may throw: it takes
     *              the demos **by value**, so building the window copies a vector of blueprints, and a copy
     *              allocates. The contract used to claim it could not throw, from when the parameter was absent --
     *              and the gate comparing the claim with the signature is what noticed.
     * @complexity  O(types + demos)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.editor_window.shares_one_session
     */
    explicit EditorWindow(qp::host::PluginHost& content,
                          std::vector<qp::views::model::GraphBlueprint> demos = {}, QWidget* parent = nullptr);
    ~EditorWindow() override;

    /// @brief The session both panels edit through. Exposed so a test can drive it.
    [[nodiscard]] qp::authoring::Session& session() noexcept { return session_; }

    /// @brief The composition root this window reads its content from. Exposed so a test can add a type.
    [[nodiscard]] qp::host::PluginHost& content() noexcept { return *content_; }

    /// @brief The catalog the palette and the panel resolve types against.
    [[nodiscard]] const qp::graph::NodeTypeRegistry& catalog() const noexcept {
        return content_->node_types();
    }

    /// @brief The measurement session this window reports on.
    ///
    /// Exposed so a test can drive it, for the same reason `session()` is: the window's job
    /// is to hold **one** of each thing, and a test that reached through the widgets to find
    /// them would be testing the layout rather than the ownership.
    [[nodiscard]] qp::views::model::MeasurementModel& measurements() noexcept {
        return measurements_;
    }

    /// @brief The confidence model over the measurement session's trace.
    ///
    /// Exposed so a test can assert the panel shows **this** model's values rather than its own
    /// arithmetic, which is the property most likely to rot in a thin rendering layer.
    [[nodiscard]] qp::views::model::ConfidenceModel& confidence() noexcept { return confidence_; }

    /// @brief The fit session over the same trace.
    ///
    /// Exposed so a test can drive the panel's path without reaching through its widgets, which is the same
    /// argument `measurements()` and `confidence()` give: the window's job is to hold **one** of each thing.
    [[nodiscard]] qp::views::model::FitSession& fit() noexcept { return fit_; }

    /// @brief The fit panel, so a test can assert what is displayed.
    [[nodiscard]] FitPanel* fit_panel() noexcept { return fit_panel_; }

    /// @brief The session's run ledger.
    ///
    /// Exposed so a test can assert that the measurement session **borrows this one** rather
    /// than holding a second. The window had two ledgers once, and the status line and the
    /// measurement panel then disagreed on screen about how many runs the session had.
    ///
    /// `RunController` was the third holder and the last to be converted, and its own ledger was
    /// invisible in exactly the same way: the run it recorded was drawn on the panels beside a status
    /// line reading `runs 0`. Everything in a session that writes or reads run identities now
    /// borrows this object, and the two labels of the status bar are where that is visible.
    [[nodiscard]] qp::runtime::RunLedger& ledger() noexcept { return ledger_; }

    /**
     * @brief The message half of the status bar, and the standing half. Exposed so a case can assert what a
     *        user actually reads.
     *
     * The two are separate labels because they are two kinds of text: news on the left, state on the right.
     * A case asserting only on the ledger would say the run was recorded; these say the user can see that it
     * was, and the second claim is the one that was false -- the window's ledger was correct and empty while
     * its neighbour drew the run's particles.
     *
     * @ownership   borrows from this window
     * @thread      ui
     * @pre         none
     * @post        Non-null once the constructor has run
     * @invariant   The pointer is stable for the window's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.run.the_status_bar_shows_the_run_it_recorded
     */
    [[nodiscard]] QLabel* status_label() noexcept { return status_; }
    [[nodiscard]] QLabel* state_label() noexcept { return state_; }

    /// @brief Adds a node of `type_name` through the session, and selects it.
    ///
    /// Returns the new node's id, or an invalid id when the type is unknown or the
    /// bus refused the command. Adding through the session is the rule this window
    /// exists to demonstrate: a window that wrote to the graph directly would
    /// produce an edit the canvas is never told about.
    [[nodiscard]] qp::graph::NodeId add_node(const std::string& type_name);

    /**
     * @brief Seeds a demo the application supplied, through the same edits a user would make.
     *
     * A refused blueprint leaves the graph alone and says why on the status line: the check runs first, so the
     * canvas never shows half a demo.
     *
     * @param blueprint The demo. Borrowed for the call.
     *
     * @ownership   observes
     * @thread      ui
     * @pre         none
     * @post        On success the session's graph holds the demo's nodes and wires
     * @invariant   Every edit goes through the session, so the demo is undoable
     * @errors      A refused blueprint is reported on the status line and the graph is left alone; the seeding path
     *              itself may allocate (the report holds the ids it added), so it does not claim otherwise.
     * @complexity  O(nodes + wires)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.editor_window.a_supplied_blueprint_becomes_a_graph
     */
    void seed_blueprint(const qp::views::model::GraphBlueprint& blueprint);

    /// @brief Seeds the window with a small graph and a matching measurement session.
    ///
    /// **Not called by the constructor.** A window that filled itself in would have decided
    /// something on the caller's behalf, and `tests/unit/views/test_views_qt.cpp` asserts a
    /// fresh window is empty -- which is the documented behaviour, not an incidental count. The
    /// application calls this; a test constructs a window and chooses.
    ///
    /// The order inside is load-bearing: the graph first, because seeding the graph is what
    /// records the run, and the measurement session's trace belongs to that run. The reverse
    /// leaves the panel reporting "no run recorded" while the status line reports the run's
    /// gaps -- two panels disagreeing about one session, which is the failure this whole window
    /// is built to avoid.
    void seed_demo();

    /// @brief Records the demonstrator graph through the session.
    void seed_demo_graph();

    /// @brief Runs the first runnable node and shows the result.
    ///
    /// Wired to the toolbar action. Kept as a named method rather than a lambda in the constructor so a
    /// test can invoke the same path the button does -- a test that called the controller directly would
    /// not cover the wiring, which is where the defects in a thin layer always are.
    void run_once();

    /**
     * @brief Records one reading taken from the node the canvas has selected.
     *
     * ## Why a reading has to be taken rather than derived
     *
     * The value is the **last sample of the trace**, and the node it is attributed to is the one the user
     * selected. Both halves matter. A reading is a single number a person decided to write down, so it
     * cannot be produced by a run on its own: a run makes a series, and choosing one point of it -- the
     * final state, after the transient -- is the act this models. And the attribution is the **selected**
     * node rather than the node the last run used, because the honest claim is "this is what that device
     * reads", and the user is the one who knows which device that is.
     *
     * This is the reverse direction of the loop the window exists to demonstrate. Every other path goes
     * from the graph to the numbers; this one gives a number the identity of the node behind it, which is
     * what lets `MeasurementPanel::reading_selected` point back at it.
     *
     * Refuses, in the status line, when there is nothing to take a reading from: no selection, an empty
     * trace, or a channel whose dimension is not the session's. The last is the one that matters, and it is
     * **not** silently coerced: `Dataset::add` normalises a reading's dimension to the dataset's, so a
     * velocity recorded into a length dataset would be stored as a length -- a wrong number reported as a
     * measurement, which is the failure mode this platform exists to prevent.
     *
     * @ownership   owns the record
     * @thread      ui
     * @pre         none
     * @post        On success the dataset grows by one reading carrying this node as its source
     * @invariant   Never writes to the graph, and never changes the dataset's dimension
     * @errors      A refusal is a sentence in the status line
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       qt.views.measurement.a_reading_points_at_its_node,
     *              qt.views.measurement.a_provider_run_closes_the_loop
     */
    void measure_selection();

    /// @brief Records a few readings so the measurement panel is not empty on start.
    ///
    /// A repeat measurement of one length with a deliberately **mixed** provenance: two
    /// readings carry a quantified uncertainty and one does not. That mix is what makes the
    /// panel's central honesty visible on first launch -- the combined uncertainty is
    /// reported, and the gap list says that one reading contributed nothing to it. A
    /// demonstration where every reading was quantified would show the arithmetic working
    /// and hide the rule.
    void seed_demo_measurement();

    /// @brief Starts a new, empty document.
    void file_new();
    /// @brief Saves to the document's own path, or asks for one when it has none.
    void file_save();
    /// @brief Asks for a path and a format, then saves.
    void file_save_as();
    /// @brief Asks for a file and opens it.
    void file_open();
    /// @brief Exports the measurement session's trace, asking the format first.
    ///
    /// The pre-flight runs **before** the dialog: a format that cannot keep this session's uncertainties
    /// is refused in the status line rather than after the user has chosen a file name.
    void file_export();

    /// @brief Exports the session's **readings** table, asking the format first.
    ///
    /// The second Export entry, and the artifact the platform's loop ends in: one row per measurement with its
    /// standard uncertainty, its kind and the node it came from. Same rule as the trace's about the pre-flight --
    /// refused in the status line before the dialog rather than after a file name was chosen -- and no keyboard
    /// shortcut, because one `Ctrl+E` is a habit and two are a coin toss.
    void file_export_readings();

    /// @brief The document controller: what a save writes and what an open installs.
    [[nodiscard]] qp::views::model::DocumentController& document_controller() noexcept {
        return document_controller_;
    }

    /// @brief Starts a new document without asking anything. The menu's New action calls this.
    void new_document();

    /// @brief Saves to `path` without asking anything. Returns whether it worked.
    ///
    /// Separate from the menu handler on purpose, and the separation is what makes the wiring testable: a
    /// modal file dialog cannot be driven from a test, but everything after it can. The handler's only job
    /// is to ask for a path and hand it here.
    bool save_document(const std::string& path);

    /// @brief Opens `path` without asking anything, choosing the format by the file's extension.
    bool open_document(const std::string& path);

    /// @brief Exports the trace to `path` without asking anything, after the pre-flight.
    bool export_document(const std::string& path);

    /// @brief Exports the **readings** table to `path` without asking anything, after the pre-flight.
    ///
    /// The other half of the Export entry: a trace is the run's series and the readings are the numbers the session
    /// recorded, each with its uncertainty and its source. The labels are resolved here -- this is the layer that has
    /// both the dataset and the graph, and `runtime/io` may not know what a node is -- and a reading whose source is
    /// not a node in this graph gets an empty label rather than a guess.
    bool export_readings_document(const std::string& path);

    /// @brief One label per reading: the name of the node it came from, or empty when it came from nowhere.
    ///
    /// A hand-entered reading has no source, which is a legitimate kind of reading in a lab session; the file says so
    /// by leaving the field empty rather than by inventing a device.
    [[nodiscard]] std::vector<std::string> reading_labels() const;

private:
    class StatusBridge;

    /// @brief Rebuilds the palette list from the catalog.
    void build_palette();
    /// @brief Refreshes the **standing** half of the status bar from the session and the ledger.
    ///
    /// Two kinds of text live in one status bar and they are refreshed by different things: a **message**
    /// (what just happened, in a sentence) and the **standing state** (what the session is, as counts). They
    /// used to be the same label, which meant every message was overwritten by the next `refresh_status()`
    /// -- including, measurably, the sentence a run produced, written and replaced inside one call. The
    /// message goes to `status_` and the standing state to `state_`, so each survives the other.
    void refresh_state();
    /// @brief Follows the canvas's selection into the property panel.
    void on_node_selected(qp::graph::NodeId node);
    /// @brief Reports a refused mutation in the status line.
    void on_mutation_failed(const QString& reason);
    /// @brief Reports a document operation's outcome, and updates the caption.
    void report_document(const qp::views::model::DocumentReport& report);
    /// @brief Sets the caption from the document's title and path, and the modified marker.
    void refresh_caption();
    /// @brief Tells the two data panels to re-read the measurement session.
    ///
    /// One function rather than a pair of calls at every site, and that is a correction rather than tidiness:
    /// the panels were refreshed inside `run_once` when that was the only thing changing the session's data,
    /// so opening a document emptied the trace and left the confidence panel showing the **previous**
    /// experiment's diagnostics. The interactive pass found it. Three call sites is exactly where one gets
    /// forgotten.
    void refresh_panels();
    /// @brief Builds the File menu's actions and shortcuts.
    void build_file_menu();
    /// @brief Builds the View menu: framing, zoom and the two removals the canvas has gestures for.
    void build_view_menu();

    qp::authoring::Session session_{};
    // The composition root this window reads its content from. Borrowed, and the only source of node types: the
    // palette, the property panel, the canvas and the framework pre-flight all resolve against its catalog, so
    // a type a plugin registered at startup is a type all four can see.
    qp::host::PluginHost* content_ = nullptr;
    // The document and the file menu's behaviour. It owns the `Document` -- the canvas borrows it from
    // here -- and registers itself as a session listener, because the dirty flag has to follow every edit
    // rather than the ones this window happens to start.
    qp::views::model::DocumentController document_controller_{
        session_, qp::views::model::document_formats()};
    qp::authoring::PortUiRegistry port_ui_{};
    qp::runtime::RunLedger ledger_{};
    // One measurement session per window, measuring a length by default. The quantity is a
    // construction parameter rather than a field the user sets later because the dataset's
    // dimension is fixed at construction -- a session that could change dimension mid-way
    // would allow a mean over metres and seconds.
    qp::views::model::MeasurementModel measurements_{ledger_, "length", qp::units::dims::length};

    NodeGraphView* canvas_ = nullptr;
    PropertyPanel* properties_ = nullptr;
    MeasurementPanel* measurements_panel_ = nullptr;
    // One confidence model per window, over the measurement session's trace. It borrows the trace
    // for the same reason the measurement model borrows the run ledger: a report about a **copy** of
    // the data is a report about something else, and this session has already paid for that lesson
    // once with two run ledgers.
    qp::views::model::ConfidenceModel confidence_{measurements_.trace()};
    ConfidencePanel* confidence_panel_ = nullptr;

    // **One scene panel per mounted view item**, paired here rather than looked up by widget type when a run
    // finishes. The pairing is the window's own fact -- it built the panels from `view_items()` at construction --
    // and keeping it is what lets two items coexist without either of them drawing over the other. A panel is
    // owned by its dock through Qt parentage; the pointer here is a view of that ownership, not a second owner.
    std::vector<std::pair<qp::graph::IViewItem*, SceneView*>> scene_panels_{};

    // One fit session per window, over the same trace, for the same borrowing reason. The **fit** is not
    // performed here or in `views/model`: it is a plugin, and this layer is built where no plugin exists. The
    // session decides what may be fitted; `views::FitPanel` calls the fitter behind a compile-time check.
    qp::views::model::FitSession fit_{measurements_.trace()};
    FitPanel* fit_panel_ = nullptr;

    // The run action. The binders are **not** owned here: `views` must not depend on `plugins`, so the
    // application mounts them through `views::model::mount_execution_binder` and the window consults
    // whatever is there. A window that named a plugin by type could not be built without that plugin,
    // which would make the plugin split a claim rather than a property.
    std::unique_ptr<qp::views::model::RunController> run_controller_{};
    /// The demos this build offers, in the order the menu shows them.
    std::vector<qp::views::model::GraphBlueprint> demos_;

    QAction* run_action_ = nullptr;
    // The measure action, kept for the same reason the others are: a test drives the path the button does.
    QAction* measure_action_ = nullptr;

    // The File menu's actions, kept so a test can invoke the same path the menu does.
    QAction* new_action_ = nullptr;
    QAction* open_action_ = nullptr;
    QAction* save_action_ = nullptr;
    QAction* save_as_action_ = nullptr;
    QAction* export_action_ = nullptr;
    /// The readings export, beside the trace's: two tables of one session, two files, one menu.
    QAction* readings_action_ = nullptr;

    // The View menu's actions, kept for the same reason: a test drives the same path the menu does.
    QAction* fit_action_ = nullptr;
    QAction* zoom_in_action_ = nullptr;
    QAction* zoom_out_action_ = nullptr;
    QAction* delete_action_ = nullptr;
    QAction* unlink_action_ = nullptr;

    /// @brief Whether the run's clamp count has been pushed into the confidence model yet.
    ///
    /// The count belongs to the operator rather than to the trace, so it arrives after a run rather
    /// than during one. Tracked here so the seed can note it exactly once without the window having
    /// to ask an operator it does not own.
    bool clamps_noted_ = false;
    /// @brief Storage for a reading an instrument produced, so the recorded value has a stable address.
    ///
    /// `measure_selection` builds the reading in one of two places -- an instrument's answer, or a sample's value
    /// already inside the trace -- and then records it. A `const UncertainValue*` into the trace is fine because
    /// the trace outlives the call; an instrument's answer is a temporary, so it lives here for the duration of
    /// the call rather than being copied into a second local that a pointer could outlive.
    qp::runtime::UncertainValue scratch_reading_{};
    QLabel* status_ = nullptr;
    /// The standing half of the status bar: the same counts on every refresh, on the right-hand side where
    /// Qt puts permanent widgets and where a reader learns to look for state rather than for news.
    QLabel* state_ = nullptr;
    std::unique_ptr<StatusBridge> status_bridge_;
    int next_node_index_ = 1;
};

}  // namespace qp::views
