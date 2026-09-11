/**
 * @file test_floating_point_env.cpp
 * @brief Evaluation-environment regression test: guards the toolchain premises that the
 *
 * Background (a real trap this project fell into)
 * --------------------
 * The local MinGW targets **32-bit** (`g++ -dumpmachine` = i686-w64-mingw32),
 * so `FLT_EVAL_METHOD == 2`: float expressions evaluate in **x87 80-bit extended precision**.
 *
 * Consequences:
 *   - `static_assert(pow<2>(Length{1.1}).value() == 1.1 * 1.1)` **fails**, because the
 *     arithmetic inside `static_assert` is computed at 80 bits while the run-time
 *     `.value()` is a 64-bit result, so the last few bits differ. **Legal**, not a defect.
 *   - Likewise, a golden regression comparing float approximations with `static_assert` would
 *
 * Conclusion (already written into standards/test-taxonomy.md section 5):
 *   - `STATIC_REQUIRE` is only for **exactly representable** float values (0.25, 9.0, 10000.0)
 *     or for integer / type-level assertions.
 *   - Every other float comparison uses `REQUIRE` (comparing in one evaluation environment).
 *   - A golden regression must record **the toolchain premise this test asserts**, so a change
 *
 * Its job: turn that premise into **an executable assertion**.
 * If a 64-bit toolchain ever replaces this one (FLT_EVAL_METHOD == 0), this test fails and
 * tells the maintainer to re-examine every float assertion -- exactly the friction we want.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   the toolchain premises recorded here hold on the current platform
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no (the toolchain may change; if it does, change this file)
 * @tests       fp.eval_method_is_recorded, fp.long_double_width_is_recorded,
 *              fp.static_assert_reliable_only_for_exact_values
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <cfloat>
#include <cstdint>
#include <limits>

TEST_CASE("fp.eval_method_is_recorded", "[fp][environment][golden]") {
    // Record the actual evaluation method. It fails on a toolchain change, prompting review.
    //
    //   0 = evaluate at type precision (x64 / SSE2, the ideal case)
    //   1 = float promoted to double
    //   2 = evaluate at long double (x87, the current 32-bit MinGW)
#if defined(__i386__) || defined(_M_IX86)
    STATIC_REQUIRE(FLT_EVAL_METHOD == 2);
#else
    STATIC_REQUIRE(FLT_EVAL_METHOD == 0);
#endif
}

TEST_CASE("fp.long_double_width_is_recorded", "[fp][environment][golden]") {
    // x87's long double is 80 bits (12 or 16 bytes depending on alignment).
    // It is recorded to explain "why compile time and run time can disagree".
#if defined(__i386__) || defined(_M_IX86)
    STATIC_REQUIRE(sizeof(long double) == 12 || sizeof(long double) == 16);
#else
    STATIC_REQUIRE(sizeof(long double) == 8);
#endif
    STATIC_REQUIRE(sizeof(double) == 8);
    STATIC_REQUIRE(std::numeric_limits<double>::is_iec559);
    STATIC_REQUIRE(std::numeric_limits<double>::digits == 53);
}

TEST_CASE("fp.static_assert_reliable_only_for_exact_values", "[fp][environment]") {
    // Exactly representable decimal values: compile time and run time agree, STATIC_REQUIRE is safe.
    STATIC_REQUIRE(qp::units::pow<2>(qp::units::Length{3.0}).value() == 9.0);
    STATIC_REQUIRE(qp::units::pow<-1>(qp::units::Time{4.0}).value() == 0.25);
    STATIC_REQUIRE(qp::units::pow<4>(qp::units::Length{10.0}).value() == 10000.0);

    // Inexact values: **must not** be compared with STATIC_REQUIRE, even when both spellings
    // are bit-identical at run time. Measured: pow<-2>(10) and 0.01 are both
    // 0x3f847ae147ae147b at run time, yet static_assert at 80 bits fails. Hence this file.
    REQUIRE(qp::units::pow<-2>(qp::units::Length{10.0}).value() == 0.01);
    REQUIRE(qp::units::pow<-2>(qp::units::Length{10.0}).value() == 1.0 / 100.0);

    // In one evaluation environment (both run time) square-and-multiply and naive repeated
    constexpr double k = 1.1;
    REQUIRE(qp::units::pow<2>(qp::units::Length{k}).value() == k * k);
}
