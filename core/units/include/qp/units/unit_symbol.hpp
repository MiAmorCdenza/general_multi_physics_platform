/**
 * @file unit_symbol.hpp
 * @brief 由**编译期量纲类型**自动生成单位字符串。
 *
 * 这是本项目"一份真相源"的关键机制：
 *   C++ 里的单位字符串、YAML 里的 `unit:` 字段、脚本里的单位名，
 *   三者全部由同一个 `Dim` 推导而来，不存在第二份单位表。
 *
 * 因此：**不允许手写单位字符串的字面量**，除非它来自本文件的函数。
 */
#pragma once

#include <qp/units/dim.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace qp::units {

/// @brief 单位符号风格。
enum class SymbolStyle {
    short_form,  ///< "kg*m/s^2"
    long_form,   ///< "kilogram*meter/second^2"
};

namespace detail {

/**
 * 单位名顺序 —— **qp 约定，非 ISO 80000 逐字复刻**。
 *
 * 规则：分子按符号字母升序，分母同理。结果：
 *   kg < m < s  →  力 = "kg*m/s^2"
 *   A < s       →  磁感应强度 = "kg/(A*s^2)"、电压 = "kg*m^2/(A*s^3)"
 *
 * 为什么是"约定"而不是"标准"：
 *   现行规范（BIPM SI 手册、NIST SP 811）对**导出单位用基本单位表达时的书写
 *   顺序没有强制规定**，只要求同一文本内自洽。因此本项目的做法是选定一种
 *   确定性顺序并写进 ABI 版本，而不是声称遵循某条并不存在的强制条款。
 *
 * 这不影响任何物理正确性：`unit_symbol` 只用于显示与互换；
 * 真正的量纲信息在 `Dim` 类型里，与字符串无关。
 *
 * 未来若需要严格照抄某个标准符号（如 NIST 的 "kg·m²/(A·s³)"），
 * 应新增符号表并升 `kUnitsAbiVersion`，而不是偷偷改顺序。
 */
inline constexpr std::array<std::string_view, 7> kShortNumerator{
    "A", "K", "cd", "kg", "m", "mol", "s"};
inline constexpr std::array<std::string_view, 7> kLongNumerator{
    "ampere", "kelvin", "candela", "kilogram", "meter", "mole", "second"};

/// 把指数追加为 "^n"（n==1 时省略）。
inline void append_exp(std::string& out, DimExp e) {
    if (e == 1) return;
    out += '^';
    out += std::to_string(static_cast<int>(e));
}

/// 单位符号按字母序排列时的轴索引：A K cd kg m mol s
/// → 对应 Dim 的成员 I, Th, J, M, L, N, T
inline constexpr std::array<int, 7> kSymbolAxisOrder{3, 4, 6, 1, 0, 5, 2};

// 以下三个是**实现细节**（namespace detail），不构成模块契约面，
// 因而不单独写契约、不单独点名测试。它们的行为通过公开的 unit_symbol()
// 在黄金测试与性质测试中被完整覆盖——这是"测试可观察行为，而非实现细节"。
namespace detail {

/// 取出按符号序排列的七个指数。
[[nodiscard]] inline std::array<DimExp, 7> symbol_order_exponents(Dim d) noexcept {
    const std::array<DimExp, 7> by_member{d.L, d.M, d.T, d.I, d.Th, d.N, d.J};
    std::array<DimExp, 7> out{};
    for (std::size_t i = 0; i < 7; ++i) {
        out[i] = by_member[static_cast<std::size_t>(kSymbolAxisOrder[i])];
    }
    return out;
}

/// 某一侧（分子 / 分母）实际会打印出几个因子。
/// 分母侧数的是**负指数**：压强 kg/(m*s^2) 的分母是 m 与 s^2，共 2 个。
/// 早期版本把"取负之后为正"当作判据，等于把分子也数了进去——由黄金测试抓出。
[[nodiscard]] inline int count_factors(Dim d, bool denominator) noexcept {
    int n = 0;
    for (DimExp e : symbol_order_exponents(d)) {
        const DimExp shown = denominator ? static_cast<DimExp>(-e) : e;
        if (shown > 0) ++n;
    }
    return n;
}

/// 分母侧是否需要括号：含多个因子时必须有括号，否则语义有歧义。
[[nodiscard]] inline bool needs_parentheses(Dim d) noexcept {
    return count_factors(d, true) > 1;
}

}  // namespace detail

/**
 * @brief 生成一侧（分子或分母）的字符串。符号取绝对值，方向由 `per` 表达。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        向 out 追加该侧的符号串；无因子时追加 "1"
 * @invariant   分母多因子时自动加括号（与 count_factors 一致）
 * @errors      noexcept；分配失败即 std::terminate
 * @complexity  O(7)
 * @nondet      none
 * @frozen      否
 * @tests       units.symbol.short_forms, units.symbol.long_form,
 *              units.symbol.denominator_parenthesized
 */
template <std::size_t N>
inline void append_side(std::string& out, const std::array<std::string_view, N>& names, Dim d,
                        bool denominator) noexcept {
    const auto exps = detail::symbol_order_exponents(d);
    const bool parenthesize = denominator && detail::needs_parentheses(d);    if (parenthesize) out += '(';

    bool first = true;
    for (std::size_t i = 0; i < 7; ++i) {
        const DimExp shown = denominator ? static_cast<DimExp>(-exps[i]) : exps[i];
        if (shown <= 0) continue;
        if (!first) out += '*';
        out += names[i];
        append_exp(out, shown);
        first = false;
    }
    if (first) out += '1';  // 例如频率的分子侧
    if (parenthesize) out += ')';
}

/// 判断分母侧是否为空（全部指数 <= 0）。
inline constexpr bool has_denominator(Dim d) noexcept {
    return d.L < 0 || d.M < 0 || d.T < 0 || d.I < 0 || d.Th < 0 || d.N < 0 || d.J < 0;
}

}  // namespace detail

/**
 * @brief 生成量纲 D 的单位字符串。
 *
 * @ownership   pure
 * @thread      any
 * @pre         D 可表示（is_representable）
 * @post        返回非空字符串；无量纲返回 "1"
 * @invariant   同一个 D 永远生成同一个字符串（幂等且确定）
 * @errors      noexcept；分配失败即 std::terminate（与全库一致：不抛异常）
 * @complexity  O(7)
 * @nondet      none
 * @frozen      否（字符串风格可扩），但 "同一 D → 同一字符串" 这一不变量冻结
 * @tests       units.symbol.short_forms, units.symbol.dimensionless_is_one,
 *              units.symbol.negative_exponent_uses_per, units.symbol.area_uses_caret,
 *              units.symbol.denominator_parenthesized,
 *              units.symbol.long_form, units.symbol.deterministic
 */
[[nodiscard]] inline std::string unit_symbol(Dim d,
                                             SymbolStyle style = SymbolStyle::short_form) noexcept {
    std::string out;
    if (style == SymbolStyle::short_form) {
        detail::append_side(out, detail::kShortNumerator, d, false);
        if (detail::has_denominator(d)) {
            out += '/';
            detail::append_side(out, detail::kShortNumerator, d, true);
        }
    } else {
        detail::append_side(out, detail::kLongNumerator, d, false);
        if (detail::has_denominator(d)) {
            out += " per ";
            detail::append_side(out, detail::kLongNumerator, d, true);
        }
    }
    if (out.empty()) out = "1";
    return out;
}

/**
 * @brief 编译期量纲 → 单位字符串（短式）。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        等价于 unit_symbol(D, short_form)
 * @invariant   见 unit_symbol
 * @errors      noexcept；分配失败即 std::terminate
 * @complexity  O(1)（首次调用后）
 * @nondet      none
 * @frozen      否
 * @tests       units.symbol.compile_time_matches_runtime
 */
template <Dim D>
[[nodiscard]] inline std::string unit_symbol() noexcept {
    return unit_symbol(D);
}

/**
 * @brief 七指数逗号序列，用于诊断信息与黄金回归的诊断点打印。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回 "L,M,T,I,Th,N,J" 形式的字符串
 * @invariant   与 unit_symbol 一样确定：同输入同输出
 * @errors      noexcept；分配失败即 std::terminate
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否（诊断格式不属于 ABI）
 * @tests       units.dim_axes.diagnostic_format
 */
[[nodiscard]] inline std::string dim_axes(Dim d) noexcept {
    return std::to_string(d.L) + "," + std::to_string(d.M) + "," + std::to_string(d.T) + "," +
           std::to_string(d.I) + "," + std::to_string(d.Th) + "," + std::to_string(d.N) + "," +
           std::to_string(d.J);
}

/**
 * @brief 运行期单位：量纲 + 换算到 SI 基本单位的因子。
 *
 * 用途：端口上标注单位、YAML 里写 `unit: cm`、仪器读数带单位显示。
 *
 * @ownership   pure（值类型）
 * @thread      any
 * @pre         factor != 0
 * @post        to_si(v) == v * factor
 * @invariant   symbol 与 dim 一致（由构造方保证，不在此处校验）
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      是——这是 ABI 侧使用的类型
 * @tests       units.unit.convert_to_si, units.unit.roundtrip
 */
struct Unit final {
    Dim dim{};
    double factor = 1.0;
    std::string symbol{};

    /**
     * @brief 把以本单位的数值换算为 SI 基本单位数值。
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        返回 v * factor
     * @invariant   from_si(to_si(v)) == v（在浮点可表示范围内）
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      是
     * @tests       units.unit.convert_to_si, units.unit.roundtrip
     */
    [[nodiscard]] double to_si(double v) const noexcept { return v * factor; }

    /**
     * @brief 把 SI 基本单位数值换算为本单位数值。
     *
     * @ownership   pure
     * @thread      any
     * @pre         factor != 0
     * @post        返回 v / factor
     * @invariant   to_si(from_si(v)) == v（在浮点可表示范围内）
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      是
     * @tests       units.unit.roundtrip
     */
    [[nodiscard]] double from_si(double v) const noexcept { return v / factor; }
};

}  // namespace qp::units
