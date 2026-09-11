/**
 * @file editor_choice.cpp
 * @brief Implementation of the editor-selection rule.
 */
#include <qp/views/model/editor_choice.hpp>

namespace qp::views {

qp::authoring::PortUiDesc fallback_description(const std::string& port_type) {
    // Delegated rather than reimplemented: the panel and the registry must agree on
    // what an unregistered type looks like, and two independent fallbacks would
    // drift into disagreeing about which control an unknown type gets.
    return qp::authoring::PortUiRegistry::fallback(port_type);
}

EditorChoice choose_editor(const qp::graph::PortDesc& port,
                           const qp::authoring::PortUiDesc& description) noexcept {
    EditorChoice choice;

    // A connectable port is not edited in a property panel; the canvas wires it.
    if (port.connectable) return choice;

    // From here on the port is a parameter.
    choice.bounded = port.has_range;
    choice.has_unit = !port.unit_symbol.empty();

    // An enum with a choice list is a choice, and the options come from the port
    // rather than from the UI description: the port is what the model accepts.
    if (has_choices(port)) {
        choice.kind = qp::authoring::EditorKind::choice;
        choice.from_port_enum = true;
        return choice;
    }

    // A registered description refines the control. Portui's entire purpose is to
    // let a plugin decide this for its own type.
    if (!description.port_type.empty() &&
        description.editor != qp::authoring::EditorKind::read_only) {
        choice.kind = description.editor;
        // A description may bound a value the port left unbounded; that is
        // legitimate, because the plugin knows its own type's useful range.
        if (description.minimum.has_value() || description.maximum.has_value()) {
            choice.bounded = true;
        }
        if (description.dimension.has_value()) choice.has_unit = true;
        return choice;
    }

    switch (port.type) {
        case qp::ports::kScalarF64:
        case qp::ports::kScalarF32:
        case qp::ports::kInt64:
            choice.kind = qp::authoring::EditorKind::number;
            break;
        case qp::ports::kBool:
            choice.kind = qp::authoring::EditorKind::boolean;
            break;
        case qp::ports::kString:
            choice.kind = qp::authoring::EditorKind::text;
            break;
        case qp::ports::kEnum:
            // An enum port with no choice list cannot be edited as a choice, and
            // inventing options would offer values the model rejects.
            choice.kind = qp::authoring::EditorKind::read_only;
            break;
        default:
            // An unknown or non-editable type: read_only, never a number. A
            // numeric control that cannot represent the value shows a 0 where the
            // user's reference used to be, and the user believes it.
            choice.kind = qp::authoring::EditorKind::read_only;
            break;
    }
    return choice;
}

}  // namespace qp::views
