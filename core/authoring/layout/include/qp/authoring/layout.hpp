/**
 * @file qp/authoring/layout.hpp
 * @brief The single entry point of the layout module.
 *
 * The layout contract: positions in, positions out, and the algorithms are
 * plugins. The registry holds them; the module ships none.
 *
 * @ownership   pure (contract) / owns (the registry)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No algorithm is implemented in this module
 * @errors      noexcept
 * @complexity  --
 * @nondet      only through the seed the caller supplies
 * @frozen      no
 * @tests       authoring.layout.registry_register_and_find
 */
#pragma once

#include <qp/authoring/layout/layout.hpp>

namespace qp::authoring {

/// @brief ABI version of the layout module. Bump when LayoutResult or the
///        registry's observable behaviour changes.
inline constexpr int kLayoutAbiVersion = 1;

}  // namespace qp::authoring
