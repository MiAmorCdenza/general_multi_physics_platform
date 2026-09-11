/**
 * @file test_floating_point_env.cpp
 * @brief 求值环境回归测试：守住"浮点断言写法"所依赖的工具链前提。
 *
 * 背景（真实踩坑记录）
 * --------------------
 * 本机 MinGW 是 **32 位**目标（`g++ -dumpmachine` = i686-w64-mingw32），
 * 因此 `FLT_EVAL_METHOD == 2`：浮点表达式按 **x87 80 位扩展精度**求值。
 *
 * 后果：
 *   - `static_assert(pow<2>(Length{1.1}).value() == 1.1 * 1.1)` 会**失败**，
 *     因为 `static_assert` 里的算术在 80 位下算，而运行期的 `.value()`
 *     是 64 位结果，两者最后几位不同。这是**合法**的差异，不是实现缺陷。
 *   - 同理，黄金回归若用 `static_assert` 比对浮点近似值，会给出假失败。
 *
 * 结论（已写入 standards/test-taxonomy.md §5）：
 *   - `STATIC_REQUIRE` 只能用于**精确可表示**的浮点值（如 0.25、9.0、10000.0）
 *     或整数/类型级断言。
 *   - 其余浮点比对一律用 `REQUIRE`（同一求值环境下比较）。
 *   - 黄金回归必须记录**本测试所断言的工具链前提**，换编译器/架构时能立刻发现。
 *
 * 本文件的作用：把这个前提变成**可执行的断言**。
 * 若将来换成 64 位工具链（FLT_EVAL_METHOD == 0），本测试会失败并提醒
 * 维护者重新审视所有浮点断言——这正是我们想要的摩擦。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   本文件所记录的工具链前提在当前平台上成立
 * @errors      noexcept
 * @complexity  —
 * @nondet      none
 * @frozen      否（工具链可换，换了就要改这里）
 * @tests       fp.eval_method_is_recorded, fp.long_double_width_is_recorded,
 *              fp.static_assert_reliable_only_for_exact_values
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/units.hpp>

#include <cfloat>
#include <cstdint>
#include <limits>

TEST_CASE("fp.eval_method_is_recorded", "[fp][environment][golden]") {
    // 记录实际的求值方法。这个断言在换工具链时会失败，提醒重新审视浮点断言。
    //
    //   0 = 按类型精度求值（x64 / SSE2，理想情况）
    //   1 = float 提升到 double
    //   2 = 按 long double 求值（x87，当前 MinGW 32 位）
#if defined(__i386__) || defined(_M_IX86)
    STATIC_REQUIRE(FLT_EVAL_METHOD == 2);
#else
    STATIC_REQUIRE(FLT_EVAL_METHOD == 0);
#endif
}

TEST_CASE("fp.long_double_width_is_recorded", "[fp][environment][golden]") {
    // x87 的 long double 是 80 位（占 12 或 16 字节，取决于对齐）。
    // 记录它是为了解释"为什么编译期与运行期会不一致"。
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
    // 精确可表示的十进制值：编译期与运行期必然一致，STATIC_REQUIRE 安全。
    STATIC_REQUIRE(qp::units::pow<2>(qp::units::Length{3.0}).value() == 9.0);
    STATIC_REQUIRE(qp::units::pow<-1>(qp::units::Time{4.0}).value() == 0.25);
    STATIC_REQUIRE(qp::units::pow<4>(qp::units::Length{10.0}).value() == 10000.0);

    // 非精确值：**不能**用 STATIC_REQUIRE 比对，即使两侧写法在运行期逐位相同。
    // 实测：pow<-2>(10) 与 0.01 在运行期同为 0x3f847ae147ae147b，
    //       但 static_assert 在 80 位下比较会失败。这是本文件存在的原因。
    REQUIRE(qp::units::pow<-2>(qp::units::Length{10.0}).value() == 0.01);
    REQUIRE(qp::units::pow<-2>(qp::units::Length{10.0}).value() == 1.0 / 100.0);

    // 同一求值环境（都是运行期）下平方求幂与朴素连乘必须逐位相同
    constexpr double k = 1.1;
    REQUIRE(qp::units::pow<2>(qp::units::Length{k}).value() == k * k);
}
