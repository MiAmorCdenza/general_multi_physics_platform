/**
 * @file lattice.hpp
 * @brief ABI description of the sampling lattice: what the field data looks like.
 *
 * This is the **metadata** of one field, not the data itself. The data lives in `FieldBuffer`.
 * Why the two are separate: metadata is tiny (copy it freely), data is huge (it must stay zero-copy).
 *
 * @frozen yes -- the layout is an ABI contract.
 */
#pragma once

#include <qp/abi/field_dim.hpp>

#include <cstdint>

namespace qp::abi {

/// @brief Lattice kind. Determines how `data_bytes` is computed.
enum class LatticeKind : std::uint8_t {
    /// Not on a lattice: a single value (point measurement, scalar parameter).
    point = 0,
    /// 1-D array: `count[0]` sample points.
    line = 1,
    /// 2-D grid: `count[0] x count[1]`.
    plane = 2,
    /// 3-D grid: `count[0] x count[1] x count[2]`.
    volume = 3,
};

/// @brief Component kind of the field.
enum class ComponentKind : std::uint8_t {
    scalar = 0,   ///< 1 float32 per point
    vector = 1,   ///< 3 float32 per point (x, y, z)
};

/// @brief Scalar type of a data element.
enum class ElementType : std::uint8_t {
    f32 = 0,   ///< 4-byte float. The default precision for field data (see ADR-0005).
    f64 = 1,   ///< 8-byte float. Used by the scalar measurement chain.
};

/**
 * @brief Description of the sampling lattice.
 *
 * Layout (little endian, all integers fixed width):
 * ```
 * offset  length  field
 *    0     7  dimension   (7 int8 values)
 *    7     1  component
 *    8     1  element
 *    9     1  kind
 *   10     1  padding
 *   12     4  count[0]
 *   16     4  count[1]
 *   20     4  count[2]
 *   24     4  spacing_bytes   (stride per sample point = component count x element size)
 *   28     4  _reserved
 * 32 bytes total, alignof == 4
 * ```
 *
 * @ownership   pure (POD, copy freely)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   the same lattice description serialises to the same byte string every time
 * @errors      noexcept
 * @frozen      yes
 * @tests       abi.lattice.size_and_alignment, abi.lattice.field_offsets,
 *              abi.lattice.point_count, abi.lattice.data_bytes,
 *              abi.lattice.trivially_copyable, abi.lattice.default_is_point_scalar
 */
struct LatticeDesc final {
    FieldDim dimension{};                                  ///< Dimension of the physical quantity
    ComponentKind component = ComponentKind::scalar;       ///< Scalar or vector
    ElementType element = ElementType::f32;                ///< Element precision
    LatticeKind kind = LatticeKind::point;                 ///< Geometric shape
    std::uint8_t padding = 0;                              ///< Explicit padding; keeps later fields 4-byte aligned

    std::uint32_t count[3] = {0, 0, 0};                    ///< Sample count per dimension
    std::uint32_t spacing_bytes = 0;                       ///< Bytes occupied by one sample point
    std::uint32_t reserved = 0;                            ///< Reserved; must be 0
};

/**
 * @brief Total number of sample points.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        point / line / plane / volume return 1 / c0 / c0*c1 / c0*c1*c2 respectively
 * @invariant   the point kind always returns 1 (even when every count is 0)
 * @errors      noexcept; on overflow the 64-bit result is truncated -- the caller must check data_bytes first
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.lattice.point_count
 */
[[nodiscard]] constexpr std::uint64_t point_count(const LatticeDesc& d) noexcept {
    switch (d.kind) {
        case LatticeKind::point:  return 1;
        case LatticeKind::line:   return d.count[0];
        case LatticeKind::plane:  return static_cast<std::uint64_t>(d.count[0]) * d.count[1];
        case LatticeKind::volume:
            return static_cast<std::uint64_t>(d.count[0]) * d.count[1] * d.count[2];
    }
    return 0;
}

/// @brief How many floats one sample point holds (1 for scalar, 3 for vector).
[[nodiscard]] constexpr std::uint32_t component_count(ComponentKind c) noexcept {
    return c == ComponentKind::vector ? 3U : 1U;
}

/// @brief Size of one element in bytes.
[[nodiscard]] constexpr std::uint32_t element_size(ElementType e) noexcept {
    return e == ElementType::f64 ? 8U : 4U;
}

/**
 * @brief Number of data bytes this lattice requires.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        equals point_count x component_count x element_size
 * @invariant   matches LatticeDesc::spacing_bytes x point_count (when spacing was filled in correctly)
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.lattice.data_bytes
 */
[[nodiscard]] constexpr std::uint64_t data_bytes(const LatticeDesc& d) noexcept {
    return point_count(d) * component_count(d.component) * element_size(d.element);
}

/// @brief The per-point stride this description should have.
[[nodiscard]] constexpr std::uint32_t expected_spacing(const LatticeDesc& d) noexcept {
    return component_count(d.component) * element_size(d.element);
}

/**
 * @brief Whether the description is self-consistent. The host must call this before adopting an outside lattice.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        checks that reserved is 0, spacing matches component/precision, and dimension counts are legal
 * @invariant   a self-consistent description cannot cause an out-of-bounds read
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       abi.lattice.consistency_check
 */
[[nodiscard]] constexpr bool is_consistent(const LatticeDesc& d) noexcept {
    if (d.reserved != 0) return false;
    if (d.padding != 0) return false;
    if (d.spacing_bytes != expected_spacing(d)) return false;
    switch (d.kind) {
        case LatticeKind::point:
            break;
        case LatticeKind::line:
            if (d.count[0] == 0) return false;
            break;
        case LatticeKind::plane:
            if (d.count[0] == 0 || d.count[1] == 0) return false;
            break;
        case LatticeKind::volume:
            if (d.count[0] == 0 || d.count[1] == 0 || d.count[2] == 0) return false;
            break;
        default:
            return false;
    }
    return true;
}

/// @brief Build a self-consistent lattice description (fills in spacing automatically).
[[nodiscard]] constexpr LatticeDesc make_lattice(LatticeKind kind, ComponentKind component,
                                                 ElementType element, FieldDim dim,
                                                 std::uint32_t n0 = 0, std::uint32_t n1 = 0,
                                                 std::uint32_t n2 = 0) noexcept {
    LatticeDesc d{};
    d.dimension = dim;
    d.component = component;
    d.element = element;
    d.kind = kind;
    d.padding = 0;
    d.count[0] = n0;
    d.count[1] = n1;
    d.count[2] = n2;
    d.spacing_bytes = component_count(component) * element_size(element);
    d.reserved = 0;
    return d;
}

}  // namespace qp::abi
