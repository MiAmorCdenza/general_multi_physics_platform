/**
 * @file qp/authoring/commands.hpp
 * @brief The single entry point of the commands module.
 *
 * One editing session: one graph, one command bus, one undo stack, and one place
 * changes are broadcast from.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The graph is reachable only as a const reference
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       authoring.session.apply_returns_command_result
 */
#pragma once

#include <qp/authoring/commands/session.hpp>

namespace qp::authoring {

/// @brief ABI version of the commands module. Bump when Session's observable
///        behaviour or the Change payload changes.
inline constexpr int kCommandsAbiVersion = 1;

}  // namespace qp::authoring
