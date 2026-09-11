/**
 * @file field.hpp
 * @brief The minimal semantics of "a field": is this one, what is its shape, which components exist.
 *
 * ## What belongs here, and what does not
 *
 * This module answers exactly three questions about a piece of data:
 *
 *   1. **Is this a field?** -- a lattice description that is self-consistent and
 *      a byte span that actually covers it.
 *   2. **What shape is it?** -- point / line / plane / volume plus the counts.
 *   3. **Which components exist?** -- scalar (one value per point) or vector
 *      (three), and whether reading a given component is legal.
 *
 * Everything specific to a physical model is a **plugin**: dipoles, magnetotail
 * configurations, T04 models, envelope tracking. None of them belong in the
 * foundation, and all of them can be written against these three questions.
 *
 * ## Why this is not part of abi
 *
 * `core/abi` describes what the bytes look like so that a foreign-language
 * binding can produce and consume them. That is a question about memory layout.
 * This module decides whether a particular buffer *is usable as a field*, which
 * is a question about meaning. Keeping them apart is what allows the ABI header
 * to stay free of `units`, of heap allocation, and of any opinion about physics.
 *
 * ## Why there is no unit here
 *
 * A field carries a `FieldDim`: the exponents of its physical dimension. It does
 * **not** carry a `units::Quantity`, and it does **not** format a unit string.
 * Two reasons:
 *
 *   - field data is float32 and lives on a hot path; converting every sample to
 *     a checked `Quantity` would be a per-sample cost for no information.
 *   - formatting belongs to display. A renderer that needs `kg*m/s^2` asks
 *     `units::unit_symbol(dim)` at the moment it draws, once, not per sample.
 *
 * @ownership   pure (a view over a buffer it does not own)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A FieldValue never owns its data and never outlives the buffer it points at
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       field.layout.point_counts, field.layout.byte_span,
 *              field.value_is_valid, field.value_is_valid_f64_alignment,
 *              field.component_access
 */
#pragma once

#include <qp/abi/lattice.hpp>

#include <cstdint>

namespace qp::graph::field {

/**
 * @brief The geometric shape of a sampled field.
 *
 * A separate enum from `qp::abi::LatticeKind`, with the same values, because the two
 * answer different questions and change for different reasons: `LatticeKind` is
 * frozen ABI vocabulary that a C binding must spell correctly, while this is the
 * name the platform uses in its own code and diagnostics. Renaming `Plane` here
 * is a source-compatible change; renaming the ABI enumerator is not.
 *
 * The mapping is total and checked by `abi.lattice.size_and_alignment`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Values match the corresponding qp::abi::LatticeKind values
 * @errors      noexcept
 * @frozen      no
 * @tests       field.kind_matches_abi
 */
enum class Kind : std::uint8_t {
    /// Not on a lattice: a single value (a scalar parameter, a point measurement).
    Point = 0,
    /// 1-D array of samples.
    Line = 1,
    /// 2-D grid of samples.
    Plane = 2,
    /// 3-D grid of samples.
    Volume = 3,
};

/**
 * @brief Which components of a sample are valid.
 *
 * The one piece of physics this module is allowed to know: a scalar field has
 * one value per point, a vector field has three, and a caller asking for the
 * fourth component of a 2-component field must be told no rather than handed
 * whatever happens to sit next in memory.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   component_count() equals qp::abi::component_count of the matching abi kind
 * @errors      noexcept
 * @frozen      no
 * @tests       field.components.valid_range, field.components.count,
 *              field.components.rejects_out_of_range
 */
enum class Components : std::uint8_t {
    /// One value per point.
    Scalar = 1,
    /// Three values per point, in x, y, z order.
    Vector = 3,
};

/// @brief How many components one sample point holds (1 or 3).
[[nodiscard]] constexpr std::uint32_t component_count(Components c) noexcept {
    return static_cast<std::uint32_t>(c);
}

/// @brief The component set implied by an ABI component kind.
[[nodiscard]] constexpr Components components_of(qp::abi::ComponentKind c) noexcept {
    return c == qp::abi::ComponentKind::vector ? Components::Vector : Components::Scalar;
}

/// @brief The ABI enumerator for a component set.
[[nodiscard]] constexpr qp::abi::ComponentKind abi_component(Components c) noexcept {
    return c == Components::Vector ? qp::abi::ComponentKind::vector : qp::abi::ComponentKind::scalar;
}

/// @brief True when `index` names a component this set actually has.
[[nodiscard]] constexpr bool has_component(Components c, std::uint32_t index) noexcept {
    return index < component_count(c);
}

/// @brief The lattice kind implied by an ABI lattice kind.
[[nodiscard]] constexpr Kind kind_of(qp::abi::LatticeKind k) noexcept {
    switch (k) {
        case qp::abi::LatticeKind::point:  return Kind::Point;
        case qp::abi::LatticeKind::line:   return Kind::Line;
        case qp::abi::LatticeKind::plane:  return Kind::Plane;
        case qp::abi::LatticeKind::volume: return Kind::Volume;
    }
    return Kind::Point;
}

/// @brief The ABI enumerator for a kind.
[[nodiscard]] constexpr qp::abi::LatticeKind abi_kind(Kind k) noexcept {
    switch (k) {
        case Kind::Point:  return qp::abi::LatticeKind::point;
        case Kind::Line:   return qp::abi::LatticeKind::line;
        case Kind::Plane:  return qp::abi::LatticeKind::plane;
        case Kind::Volume: return qp::abi::LatticeKind::volume;
    }
    return qp::abi::LatticeKind::point;
}

/**
 * @brief A field: a lattice description plus a non-owning pointer to its samples.
 *
 * The pointer is **not** owned and **not** lifetime-managed. That is deliberate:
 * field data is produced by one node and consumed by the next, both inside a
 * single evaluation, and giving the pointer an owner would mean either a copy of
 * megabytes per sample or a reference-counted buffer on a path where allocation
 * is forbidden. The frame in which a FieldValue is valid is the frame that
 * produced it; carrying one beyond that is a contract violation of the caller.
 *
 * @ownership   observes (never owns `data`)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `data` is either null or points at `bytes` readable bytes
 * @errors      noexcept
 * @frozen      no
 * @tests       field.value_bytes, field.value_points,
 *              field.value_is_trivially_copyable
 */
struct FieldValue final {
    qp::abi::LatticeDesc desc{};      ///< Shape, dimension and element type
    const void* data = nullptr;   ///< Non-owning pointer to the first sample
    std::uint64_t bytes = 0;      ///< Bytes actually available at `data`

    /**
     * @brief Whether this is a scalar field (exactly one component per point).
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Equivalent to desc.component == qp::abi::ComponentKind::scalar
     * @invariant   Never true at the same time as is_vector()
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       field.value_points
     */
    [[nodiscard]] constexpr bool is_scalar() const noexcept {
        return desc.component == qp::abi::ComponentKind::scalar;
    }

    /**
     * @brief Whether this is a vector field (three components per point).
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Equivalent to desc.component == qp::abi::ComponentKind::vector
     * @invariant   Never true at the same time as is_scalar()
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       field.component_access
     */
    [[nodiscard]] constexpr bool is_vector() const noexcept {
        return desc.component == qp::abi::ComponentKind::vector;
    }

    /**
     * @brief The geometric shape.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns the Kind matching desc.kind
     * @invariant   abi_kind(kind()) == desc.kind for every description
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       field.kind_matches_abi
     */
    [[nodiscard]] constexpr Kind kind() const noexcept { return kind_of(desc.kind); }

    /**
     * @brief The components each point holds.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns the Components matching desc.component
     * @invariant   component_count(components()) == qp::abi::component_count(desc.component)
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       field.components.count
     */
    [[nodiscard]] constexpr Components components() const noexcept {
        return components_of(desc.component);
    }

    /**
     * @brief Total number of sample points.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Equivalent to qp::abi::point_count on the value's description
     * @invariant   Returns 1 for a point field even when desc.count is non-zero
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       field.value_points, field.layout.point_counts
     */
    [[nodiscard]] constexpr std::uint64_t point_count() const noexcept {
        return qp::abi::point_count(desc);
    }

    /**
     * @brief Bytes the described lattice requires.
     *
     * Computed in 64-bit: `point_count` of a large volume grid times the element
     * size can exceed 32 bits, and truncating it would turn "this field is too
     * big to be real" into "this field is 0 bytes", which reads as a valid empty
     * field. The 64-bit product cannot wrap for any description that
     * `qp::abi::is_consistent` accepts.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Equivalent to qp::abi::data_bytes on the value's description
     * @invariant   Equals point_count() x component_count() x element size
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       field.value_points, field.value_bytes
     */
    [[nodiscard]] constexpr std::uint64_t required_bytes() const noexcept {
        return qp::abi::data_bytes(desc);
    }

    /**
     * @brief The dimension of the physical quantity, one per component.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Returns desc.dimension unchanged
     * @invariant   The same for every point in the field
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       field.dimension_is_per_component
     */
    [[nodiscard]] constexpr qp::abi::FieldDim dimension() const noexcept { return desc.dimension; }
};

/**
 * @brief Whether a field's shape is describable at all.
 *
 * This is the "is this a field?" question, and it deliberately does **not** look
 * at `data` or `bytes`: a description can be perfectly well formed while the
 * buffer is missing, and a caller that conflates the two cannot tell "the plugin
 * described nonsense" from "the plugin did not produce anything yet".
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Equivalent to qp::abi::is_consistent on the value's description
 * @invariant   True implies required_bytes() is a correct byte count
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       field.value_is_valid
 */
[[nodiscard]] constexpr bool is_valid_field(const FieldValue& v) noexcept {
    return qp::abi::is_consistent(v.desc);
}

/**
 * @brief Whether a field can actually be read point by point.
 *
 * Stricter than `is_valid_field`: it also requires a non-null pointer, a byte
 * span that covers the described lattice, and an alignment that makes f64 reads
 * legal. Both checks exist because they fail for different reasons and a caller
 * usually wants to report which one happened.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        False when any of: description inconsistent, null data,
 *              bytes < required_bytes(), or f64 data not 8-byte aligned
 * @invariant   True implies every component index in range is readable
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       field.value_is_valid, field.value_is_valid_f64_alignment
 */
[[nodiscard]] constexpr bool is_readable(const FieldValue& v) noexcept {
    if (!is_valid_field(v)) return false;
    if (v.data == nullptr) return false;
    if (v.bytes < v.required_bytes()) return false;
    if (v.desc.element == qp::abi::ElementType::f64) {
        // A misaligned f64 pointer is not a hypothesis: a plugin can hand over a
        // view into the middle of a packed buffer, and the resulting load is
        // undefined behaviour rather than a wrong number.
        return reinterpret_cast<std::uintptr_t>(v.data) % alignof(double) == 0;
    }
    return true;
}

namespace detail {

/// @brief f32 element pointer, or null when the field is not f32.
[[nodiscard]] inline const float* elements_f32(const FieldValue& v) noexcept {
    if (v.desc.element != qp::abi::ElementType::f32) return nullptr;
    return static_cast<const float*>(v.data);
}

/// @brief f64 element pointer, or null when the field is not f64.
[[nodiscard]] inline const double* elements_f64(const FieldValue& v) noexcept {
    if (v.desc.element != qp::abi::ElementType::f64) return nullptr;
    return static_cast<const double*>(v.data);
}

/// @brief Flat element offset of one component of one point, or 0 when out of range.
[[nodiscard]] inline std::uint64_t element_offset(const FieldValue& v, std::uint64_t point,
                                                  std::uint32_t component) noexcept {
    if (!has_component(v.components(), component)) return 0;
    if (point >= v.point_count()) return 0;
    return point * component_count(v.components()) + component;
}

}  // namespace detail

/**
 * @brief Reads one component of one sample point as a double.
 *
 * f32 data is widened, never the other way round: the scalar measurement chain
 * works in f64 (ADR-0005), and a widening conversion is exact.
 *
 * @ownership   pure
 * @thread      any
 * @pre         the caller has checked is_readable(v) -- this function does not
 * @post        Returns the stored value; returns 0.0 when the component does not
 *              exist or the point is out of range
 * @invariant   Never reads outside the described lattice
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       field.component_access, field.components.rejects_out_of_range
 */
[[nodiscard]] inline double get_component(const FieldValue& v, std::uint64_t point,
                                          std::uint32_t component) noexcept {
    if (!has_component(v.components(), component)) return 0.0;
    if (point >= v.point_count()) return 0.0;
    const std::uint64_t offset = detail::element_offset(v, point, component);
    if (const float* p = detail::elements_f32(v); p != nullptr) {
        return static_cast<double>(p[offset]);
    }
    if (const double* p = detail::elements_f64(v); p != nullptr) {
        return p[offset];
    }
    return 0.0;
}

}  // namespace qp::graph::field
