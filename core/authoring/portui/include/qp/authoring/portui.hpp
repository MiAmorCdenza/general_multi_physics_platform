/**
 * @file qp/authoring/portui.hpp
 * @brief The single entry point of the portui module.
 *
 * Port-type-driven property panels: each port type declares how it is edited, and
 * one generic panel renders any declaration. Only descriptions cross into the
 * view layer -- no widgets, no Qt.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No Qt type appears in this module
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       portui.registry.fallback_is_always_available
 */
#pragma once

#include <qp/authoring/portui/port_ui.hpp>

namespace qp::authoring {

/// @brief ABI version of the portui module. Bump when PortUiDesc's field set changes.
inline constexpr int kPortUiAbiVersion = 1;

}  // namespace qp::authoring
