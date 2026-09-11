/**
 * @file field_dim.hpp
 * @brief The dimension encoding the ABI carries: **deliberately a duplicate of `qp::units::Dim`**.
 *
 * An intentional trade-off, written down here so nobody "helpfully deduplicates" it later
 *
 * ## Why not simply include `qp/units/dim.hpp`
 *
 * `core/abi` is the boundary contract shown to **external language bindings** (Python /
 * MATLAB / a future Web target). Those do not compile C++; they read one layout spec.
 * If an abi header needed a units header, "what the ABI is" would hide inside a C++
 *
 * Hence the `abi` rule: **use only the C subset** (POD, fixed-width integers, no
 * templates, no STL). That lets the ABI be copied byte for byte into one language-neutral .h.
 *
 * ## Will the duplicate definitions drift apart
 *
 * No ---- `tests/abi/test_abi_layout.cpp` carries an assertion:
 * `qp::abi::FieldDim` and `qp::units::Dim` must agree on sizeof and on the
 * semantics of all seven exponents. Drift surfaces at compile time. **Guard the
 *
 * @frozen yes ---- this structure's layout is an ABI contract.
 */
#pragma once

#include <cstdint>

namespace qp::abi {

/// @brief Dimension exponent. Same as `qp::units::DimExp` (int8).
using DimExp = std::int8_t;

/// @brief The ABI representation of a dimension: integer exponents over the seven SI
///
/// Member order and semantics must match `qp::units::Dim`: L, M, T, I, Th, N, J
/// (corresponding to m, kg, s, A, K, mol, cd).
///
/// A struct rather than `int8_t[7]`: an array cannot carry a contract and cannot be
/// addressed by member name, and external bindings read **member names and offsets**.
struct FieldDim final {
    DimExp L = 0;
    DimExp M = 0;
    DimExp T = 0;
    DimExp I = 0;
    DimExp Th = 0;
    DimExp N = 0;
    DimExp J = 0;
};

/// @brief The meaning of the seven exponents (for external bindings; not part of the layout).
enum class DimAxis : std::uint8_t {
    length = 0,        ///< metre
    mass = 1,          ///< kilogram
    time = 2,          ///< second
    current = 3,       ///< ampere
    temperature = 4,   ///< kelvin
    amount = 5,        ///< mole
    luminous = 6,      ///< candela
};

/// @brief Dimensionless (all zeros).
inline constexpr FieldDim kDimensionless{};

}  // namespace qp::abi
