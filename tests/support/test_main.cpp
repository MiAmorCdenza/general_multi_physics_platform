/**
 * @file test_main.cpp
 * @brief Entry point of every test executable. It does one thing: **keep a crash from popping up a dialog**.
 *
 * ## Why this file must exist
 *
 * One death test (`diag.contract.violation_terminates`) **deliberately** calls
 * `std::terminate` -> `abort()`, to verify that "violating @pre always terminates the process".
 *
 * But on Windows the MSVC debug runtime pops a **modal dialog** when it reaches `abort()`
 * ("Debug Error! abort() has been called / Press Retry to debug").
 * Modal means **the process blocks until somebody clicks the button**.
 *
 * Consequence: any automated run (local ctest, CI) **hangs forever**.
 * This project has already paid for that lesson: several MSVC test runs timed out,
 * and the log showed them stopping near `qp_test_diag.death.contract_violation`.
 *
 * ## The fix: turn off the "error UI", keep the "error message"
 *
 * - `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)`
 *   turns off the Windows error reporting hook (the source of that dialog).
 * - `_CrtSetReportMode(_CRT_ASSERT/_CRT_ERROR, _CRTDBG_MODE_FILE)` +
 *   `_CrtSetReportFile(..., _CRTDBG_FILE_STDERR)`
 *   redirects CRT diagnostics to stderr instead of a dialog.
 *
 * Key trade-off: **turn off the UI only, never the information**. The contract-violation text still goes to stderr,
 * so CTest's `PASS_REGULAR_EXPRESSION "contract violation"` can still verify
 * that "the termination really was a contract violation" rather than an arbitrary crash.
 *
 * ## Why not Catch2::Catch2WithMain
 *
 * That default main returns **before** the tests start, with no chance to set these switches.
 * So every test target in this project links `Catch2::Catch2` + this file.
 *
 * @ownership   pure
 * @thread      main (process startup, single-threaded)
 * @pre         none
 * @post        abort/CRT diagnostics no longer pop a dialog; the diagnostic text still goes to stderr
 * @invariant   test failure information is unaffected; it merely no longer needs a human click
 * @errors      noexcept
 * @frozen      no
 */
#include <catch2/catch_session.hpp>

#include <cstdio>

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace {

/// @brief Change the CRT crash behaviour to "write to stderr" instead of "pop a dialog".
void configure_crash_reporting() noexcept {
#if defined(_MSC_VER)
    // Turn off Windows error reporting: the source of the modal dialog.
    // Deliberately keep every output other than _WRITE_ABORT_MSG -- the information must survive.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // CRT assert/error -> write to a file (stderr), not to a window.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);

    // Unbuffered: the process may terminate immediately, and buffered diagnostics would be lost.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    configure_crash_reporting();
    return Catch::Session().run(argc, argv);
}
