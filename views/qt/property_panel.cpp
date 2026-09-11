/**
 * @file property_panel.cpp
 * @brief Implementation of the property panel.
 */
#include "property_panel.hpp"

#include <qp/graph/mutate/command.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QString>

#include <string>
#include <utility>

namespace qp::views {
namespace {

/// @brief An error code as text, for the failure signal.
QString describe(qp::diag::ErrorCode code) {
    const std::string_view text = qp::diag::to_string(code);
    return QString::fromLatin1(text.data(), static_cast<int>(text.size()));
}

/// @brief The label a row shows: the port's label, falling back to its name.
QString row_label(const qp::graph::PortDesc& port) {
    return QString::fromStdString(port.label.empty() ? port.name : port.label);
}

/// @brief A row's caption, with the unit appended when the port carries one.
///
/// The unit is appended rather than shown beside the field on purpose: a value
/// without its unit beside it is how a lab report becomes wrong, and the caption
/// is the one place that always remains visible however narrow the panel gets.
QString row_caption(const qp::graph::PortDesc& port) {
    QString caption = row_label(port);
    if (!port.unit_symbol.empty()) {
        caption += QStringLiteral("  [%1]").arg(QString::fromStdString(port.unit_symbol));
    }
    return caption;
}

}  // namespace

PropertyPanel::PropertyPanel(qp::authoring::Session& session, const TypeCatalog& catalog,
                             const qp::authoring::PortUiRegistry& port_ui, QWidget* parent)
    : QWidget(parent), session_(session), catalog_(catalog), port_ui_(port_ui) {
    form_ = new QFormLayout(this);
    form_->setContentsMargins(10, 10, 10, 10);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    show_node(qp::graph::NodeId{});
}

void PropertyPanel::show_node(qp::graph::NodeId node) {
    current_ = node.valid() ? std::optional<qp::graph::NodeId>{node} : std::nullopt;
    rebuild_rows();
}

void PropertyPanel::rebuild_rows() {
    while (form_->rowCount() > 0) {
        form_->removeRow(0);
    }
    last_error_.clear();

    if (!current_.has_value()) {
        form_->addRow(new QLabel(QStringLiteral("Select a node to edit its parameters."), this));
        return;
    }

    const qp::graph::Node* node = session_.graph().find_node(*current_);
    if (node == nullptr) {
        // The node was deleted between the selection and this rebuild. Saying so is
        // better than an empty panel, which reads as "this node has no parameters".
        form_->addRow(new QLabel(QStringLiteral("That node no longer exists."), this));
        current_.reset();
        return;
    }

    const qp::graph::NodeDesc* desc = catalog_.find(node->type_name);
    if (desc == nullptr) {
        // An unregistered type is a normal state: a document can be opened on a
        // machine that has not installed the plugin. The panel says which type is
        // missing rather than showing an empty form, because "I cannot edit this"
        // and "there is nothing to edit" need different responses from the user.
        form_->addRow(new QLabel(QStringLiteral("Type \"%1\" is not registered, so its "
                                                "parameters cannot be edited.")
                                     .arg(QString::fromStdString(node->type_name)),
                                 this));
        return;
    }

    const auto heading = new QLabel(
        QStringLiteral("<b>%1</b><br/><span style=\"color:#9aa2ac\">%2</span>")
            .arg(QString::fromStdString(desc->label.empty() ? desc->type_name : desc->label),
                 QString::fromStdString(desc->type_name)),
        this);
    heading->setTextFormat(Qt::RichText);
    form_->addRow(heading);

    const qp::graph::NodeId id = *current_;
    for (const qp::graph::PortDesc& port : desc->inputs) {
        if (port.connectable) continue;   // wired in the canvas, not edited here

        const qp::authoring::PortUiDesc description =
            port_ui_.describe(std::to_string(port.type));
        const EditorChoice choice = choose_editor(port, description);
        const qp::ports::Value initial = node->param(port.number);

        switch (choice.kind) {
            case qp::authoring::EditorKind::boolean: {
                auto* box = new QCheckBox(this);
                box->setChecked(initial.as_bool());
                connect(box, &QCheckBox::toggled, this, [this, id, port](bool on) {
                    apply_value(id, port.number, qp::ports::Value{on});
                });
                form_->addRow(row_caption(port), box);
                break;
            }
            case qp::authoring::EditorKind::text: {
                auto* field = new QLineEdit(this);
                field->setText(QString::fromStdString(initial.as_text()));
                connect(field, &QLineEdit::editingFinished, this, [this, id, port, field] {
                    apply_value(id, port.number, qp::ports::Value{field->text().toStdString()});
                });
                form_->addRow(row_caption(port), field);
                break;
            }
            case qp::authoring::EditorKind::choice: {
                auto* combo = new QComboBox(this);
                for (const std::string& label : port.choice_labels) {
                    combo->addItem(QString::fromStdString(label));
                }
                const auto index = static_cast<int>(initial.as_i64());
                if (index >= 0 && index < combo->count()) combo->setCurrentIndex(index);
                connect(combo, &QComboBox::currentIndexChanged, this, [this, id, port](int index) {
                    apply_value(id, port.number,
                                qp::ports::Value{static_cast<std::int64_t>(index)});
                });
                form_->addRow(row_caption(port), combo);
                break;
            }
            case qp::authoring::EditorKind::number: {
                auto* spin = new QDoubleSpinBox(this);
                // An unbounded parameter gets a wide but finite range. Leaving Qt's
                // default 0..99.99 would silently reject a legitimate value, and a
                // genuinely unbounded spin box does not exist; the caption carries
                // the unit so the number is never ambiguous.
                if (port.has_range) {
                    spin->setRange(port.min_value, port.max_value);
                } else if (description.minimum.has_value() || description.maximum.has_value()) {
                    spin->setRange(description.minimum.value_or(-1.0e9),
                                   description.maximum.value_or(1.0e9));
                } else {
                    spin->setRange(-1.0e12, 1.0e12);
                }
                spin->setDecimals(6);
                if (port.step > 0.0) spin->setSingleStep(port.step);
                spin->setValue(initial.as_f64());
                connect(spin, &QDoubleSpinBox::valueChanged, this, [this, id, port](double v) {
                    apply_value(id, port.number, qp::ports::Value{v});
                });
                form_->addRow(row_caption(port), spin);
                break;
            }
            case qp::authoring::EditorKind::reference:
            case qp::authoring::EditorKind::read_only:
            default: {
                const qp::ports::Value shown = initial;
                auto* label = new QLabel(QString::fromStdString(
                    shown.valid() ? std::to_string(shown.as_f64()) : std::string{"(unset)"}), this);
                label->setTextInteractionFlags(Qt::TextSelectableByMouse);
                form_->addRow(row_caption(port), label);
                break;
            }
        }
    }

    if (form_->rowCount() <= 1) {
        form_->addRow(new QLabel(QStringLiteral("This type declares no editable parameters."),
                                 this));
    }
}

void PropertyPanel::apply_value(qp::graph::NodeId node, qp::graph::PortNumber port,
                               qp::ports::Value value) {
    // Through the session, always. A panel that wrote to the graph directly would
    // produce an edit no other view is told about and that undo cannot reverse --
    // and the canvas would keep drawing the old value, which the user would read
    // as the edit having failed.
    qp::graph::SetParam command;
    command.id = node;
    command.port = port;
    command.value = std::move(value);

    const auto applied = session_.apply(command);
    if (!applied.has_value()) {
        last_error_ = std::string{qp::diag::to_string(applied.error())};
        Q_EMIT edit_failed(describe(applied.error()));
        return;
    }
    last_error_.clear();
}

int PropertyPanel::row_count() const noexcept { return form_->rowCount(); }

}  // namespace qp::views
