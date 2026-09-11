/**
 * @file check.cpp
 * @brief Implementation of port type checking.
 */
#include <qp/ports/check.hpp>

namespace qp::ports {
namespace {

using qp::diag::ErrorCode;

/// @brief Whether two numeric kinds need a width conversion, and in which direction.
enum class NumericBridge : std::uint8_t {
    identical = 0,   ///< exactly the same
    widen = 1,       ///< a is narrower, so reaching b needs widening (lossless)
    narrow = 2,      ///< a is wider, so reaching b needs narrowing (lossy)
    incompatible = 3 ///< different numeric categories (e.g. f64 -> bool)
};

/// @brief Rank of a numeric kind. Used to decide the widening/narrowing direction.
[[nodiscard]] constexpr int numeric_rank(NumericKind k) noexcept {
    switch (k) {
        case NumericKind::none: return -1;
        case NumericKind::boolean: return 0;
        case NumericKind::i32: return 1;
        case NumericKind::i64: return 2;
        case NumericKind::f32: return 3;
        case NumericKind::f64: return 4;
    }
    return -1;
}

/// @brief Decides how two numeric kinds bridge.
///
/// Rules (deliberately conservative):
///   - exactly the same -> identical
///   - f32 <-> f64 -> widen / narrow (the only width interchange ADR-0005 explicitly allows)
///   - any other pair (e.g. f64 -> bool, i64 -> f64) -> incompatible
///
/// Why i64 -> f64 is not allowed: a large integer loses bits in a double, and silent bit loss
/// is exactly the error class we guard against; a node that needs it declares a conversion port.
[[nodiscard]] constexpr NumericBridge bridge_of(NumericKind a, NumericKind b) noexcept {
    if (a == b) return NumericBridge::identical;

    const auto is_float = [](NumericKind k) {
        return k == NumericKind::f32 || k == NumericKind::f64;
    };
    if (is_float(a) && is_float(b)) {
        return numeric_rank(a) < numeric_rank(b) ? NumericBridge::widen : NumericBridge::narrow;
    }
    return NumericBridge::incompatible;
}

/// @brief Whether two field ports agree on component count and element kind.
[[nodiscard]] constexpr bool fields_shape_equal(const PortTypeDesc& a,
                                                const PortTypeDesc& b) noexcept {
    return a.field_components == b.field_components && a.field_element == b.field_element;
}

}  // namespace

bool check_dimensions(const PortTypeDesc& a, const PortTypeDesc& b) noexcept {
    // Either end leaves the dimension unconstrained -> compatible
    if (a.constraint == DimensionConstraint::any || b.constraint == DimensionConstraint::any) {
        return true;
    }
    // same_as_input needs graph context to resolve; not decided here (compatible, deferred)
    if (a.constraint == DimensionConstraint::same_as_input ||
        b.constraint == DimensionConstraint::same_as_input) {
        return true;
    }
    return a.dimension == b.dimension;
}

ConnectionCheck check_connection(const PortTypeDesc& from, PortDirection from_dir,
                                 const PortTypeDesc& to, PortDirection to_dir) noexcept {
    ConnectionCheck r{};

    // Direction: only output -> input
    if (from_dir == to_dir) {
        r.verdict = ConnectionVerdict::direction_mismatch;
        return r;
    }
    // Internally always treated as output -> input
    const PortTypeDesc& src = (from_dir == PortDirection::output) ? from : to;
    const PortTypeDesc& dst = (from_dir == PortDirection::output) ? to : from;

    if (!src.valid() || !dst.valid()) {
        r.verdict = ConnectionVerdict::unknown_type;
        return r;
    }

    // The any escape hatch: allowed, but explicitly flagged
    if (src.is_any || dst.is_any) {
        r.verdict = ConnectionVerdict::ok_with_any;
        r.has_any = true;
        return r;
    }

    // Field kinds: the shape must match exactly
    const bool src_field = src.is_field();
    const bool dst_field = dst.is_field();
    if (src_field != dst_field) {
        r.verdict = ConnectionVerdict::type_mismatch;
        return r;
    }
    if (src_field && dst_field) {
        if (!fields_shape_equal(src, dst)) {
            r.verdict = ConnectionVerdict::type_mismatch;
            return r;
        }
        // A field's dimension constraint is usually any; compare only when both constrain it
        if (!check_dimensions(src, dst)) {
            r.verdict = ConnectionVerdict::dimension_mismatch;
            return r;
        }
        r.verdict = ConnectionVerdict::ok;
        return r;
    }

    // Numeric scalars: decide by numeric category
    if (src.is_numeric() != dst.is_numeric()) {
        r.verdict = ConnectionVerdict::type_mismatch;
        return r;
    }
    if (src.is_numeric() && dst.is_numeric()) {
        switch (bridge_of(src.numeric, dst.numeric)) {
            case NumericBridge::identical:
                break;
            case NumericBridge::widen:
                r.needs_numeric_conversion = true;
                break;
            case NumericBridge::narrow:
                // Narrowing is allowed but must be flagged: losing precision is the caller's call
                r.needs_numeric_conversion = true;
                break;
            case NumericBridge::incompatible:
                r.verdict = ConnectionVerdict::type_mismatch;
                return r;
        }
        if (!check_dimensions(src, dst)) {
            r.verdict = ConnectionVerdict::dimension_mismatch;
            return r;
        }
        r.verdict = r.needs_numeric_conversion ? ConnectionVerdict::ok_with_numeric_widening
                                               : ConnectionVerdict::ok;
        return r;
    }

    // Non-numeric, non-field kinds (text, handles, datasets, ...): must be exactly the same kind
    if (src.id != dst.id) {
        r.verdict = ConnectionVerdict::type_mismatch;
        return r;
    }
    r.verdict = ConnectionVerdict::ok;
    return r;
}

Result<void> check_value(const PortTypeDesc& port, const Value& v, SourceId source) {
    if (!port.valid()) {
        return Result<void>{ErrorCode::unknown_port_type};
    }

    // An any port accepts everything (including invalid -- "not computed yet" is legal for any)
    if (port.is_any) {
        return Result<void>{};
    }

    if (!v.valid()) {
        return Result<void>{ErrorCode::missing_field};
    }

    // -- field kinds ---------------------------------------------------------
    if (port.is_field()) {
        if (v.kind() != ValueKind::field_handle) {
            return Result<void>{ErrorCode::type_mismatch};
        }
        const auto lattice = v.as_field();
        const std::uint32_t expected = port.field_components;
        const std::uint32_t actual = qp::abi::component_count(lattice.component);
        if (expected != actual) {
            return Result<void>{ErrorCode::type_mismatch};
        }
        return Result<void>{};
    }

    // -- scalar kinds --------------------------------------------------------
    switch (port.numeric) {
        case NumericKind::f64:
            // An f32 value may go on an f64 port (lossless widening)
            if (v.kind() == ValueKind::f64 || v.kind() == ValueKind::f32) {
                return Result<void>{};
            }
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::f32:
            // f64 -> f32 is lossy but **allowed**: narrowing is the caller's explicit choice (ADR-0005)
            if (v.kind() == ValueKind::f32 || v.kind() == ValueKind::f64) {
                return Result<void>{};
            }
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::boolean:
            if (v.kind() == ValueKind::boolean) return Result<void>{};
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::i64:
        case NumericKind::i32:
            // An integer port takes **only** integers: no silent bit loss
            if (v.kind() == ValueKind::i64) return Result<void>{};
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::none:
            break;
    }

    // -- non-numeric: expected kind by ID ------------------------------------
    switch (port.id) {
        case kString:
        case kEnum:
            return v.kind() == ValueKind::text ? Result<void>{}
                                               : Result<void>{ErrorCode::type_mismatch};
        case kDimension:
            return v.kind() == ValueKind::dimension ? Result<void>{}
                                                    : Result<void>{ErrorCode::type_mismatch};
        case kParticleBuffer:
        case kGeometry:
        case kFieldTable:
        case kDataset:
        case kFitResult:
            // The payloads of these types are defined in their own modules (abi / runtime/store).
            // The port layer only guarantees "kinds are not mixed"; the user checks the payload.
            return Result<void>{};
        default:
            break;
    }

    (void)source;
    return Result<void>{ErrorCode::type_mismatch};
}

}  // namespace qp::ports
