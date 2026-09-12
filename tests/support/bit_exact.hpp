/**
 * @file bit_exact.hpp
 * @brief Turning floating-point state into a string that changes when **one bit** changes.
 *
 * ## Why this exists rather than a tolerance comparison
 *
 * Charter C7 requires every numerical kernel to have a fixed-input, **bit-for-bit** comparison
 * case. The plugin tests written so far assert against closed-form solutions, which is a
 * different and also necessary thing: it says the physics is right. It does not say the
 * arithmetic is *unchanged*, and for an integrator those are separate questions. A reordering of
 * the RK4 stages, a `x*0.5` changed to `x/2`, an expression reassociated by a compiler flag --
 * every one of those leaves the closed-form assertions passing to within their tolerances while
 * producing different bits. The bits are what a saved run contains, and a run that reproduces to
 * four digits is not the run that was recorded.
 *
 * ## Why `%a` and not `%.17g`
 *
 * `%.17g` is enough to **round-trip** a double, so it looks sufficient. It is not sufficient for
 * a comparison, because it is a *decimal* rendering of a binary value: two implementations can
 * agree on every significant digit they print and still differ in the last bit, and 17 digits is
 * chosen to make round-tripping work rather than to expose the low bits. `%a` is exact -- it
 * prints the binary significand as hexadecimal -- so an equal string means an equal double, and
 * an unequal string means an unequal double.
 *
 * This is the same reasoning `core/graph/eval` reached for its content-addressed cache key: the
 * platform already normalises `%a` for hashing, because a hash of a decimal approximation is a
 * hash of something the run did not compute.
 *
 * ## Why a checksum as well as the values
 *
 * A golden case that stores every doubles' bit pattern is a file that has to be rewritten
 * whenever the case's step count changes, which makes it a file people edit without reading. The
 * checksum is one number that covers the whole state, so a regression shows up as a single
 * mismatch with the differing component named beside it.
 *
 * The checksum is **order-sensitive** by construction: it accumulates `sum = sum * 31 + byte`
 * over the state's bytes in index order, so swapping two particles changes it. A commutative
 * combination would be worthless here -- a kernel that iterated its batch backwards would produce
 * the same checksum and a different state.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The output is a pure function of the input bits
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.golden.oscillator_bits, plugin.golden.incline_bits,
 *              plugin.golden.merge_bits
 */
#pragma once

#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace qp::test::golden {

/**
 * @brief The exact bit pattern of `v`, as 16 lowercase hex digits.
 *
 * Via `std::bit_cast` rather than a union or a `memcpy`, so the conversion is defined behaviour
 * and works in a constant expression. A union is the version that appears to work and is
 * undefined; `memcpy` is correct and cannot be used at compile time.
 *
 * @ownership   owns the returned string
 * @thread      any
 * @pre         none
 * @post        Exactly 16 characters, `[0-9a-f]`
 * @invariant   `bits_of(a) == bits_of(b)` exactly when `a` and `b` have identical bits, so
 *              `-0.0` and `0.0` differ and two NaNs with different payloads differ
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       plugin.golden.oscillator_bits
 */
[[nodiscard]] inline std::string bits_of(double v) noexcept {
    const std::uint64_t raw = std::bit_cast<std::uint64_t>(v);
    char buffer[17] = {};
    for (int i = 15; i >= 0; --i) {
        const auto nibble = static_cast<unsigned>((raw >> (4U * static_cast<unsigned>(15 - i))) & 0xFU);
        buffer[i] = static_cast<char>(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U)));
    }
    return std::string{buffer, 16};
}

/**
 * @brief An order-sensitive checksum of a state vector's exact bits.
 *
 * Exposed so a golden case can assert one number for a whole batch and print the per-component
 * patterns only when it fails -- a mismatch then names which component moved, instead of leaving
 * the reader to diff two lists of sixteen-character strings.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Zero for an empty vector
 * @invariant   Depends on the order of `values`: a permutation changes the result
 * @errors      noexcept
 * @complexity  O(values.size())
 * @nondet      none
 * @frozen      no
 * @tests       plugin.golden.oscillator_bits
 */
[[nodiscard]] inline std::uint64_t checksum(const std::vector<double>& values) noexcept {
    std::uint64_t acc = 1469598103934665603ULL;  // FNV-1a offset basis, so an empty vector is not 0
    for (const double v : values) {
        const std::uint64_t raw = std::bit_cast<std::uint64_t>(v);
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            acc ^= (raw >> shift) & 0xFFU;
            acc *= 1099511628211ULL;  // FNV-1a prime
        }
    }
    return acc;
}

/// @brief The checksum as 16 hex digits, for pasting into a golden expectation.
///
/// @ownership   owns the returned string
/// @thread      any
/// @pre         none
/// @post        Exactly 16 characters, `[0-9a-f]`
/// @invariant   A pure function of `checksum(values)`
/// @errors      noexcept
/// @complexity  O(values.size())
/// @nondet      none
/// @frozen      no
/// @tests       plugin.golden.oscillator_bits
[[nodiscard]] inline std::string checksum_hex(const std::vector<double>& values) noexcept {
    const std::uint64_t acc = checksum(values);
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[(acc >> (4U * static_cast<unsigned>(15 - i))) & 0xFU];
    }
    return out;
}

}  // namespace qp::test::golden
