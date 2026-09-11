/**
 * @file unit_symbol.hpp
 * @brief Generates unit strings from the **compile-time dimension type**.
 *
 * This is the project's key "single source of truth" mechanism:
 *   the unit strings in C++, the `unit:` field in YAML, the unit names in scripts --
 *   all three are derived from one `Dim`, and no second unit table exists.
 *
 * Hence: **unit string literals must never be written by hand**, unless this file made them.
 */
#pragma once

#include <qp/units/dim.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace qp::units {

/// @brief Unit symbol style.
enum class SymbolStyle {
    short_form,  ///< "kg*m/s^2"
    long_form,   ///< "kilogram*meter/second^2"
};

namespace detail {

/**
 * Unit name order -- a **qp convention, not a verbatim copy of ISO 80000**.
 *
 * Rule: the numerator sorts symbols by ascending letter, and so does the denominator. Result:
 *   kg < m < s  ->  force = "kg*m/s^2"
 *   A < s       ->  magnetic flux density = "kg/(A*s^2)", voltage = "kg*m^2/(A*s^3)"
 *
 * Why a "convention" and not a "standard":
 *   The current specifications (BIPM SI brochure, NIST SP 811) impose **no mandatory order
 *   for writing derived units in base units**, only self-consistency within one text. So this
 *   project fixes one deterministic order in the ABI version, rather than citing a nonexistent clause.
 *
 * This affects no physical correctness: `unit_symbol` serves display and interchange only;
 * the real dimension information lives in the `Dim` type and is independent of the string.
 *
 * If a standard symbol must ever be copied verbatim (say NIST's "kg·m²/(A·s³)"),
 * add a new symbol table and bump `kUnitsAbiVersion`, rather than quietly changing the order.
 */
inline constexpr std::array<std::string_view, 7> kShortNumerator{
    "A", "K", "cd", "kg", "m", "mol", "s"};
inline constexpr std::array<std::string_view, 7> kLongNumerator{
    "ampere", "kelvin", "candela", "kilogram", "meter", "mole", "second"};

/// Appends the exponent as "^n" (omitted when n == 1).
inline void append_exp(std::string& out, DimExp e) {
    if (e == 1) return;
    out += '^';
    out += std::to_string(static_cast<int>(e));
}

/// Axis index when unit symbols are ordered alphabetically: A K cd kg m mol s
/// -> the Dim members I, Th, J, M, L, N, T
inline constexpr std::array<int, 7> kSymbolAxisOrder{3, 4, 6, 1, 0, 5, 2};

// The next three are **implementation details** (namespace detail) and form no module contract
// surface, so they carry no contract block and no named tests; the public unit_symbol() covers
// them fully in the golden and property tests -- "test observable behavior, not implementation".
namespace detail {

/// Extracts the seven exponents in symbol order.
[[nodiscard]] inline std::array<DimExp, 7> symbol_order_exponents(Dim d) noexcept {
    const std::array<DimExp, 7> by_member{d.L, d.M, d.T, d.I, d.Th, d.N, d.J};
    std::array<DimExp, 7> out{};
    for (std::size_t i = 0; i < 7; ++i) {
        out[i] = by_member[static_cast<std::size_t>(kSymbolAxisOrder[i])];
    }
    return out;
}

/// How many factors one side (numerator / denominator) will actually print.
/// The denominator counts **negative exponents**: for pressure kg/(m*s^2) that is m and s^2, 2 in all.
/// An earlier version tested "positive after negation" and counted the numerator too -- caught by a golden test.
[[nodiscard]] inline int count_factors(Dim d, bool denominator) noexcept {
    int n = 0;
    for (DimExp e : symbol_order_exponents(d)) {
        const DimExp shown = denominator ? static_cast<DimExp>(-e) : e;
        if (shown > 0) ++n;
    }
    return n;
}

/// Whether the denominator needs parentheses: more than one factor requires them, or the meaning is ambiguous.
[[nodiscard]] inline bool needs_parentheses(Dim d) noexcept {
    return count_factors(d, true) > 1;
}

}  // namespace detail

/**
 * @brief Builds the string for one side (numerator or denominator). Symbols are absolute; `per` carries direction.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Appends that side's symbol string to out; appends "1" when there is no factor
 * @invariant   A multi-factor denominator is parenthesized automatically (consistent with count_factors)
 * @errors      noexcept; allocation failure calls std::terminate
 * @complexity  O(7)
 * @nondet      none
 * @frozen      no
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
    if (first) out += '1';  // e.g. the numerator side of a frequency
    if (parenthesize) out += ')';
}

/// Whether the denominator side is empty (every exponent <= 0).
inline constexpr bool has_denominator(Dim d) noexcept {
    return d.L < 0 || d.M < 0 || d.T < 0 || d.I < 0 || d.Th < 0 || d.N < 0 || d.J < 0;
}

}  // namespace detail

/**
 * @brief Builds the unit string of dimension D.
 *
 * @ownership   pure
 * @thread      any
 * @pre         D is representable (is_representable)
 * @post        Returns a non-empty string; a dimensionless D returns "1"
 * @invariant   One D always yields one string (idempotent and deterministic)
 * @errors      noexcept; allocation failure calls std::terminate (library-wide: nothing throws)
 * @complexity  O(7)
 * @nondet      none
 * @frozen      no (the string style may grow), but "same D -> same string" is frozen
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
 * @brief Compile-time dimension -> unit string (short form).
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Equivalent to unit_symbol(D, short_form)
 * @invariant   See unit_symbol
 * @errors      noexcept; allocation failure calls std::terminate
 * @complexity  O(1) (after the first call)
 * @nondet      none
 * @frozen      no
 * @tests       units.symbol.compile_time_matches_runtime
 */
template <Dim D>
[[nodiscard]] inline std::string unit_symbol() noexcept {
    return unit_symbol(D);
}

/**
 * @brief The seven exponents as a comma sequence, for diagnostics and golden-regression print points.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a string of the form "L,M,T,I,Th,N,J"
 * @invariant   As deterministic as unit_symbol: same inputs, same output
 * @errors      noexcept; allocation failure calls std::terminate
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no (the diagnostic format is not part of the ABI)
 * @tests       units.dim_axes.diagnostic_format
 */
[[nodiscard]] inline std::string dim_axes(Dim d) noexcept {
    return std::to_string(d.L) + "," + std::to_string(d.M) + "," + std::to_string(d.T) + "," +
           std::to_string(d.I) + "," + std::to_string(d.Th) + "," + std::to_string(d.N) + "," +
           std::to_string(d.J);
}

/**
 * @brief A runtime unit: a dimension plus the factor converting to SI base units.
 *
 * Uses: unit labels on ports, `unit: cm` in YAML, instrument readings shown with their unit.
 *
 * @ownership   pure (a value type)
 * @thread      any
 * @pre         factor != 0
 * @post        to_si(v) == v * factor
 * @invariant   symbol agrees with dim (guaranteed by the constructor, not checked here)
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      yes -- this is the type the ABI side uses
 * @tests       units.unit.convert_to_si, units.unit.roundtrip
 */
struct Unit final {
    Dim dim{};
    double factor = 1.0;
    std::string symbol{};

    /**
     * @brief Converts a value in this unit into an SI base-unit value.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns v * factor
     * @invariant   from_si(to_si(v)) == v (within the floating-point representable range)
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      yes
     * @tests       units.unit.convert_to_si, units.unit.roundtrip
     */
    [[nodiscard]] double to_si(double v) const noexcept { return v * factor; }

    /**
     * @brief Converts an SI base-unit value into a value in this unit.
     *
     * @ownership   pure
     * @thread      any
     * @pre         factor != 0
     * @post        Returns v / factor
     * @invariant   to_si(from_si(v)) == v (within the floating-point representable range)
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      yes
     * @tests       units.unit.roundtrip
     */
    [[nodiscard]] double from_si(double v) const noexcept { return v / factor; }
};

}  // namespace qp::units
