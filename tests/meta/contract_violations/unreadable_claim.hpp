/**
 * @file unreadable_claim.hpp
 * @brief The counterexample for C7: a claim written where the gate does not read.
 *
 * The gate reads block comments. A claim written in doc-comment lines is, to the gate, prose -- so the case it
 * names is reported as an **orphan**, and the author is sent looking for a claim that is in the file. This fixture
 * is one such claim; `violations.hpp` beside it is the compliant sample, whose contracts are all blocks and which
 * must produce nothing.
 */
#pragma once

namespace fixture {

/// @brief A function whose claim the gate cannot read.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        none
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       meta.sample.case
[[nodiscard]] inline int unreadable() noexcept { return 1; }

}  // namespace fixture
