/**
 * @file demo_library.hpp
 * @brief A small built-in node library, so the editor has something to edit.
 *
 * ## What this is, and what it deliberately is not
 *
 * This is **not** the platform's physics. The real models, instruments and kernels
 * are plugins, and the plan tree is explicit that they live under `plugins/`.
 * What is here is the minimum needed to exercise the editor end to end: a handful
 * of types with ports, ranges and units, chosen so that every UI path the editor
 * has actually gets used --
 *
 *   - a parameter with a bounded range and a unit (the commonest control),
 *   - a parameter with no bounds (to prove the panel does not invent 0..100),
 *   - a choice parameter (to prove the editor renders one at all),
 *   - a boolean,
 *   - a type that may appear in the particle domain and one that may not, so the
 *     domain cap is visible rather than theoretical,
 *   - a type with no inputs (a source) and one with no outputs (a sink).
 *
 * Keeping it small is the point: a demo library that grew into a physics package
 * would be a plugin living in the view layer, which is exactly the boundary this
 * project exists to hold.
 *
 * @ownership   owns (the descriptors it builds)
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   Every descriptor has a non-empty type_name and a unique one
 * @errors      reports failure through diag::Result
 * @frozen      no
 * @tests       views.demo.library_has_expected_types, views.catalog.grouped_by_category
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/views/model/type_catalog.hpp>

namespace qp::views {

/// @brief Registers the built-in demonstrator types into `catalog`.
///
/// Idempotent in the sense that re-registering into a fresh catalog is expected;
/// registering twice into the *same* catalog fails on the first duplicate, which
/// the caller can either treat as an error or ignore. Returns the first failure
/// rather than continuing, so a caller that cares can tell that the library is
/// incomplete -- a palette missing half its entries is worse than a reported error.
///
/// @ownership   observes (`catalog` outlives the calls)
/// @thread      ui
/// @pre         none
/// @post        The catalog holds the demonstrator types
/// @invariant   Does not modify types it did not add
/// @errors      Returns the first registration failure (invalid_argument /
///              duplicate_connection)
/// @complexity  O(types)
/// @nondet      none
/// @frozen      no
/// @tests       views.demo.library_has_expected_types
[[nodiscard]] qp::diag::Result<void> register_demo_library(TypeCatalog& catalog) noexcept;

}  // namespace qp::views
