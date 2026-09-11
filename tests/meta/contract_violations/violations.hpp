/**
 * @file contract_violations.hpp
 * @brief Contract counter-examples: every violation below must be caught by check_contracts.py.
 *
 * This file is **not** production code; it is used only by tests/meta/check_contract_checker.py.
 * Corresponds to standards/enforcement.md section 8: an unverified gate is no gate at all.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The compliant samples in this file produce no violation
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      no
 * @tests       meta.sample.case, meta.another.case
 */
#pragma once

namespace qp::meta {

    // -- counterexample 1: missing @tests (must trigger C2) -------------------
/**
 * @brief A function missing its @tests field.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns the input
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 */
[[nodiscard]] inline int missing_tests(int x) noexcept { return x; }

    // -- counterexample 2: @tests names a missing case (must trigger C4) ------
/**
 * @brief References a test case that does not exist.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns the input
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       meta.this_test_case_does_not_exist
 */
[[nodiscard]] inline int bad_test_id(int x) noexcept { return x; }

    // -- counterexample 3: @errors vs noexcept (must trigger C5) --------------
/**
 * @brief Claims noexcept while the signature may throw.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns the input
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       meta.sample.case
 */
[[nodiscard]] inline int noexcept_mismatch(int x) { return x; }

    // -- counterexample 4: illegal @ownership value (must trigger C3) --------
/**
 * @brief ownership carries a value that does not exist.
 *
 * @ownership   whatever
 * @thread      any
 * @pre         none
 * @post        Returns the input
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       meta.sample.case
 */
[[nodiscard]] inline int bad_ownership(int x) noexcept { return x; }

    // -- positive sample: compliant, no violation (must trigger nothing) -----
/**
 * @brief A compliant sample, confirming that the gate does not cry wolf.
 *
 * @ownership   pure
 * @thread      any
 * @pre         x is any integer
 * @post        Returns x
 * @invariant   none
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       meta.sample.case
 */
[[nodiscard]] inline int good_sample(int x) noexcept { return x; }

}  // namespace qp::meta
