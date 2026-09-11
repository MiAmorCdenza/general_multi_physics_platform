/**
 * @file editor_choice.hpp
 * @brief Which control edits a port, decided from the port and its type's description.
 *
 * ## Why this lives in the Qt-free model layer
 *
 * This is the part of a property panel most likely to be wrong: a declared range
 * that should mean a bounded control, an enum that should mean a choice list, an
 * unregistered type that must **not** get a numeric field. Every one of those
 * mistakes is invisible in a screenshot -- the panel still draws, it just edits the
 * wrong thing -- so it has to be asserted rather than looked at.
 *
 * A decision buried inside a `QWidget` can only be tested by constructing a
 * `QWidget`, which drags a Qt test harness and an event loop into the suite. As a
 * free function over two plain structs it is covered by an ordinary unit test, and
 * the promise "adding a port type needs no panel code" becomes checkable instead of
 * aspirational.
 *
 * ## The precedence, and why each step is where it is
 *
 *   1. **Not connectable means parameter.** `PortDesc::connectable` separates "a
 *      value the user types" from "a socket to wire". A parameter drawn as a socket
 *      invites a connection the model refuses -- a UI offering what the model
 *      forbids is worse than one that omits it.
 *   2. **A declared range means bounded.** `has_range` is the port saying it has a
 *      range. Inventing one for a port that declared none rejects legitimate
 *      values, and the user has no way to enter them.
 *   3. **An enum with choices is a choice, and the options come from the port.**
 *      The port is what the model accepts; a UI description offering different
 *      options would let a user select a value the model then rejects.
 *   4. **A registered UI description refines the control.** This is the whole point
 *      of `authoring::portui`: a plugin decides how its own type is edited without
 *      the panel knowing the type exists.
 *   5. **Otherwise the port type's default control**, and for an unregistered type
 *      that is `read_only`. Never a number: an unknown type may hold text or a
 *      handle, and a numeric control that cannot represent the value displays a 0
 *      where the user's reference used to be -- and the user believes it.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The returned kind can represent the port's type
 * @invariant   Same inputs yield the same choice
 * @errors      noexcept
 * @frozen      no
 * @tests       views.editor_choice.parameter_versus_socket,
 *              views.editor_choice.declared_range_is_bounded,
 *              views.editor_choice.enum_options_come_from_the_port,
 *              views.editor_choice.description_refines_the_control,
 *              views.editor_choice.unregistered_type_is_read_only
 */
#pragma once

#include <qp/authoring/portui/port_ui.hpp>
#include <qp/graph/ir.hpp>
#include <qp/ports/port_type.hpp>

#include <cstdint>
#include <string>

namespace qp::views {

/**
 * @brief What the panel decided to build for one port.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `kind` is always a member of authoring::EditorKind
 * @errors      noexcept
 * @frozen      no
 * @tests       views.editor_choice.parameter_versus_socket
 */
struct EditorChoice final {
    qp::authoring::EditorKind kind = qp::authoring::EditorKind::read_only;
    /// Whether the value should be built with bounds.
    bool bounded = false;
    /// Whether a unit accompanies the value.
    bool has_unit = false;
    /// Whether the options came from the port's own enum rather than a description.
    bool from_port_enum = false;

    [[nodiscard]] friend constexpr bool operator==(const EditorChoice& a,
                                                   const EditorChoice& b) noexcept {
        return a.kind == b.kind && a.bounded == b.bounded && a.has_unit == b.has_unit &&
               a.from_port_enum == b.from_port_enum;
    }
    [[nodiscard]] friend constexpr bool operator!=(const EditorChoice& a,
                                                   const EditorChoice& b) noexcept {
        return !(a == b);
    }
};

/// @brief Whether the port carries a usable discrete choice list.
[[nodiscard]] constexpr bool has_choices(const qp::graph::PortDesc& port) noexcept {
    // Both lists must exist and agree in length. A `choice_labels` longer than
    // `choice_names` would produce a combo whose indices do not match the values
    // the model stores, which is a silent off-by-one in the user's data.
    return !port.choice_labels.empty() &&
           port.choice_labels.size() == port.choice_names.size();
}

/// @brief Decides how to edit one port. See the file comment for the precedence.
[[nodiscard]] EditorChoice choose_editor(
    const qp::graph::PortDesc& port,
    const qp::authoring::PortUiDesc& description) noexcept;

/// @brief The description a panel must use for a port type with no registered UI.
///
/// Exposed so the panel and the tests agree on what "unregistered" means instead
/// of each constructing its own idea of a default and drifting apart.
[[nodiscard]] qp::authoring::PortUiDesc fallback_description(const std::string& port_type);

}  // namespace qp::views
