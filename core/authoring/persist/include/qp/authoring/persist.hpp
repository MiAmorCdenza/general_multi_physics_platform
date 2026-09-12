/**
 * @file qp/authoring/persist.hpp
 * @brief The single entry point of the persistence contract module.
 *
 * Saving and loading a document is a format, and a format is content. What the platform owns is the
 * shape of the question a format answers: a document borrows its way out to bytes and owns its way back
 * from them, and every failure has a name.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A format registered by a plugin is reached only through `IDocumentFormat`
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       persist.format.describes_itself
 */
#pragma once

#include <qp/authoring/persist/persist.hpp>

namespace qp::authoring {

/// @brief ABI version of the persistence contract. Bump when `DocumentRefusal`'s tags,
///        `DocumentFormatDesc`'s meaning, or either direction's guarantee changes.
inline constexpr int kPersistAbiVersion = 1;

}  // namespace qp::authoring
