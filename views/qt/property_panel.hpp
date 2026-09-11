/**
 * @file property_panel.hpp
 * @brief The property panel: a port's own declaration decides how it is edited.
 *
 * ## The promise under test
 *
 * From the charter: adding a port type must not require touching panel code. This
 * panel therefore contains **no per-type logic**. It asks the descriptor what kind
 * of value the port holds, asks `authoring::portui` how that type is edited, and
 * builds whatever the answer describes. A new port type arrives with its
 * description and gets an editor without a line changing here.
 *
 * ## Why editor selection is a free function
 *
 * `choose_editor` is deliberately **not** a member of the widget. It is the part
 * most likely to be wrong -- a range that should mean a slider, a choice list that
 * should mean a combo -- and a decision inside a Qt widget can only be tested by
 * building a Qt widget. As a free function over two plain structs it is covered by
 * an ordinary unit test, which is what makes the promise above checkable rather
 * than aspirational.
 *
 * ## Why an edit goes through the session
 *
 * A parameter edit is a `graph::SetParam` command applied through
 * `authoring::Session`. It must not touch the graph directly: this widget is
 * registered as a document-wide "one graph, one undo stack" concern, and a panel
 * that wrote to the graph behind the bus would produce an edit that no other view
 * is told about and that undo cannot reverse.
 *
 * @ownership   owns (the widgets it builds)
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   No per-port-type branch exists anywhere in this module
 * @errors      reports mutations that the bus refused through its Result
 * @frozen      no
 * @tests       qt.views.properties.row_per_parameter, qt.views.properties.edit_goes_through_session
 */
#pragma once

#include <QWidget>

#include <qp/authoring/commands/session.hpp>
#include <qp/authoring/document/document.hpp>
#include <qp/authoring/portui/port_ui.hpp>
#include <qp/graph/ir.hpp>
#include <qp/views/model/editor_choice.hpp>
#include <qp/views/model/type_catalog.hpp>

#include <optional>
#include <string>

class QFormLayout;

namespace qp::views {

/**
 * @brief A form that edits the parameters of one selected node.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         the session, catalog and registry outlive the widget
 * @post        none
 * @invariant   Every edit is applied as a graph::SetParam through the session
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.properties.edit_goes_through_session
 */
class PropertyPanel final : public QWidget {
    Q_OBJECT

public:
    PropertyPanel(qp::authoring::Session& session, const TypeCatalog& catalog,
                  const qp::authoring::PortUiRegistry& port_ui, QWidget* parent = nullptr);

    /// @brief Shows the parameters of `node`. An invalid id shows an empty panel.
    void show_node(qp::graph::NodeId node);

    /// @brief The node currently displayed, if any.
    [[nodiscard]] std::optional<qp::graph::NodeId> current_node() const noexcept {
        return current_;
    }

    /// @brief Number of parameter rows currently built. Used by the tests.
    [[nodiscard]] int row_count() const noexcept;

    /// @brief The most recent mutation error, so the panel can report a refusal.
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

Q_SIGNALS:
    /// @brief Emitted when a parameter edit was refused, with the reason.
    void edit_failed(const QString& reason);

private:
    /// @brief Rebuilds every row for the current node.
    void rebuild_rows();

    /// @brief Applies one parameter value through the session.
    void apply_value(qp::graph::NodeId node, qp::graph::PortNumber port,
                     qp::ports::Value value);

    qp::authoring::Session& session_;
    const TypeCatalog& catalog_;
    const qp::authoring::PortUiRegistry& port_ui_;
    QFormLayout* form_ = nullptr;
    std::optional<qp::graph::NodeId> current_{};
    std::string last_error_{};
};

}  // namespace qp::views
