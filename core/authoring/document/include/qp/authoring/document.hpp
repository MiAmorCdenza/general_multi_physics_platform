/**
 * @file qp/authoring/document.hpp
 * @brief The single entry point of the document module.
 *
 * What a saved document is: identity, a place on disk, and the layout metadata
 * that belongs to each view rather than to the graph.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The core never interprets a view's layout bytes
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       authoring.document.layouts_are_slotted_by_view
 */
#pragma once

#include <qp/authoring/document/document.hpp>

namespace qp::authoring {

/// @brief ABI version of the document module. Bump when Document's observable
///        behaviour or ViewLayouts' ordering guarantee changes.
inline constexpr int kDocumentAbiVersion = 1;

}  // namespace qp::authoring
