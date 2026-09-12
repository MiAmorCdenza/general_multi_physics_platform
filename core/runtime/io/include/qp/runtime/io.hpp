/**
 * @file qp/runtime/io.hpp
 * @brief The single entry point of the io module.
 *
 * The export contract and the registry that finds a format. Every format is a
 * plugin; this module writes no file itself.
 *
 * The one thing the contract refuses to lose is the **uncertainty**. A CSV of bare
 * numbers is exactly the artifact this platform exists to replace, so a format that
 * cannot carry an uncertainty must declare that, and a caller who needs it can
 * refuse the format rather than publish a table that lost its error bars.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No format is implemented in this module
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       io.registry.register_and_find
 */
#pragma once

#include <qp/runtime/io/io.hpp>

namespace qp::runtime {

/// @brief ABI version of the io module. Bump when FormatDesc's or ExportRequest's
///        field set changes.
inline constexpr int kIoAbiVersion = 1;

}  // namespace qp::runtime
