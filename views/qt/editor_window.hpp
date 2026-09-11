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
 * @tests       qt.views.editor_window.shares_one_session
 */
#pragma once

#include <QMainWindow>

#include <qp/authoring/capability/capability.hpp>
#include <qp/authoring/commands/session.hpp>
#include <qp/authoring/document/document.hpp>
#include <qp/authoring/portui/port_ui.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/views/model/type_catalog.hpp>
#include <qp/views/model/demo_library.hpp>

#include <memory>

class QLabel;

namespace qp::views {

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

    /// @brief Adds a node of `type_name` through the session, and selects it.
    ///
    /// Returns the new node's id, or an invalid id when the type is unknown or the
    /// bus refused the command. Adding through the session is the rule this window
    /// exists to demonstrate: a window that wrote to the graph directly would
    /// produce an edit the canvas is never told about.
    [[nodiscard]] qp::graph::NodeId add_node(const std::string& type_name);

    /// @brief Seeds the window with a small graph so it is not empty on start.
    void seed_demo_graph();

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

    qp::authoring::Session session_{};
    qp::authoring::Document document_{};
    TypeCatalog catalog_{};
    qp::authoring::PortUiRegistry port_ui_{};
    qp::authoring::Registry capabilities_{};
    qp::runtime::RunLedger ledger_{};

    NodeGraphView* canvas_ = nullptr;
    PropertyPanel* properties_ = nullptr;
    QLabel* status_ = nullptr;
    std::unique_ptr<StatusBridge> status_bridge_;
    int next_node_index_ = 1;
};

}  // namespace qp::views
