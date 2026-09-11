/**
 * @file contract.hpp
 * @brief Precondition checking: the enforcement tool for `@pre`.
 *
 * The contract standard (standards/function-contract.md) requires a violated `@pre`
 * to be a **programming error**: an assert, not an error code. This file is that assert.
 *
 * Behavior contract:
 *   - Debug (NDEBUG undefined): a false condition calls `std::terminate`; the message has
 *     file/line/expression. **No assert()**: NDEBUG erases it, yet debug must always stop.
 *   - Release: a false condition immediately calls `std::terminate`.
 *     Deliberately **not "undefined behavior"**: a `@pre` violation is a programming error,
 *     but errors must be **deterministic**, else "same seed reproduces" (charter R2) flakes.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A violated @pre always terminates the process: no return, no continuation
 * @errors      noexcept (no function in this file throws)
 * @frozen      no
 * @tests       diag.contract.holds_does_nothing, diag.contract.violation_terminates
 */
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace qp::diag {

/**
 * @brief Termination on contract violation. Its own function, easy to test and replace.
 *
 * ## Why this message must be **pure ASCII**
 *
 * This text is read by the **test framework and CI**, not by a person as final UI copy:
 * `tests/CMakeLists.txt` uses `PASS_REGULAR_EXPRESSION "contract violation"` to
 * confirm the death test died of a contract violation, not of some random crash.
 *
 * Non-ASCII characters here trip a codepage mismatch on Chinese Windows: the program
 * writes UTF-8 bytes while the console/CTest decodes them as GBK; the regex therefore
 * does **not match** -- the gate produces a false failure.
 * This project has measured the phenomenon: under MSVC a death test was judged failed.
 *
 * Hence: a diagnostic's **location and verdict** is always ASCII, while user-facing
 * Chinese copy lives in the display layer (`Diagnostic`) and is never regex-matched.
 */
[[noreturn]] inline void contract_violation(const char* expr, const char* file,
                                            int line) noexcept {
    std::fprintf(stderr, "\n[qp] contract violation (@pre does not hold)\n  %s\n  at %s:%d\n\n",
                 expr, file, line);
    std::fflush(stderr);
    std::terminate();
}

/**
 * @brief Precondition check. A false condition terminates.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns normally when the condition holds, with no side effects
 * @invariant   A false condition **always** terminates the process (no return)
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       diag.contract.holds_does_nothing, diag.contract.violation_terminates
 */
inline void precondition(bool holds, const char* expr, const char* file, int line) noexcept {
    if (!holds) contract_violation(expr, file, line);
}

}  // namespace qp::diag

/// @brief Checks a precondition. False -> terminate (see contract.hpp for details).
#define QP_PRECONDITION(expr) \
    ::qp::diag::precondition(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

/// @brief Checks an invariant. Same semantics as QP_PRECONDITION, named apart for grepping.
#define QP_INVARIANT(expr) \
    ::qp::diag::precondition(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
// build-system dependency probe
