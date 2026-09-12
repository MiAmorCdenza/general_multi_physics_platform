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
 *              qt.views.measurement.fresh_window_is_empty
 */
#pragma once

#include <QMainWindow>

#include <qp/authoring/capability/capability.hpp>
#include <qp/authoring/commands/session.hpp>
#include <qp/authoring/document/document.hpp>
#include <qp/authoring/portui/port_ui.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/views/model/confidence_model.hpp>
#include <qp/views/model/document_controller.hpp>
#include <qp/views/model/run_controller.hpp>
#include <qp/views/model/measurement_model.hpp>
#include <qp/views/model/type_catalog.hpp>
#include <qp/views/model/demo_library.hpp>

#include <memory>

class QAction;
class QLabel;

namespace qp::views {

class ConfidencePanel;
class MeasurementPanel;
class NodeGraphView;
class PropertyPanel;

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
    explicit EditorWindow(QWidget* parent = nullptr);
    ~EditorWindow() override;

    /// @brief The session both panels edit through. Exposed so a test can drive it.
    [[nodiscard]] qp::authoring::Session& session() noexcept { return session_; }

    /// @brief The catalog the palette and the panel resolve types against.
    [[nodiscard]] TypeCatalog& catalog() noexcept { return catalog_; }

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

    /// @brief The session's run ledger.
    ///
    /// Exposed so a test can assert that the measurement session **borrows this one** rather
    /// than holding a second. The window had two ledgers once, and the status line and the
    /// measurement panel then disagreed on screen about how many runs the session had.
    [[nodiscard]] qp::runtime::RunLedger& ledger() noexcept { return ledger_; }

    /// @brief Adds a node of `type_name` through the session, and selects it.
    ///
    /// Returns the new node's id, or an invalid id when the type is unknown or the
    /// bus refused the command. Adding through the session is the rule this window
    /// exists to demonstrate: a window that wrote to the graph directly would
    /// produce an edit the canvas is never told about.
    [[nodiscard]] qp::graph::NodeId add_node(const std::string& type_name);

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

private:
    class StatusBridge;

    /// @brief Rebuilds the palette list from the catalog.
    void build_palette();
    /// @brief Refreshes the status line from the session and the ledger.
    void refresh_status();
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

    qp::authoring::Session session_{};
    // The document and the file menu's behaviour. It owns the `Document` -- the canvas borrows it from
    // here -- and registers itself as a session listener, because the dirty flag has to follow every edit
    // rather than the ones this window happens to start.
    qp::views::model::DocumentController document_controller_{
        session_, qp::views::model::document_formats()};
    TypeCatalog catalog_{};
    qp::authoring::PortUiRegistry port_ui_{};
    qp::authoring::Registry capabilities_{};
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

    // The run action. The binders are **not** owned here: `views` must not depend on `plugins`, so the
    // application mounts them through `views::model::mount_execution_binder` and the window consults
    // whatever is there. A window that named a plugin by type could not be built without that plugin,
    // which would make the plugin split a claim rather than a property.
    std::unique_ptr<qp::views::model::RunController> run_controller_{};
    QAction* run_action_ = nullptr;

    // The File menu's actions, kept so a test can invoke the same path the menu does.
    QAction* new_action_ = nullptr;
    QAction* open_action_ = nullptr;
    QAction* save_action_ = nullptr;
    QAction* save_as_action_ = nullptr;
    QAction* export_action_ = nullptr;

    /// @brief Whether the run's clamp count has been pushed into the confidence model yet.
    ///
    /// The count belongs to the operator rather than to the trace, so it arrives after a run rather
    /// than during one. Tracked here so the seed can note it exactly once without the window having
    /// to ask an operator it does not own.
    bool clamps_noted_ = false;
    QLabel* status_ = nullptr;
    std::unique_ptr<StatusBridge> status_bridge_;
    int next_node_index_ = 1;
};

}  // namespace qp::views
