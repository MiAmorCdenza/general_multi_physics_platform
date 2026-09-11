/**
 * @file registry.cpp
 * @brief Implementation of the port type registry.
 */
#include <qp/ports/registry.hpp>

#include <algorithm>

namespace qp::ports {
namespace {

/// @brief Build a scalar port description.
[[nodiscard]] PortTypeDesc scalar(std::string_view name, PortTypeId id, NumericKind numeric,
                                  qp::units::Dim dim) noexcept {
    PortTypeDesc d{};
    d.id = id;
    d.name = name;
    d.numeric = numeric;
    d.constraint = DimensionConstraint::exact;
    d.dimension = dim;
    return d;
}

/// @brief Build a non-numeric port description (text, handle, etc.).
[[nodiscard]] PortTypeDesc plain(std::string_view name, PortTypeId id) noexcept {
    PortTypeDesc d{};
    d.id = id;
    d.name = name;
    d.numeric = NumericKind::none;
    d.constraint = DimensionConstraint::any;
    return d;
}

/// @brief Build a field port description.
[[nodiscard]] PortTypeDesc field(std::string_view name, PortTypeId id,
                                 std::uint8_t components) noexcept {
    PortTypeDesc d{};
    d.id = id;
    d.name = name;
    d.numeric = NumericKind::none;
    d.constraint = DimensionConstraint::any;
    d.field_element = 0;   // f32 (see ADR-0005: field data is always f32)
    d.field_components = components;
    return d;
}

}  // namespace

std::vector<PortTypeDesc> make_builtin_types() {
    std::vector<PortTypeDesc> out;
    out.reserve(kBuiltinTypeCount + 12);

    // -- Scalars -------------------------------------------------------------
    // Dimensionless scalars: the concrete dimension is overridden by the node's port declaration;
    // the builtin description here is "a numeric value with no specific dimension", for generic nodes.
    out.push_back(scalar("scalar_f64", kScalarF64, NumericKind::f64, qp::units::Dim{}));
    out.push_back(scalar("scalar_f32", kScalarF32, NumericKind::f32, qp::units::Dim{}));
    out.push_back(scalar("bool", kBool, NumericKind::boolean, qp::units::Dim{}));
    out.push_back(scalar("int64", kInt64, NumericKind::i64, qp::units::Dim{}));
    out.push_back(plain("string", kString));
    out.push_back(plain("enum", kEnum));
    out.push_back(plain("dimension", kDimension));

    // -- Fields --------------------------------------------------------------
    out.push_back(field("scalar_field", kScalarField, 1));
    out.push_back(field("vector_field", kVectorField, 3));
    out.push_back(plain("field_table", kFieldTable));

    // -- Geometry and particles ----------------------------------------------
    out.push_back(plain("particle_buffer", kParticleBuffer));
    out.push_back(plain("geometry", kGeometry));

    // -- Measurement and data ------------------------------------------------
    out.push_back(plain("dataset", kDataset));
    out.push_back(plain("fit_result", kFitResult));

    // -- Escape hatch --------------------------------------------------------
    {
        PortTypeDesc d{};
        d.id = kAny;
        d.name = "any";
        d.numeric = NumericKind::none;
        d.constraint = DimensionConstraint::any;
        d.is_any = true;
        out.push_back(d);
    }

    return out;
}

PortTypeRegistry::PortTypeRegistry() {
    for (const auto& d : make_builtin_types()) {
        // Builtin types are not constructed through register_type: they are the authoritative
        // definition, and the registration path would apply the "host reserved range" check to them.
        types_.push_back(d);
    }
}

Result<void> PortTypeRegistry::register_type(const PortTypeDesc& desc) {
    using qp::diag::ErrorCode;

    if (desc.id == kInvalidType) {
        return Result<void>{ErrorCode::invalid_argument};
    }
    if (desc.name.empty()) {
        return Result<void>{ErrorCode::missing_field};
    }

    // Host reserved range: a plugin ID must be >= kUserTypeBase.
    // Note that kAny(1000) is also inside the reserved range and is a builtin, so what is
    // checked here is "already taken by a builtin", not "the value is below the threshold".
    const bool is_reserved_id = desc.id < kUserTypeBase;
    const PortTypeDesc* existing = find(desc.id);

    if (existing != nullptr) {
        // Idempotent: the same ID with an identical description -> allowed (a plugin loading twice is normal)
        if (existing->name == desc.name && existing->numeric == desc.numeric &&
            existing->constraint == desc.constraint &&
            existing->dimension == desc.dimension &&
            existing->field_element == desc.field_element &&
            existing->field_components == desc.field_components &&
            existing->is_any == desc.is_any) {
            return Result<void>{};   // no change
        }
        // Same ID, different description -> reject. A silent overwrite would let two plugins trample each other.
        return Result<void>{ErrorCode::duplicate_connection};
    }

    if (is_reserved_id) {
        // Unoccupied but inside the reserved range: a plugin must not use a host-reserved ID
        return Result<void>{ErrorCode::out_of_range};
    }

    types_.push_back(desc);
    return Result<void>{};
}

const PortTypeDesc* PortTypeRegistry::find(PortTypeId id) const noexcept {
    if (id == kInvalidType) return nullptr;
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [id](const PortTypeDesc& d) { return d.id == id; });
    return it == types_.end() ? nullptr : &*it;
}

const PortTypeDesc* PortTypeRegistry::find_by_name(std::string_view name) const noexcept {
    if (name.empty()) return nullptr;
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [name](const PortTypeDesc& d) { return d.name == name; });
    return it == types_.end() ? nullptr : &*it;
}

bool PortTypeRegistry::has_builtin(PortTypeId id) const noexcept {
    const PortTypeDesc* d = find(id);
    return d != nullptr && d->id < kUserTypeBase;
}

const PortTypeRegistry& builtin_registry() noexcept {
    // Local static initialization has been thread-safe since C++11, and nothing modifies it
    // afterwards, so this is immutable shared data and does not violate "no mutable global state".
    static const PortTypeRegistry instance{};
    return instance;
}

}  // namespace qp::ports
