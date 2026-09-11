/**
 * @file port_type.hpp
 * @brief Port type description: the port's **type contract**.
 *
 * ## Why a port type must be registerable data, not a C++ type
 *
 * Graph editing, YAML loading, script bindings and a future web front end must all answer the
 * same question: "can this port connect to that port". If the type information hid inside a C++
 * type, every consumer would implement the decision itself, and four implementations would drift.
 *
 * So the type information is **data**: a `PortTypeDesc` that can be queried, enumerated and
 * serialized. The C++ side is only a thin convenience wrapper around it.
 *
 * ## Why the ID is a stable integer and not a `type_index`
 *
 * `std::type_index` is valid only within one process, but:
 *   - it cannot be written into YAML (a student's saved experiment must open on another machine);
 *   - it cannot cross a DLL boundary (the RTTI of different DLLs is not shared);
 *   - it cannot cross languages (a Python binding cannot obtain it).
 *
 * So the ID is an explicit `uint16_t`, and plugin types are allocated from `kUserTypeBase` upward.
 *
 * @frozen yes -- the ID and name of a registered type are immutable once published.
 */
#pragma once

#include <qp/diag/error.hpp>
#include <qp/units.hpp>

#include <cstdint>
#include <string_view>

namespace qp::ports {

/// @brief Port type identifier. Stable, serializable, language-independent.
using PortTypeId = std::uint16_t;

/// @brief Fixed IDs of the builtin types. **The numeric values must not be reordered.**
enum BuiltinTypeId : PortTypeId {
    kInvalidType = 0,

    // -- 1..19 scalars -------------------------------------------------------
    kScalarF64 = 1,   ///< Double-precision scalar. The default for measurement chains, parameters, uncertainties
    kScalarF32 = 2,   ///< Single-precision scalar. **Only for ports that meet field data** (ADR-0005)
    kBool = 3,
    kInt64 = 4,
    kString = 5,
    kEnum = 6,        ///< Named choice; the values live in PortTypeDesc::choice_names
    kDimension = 7,   ///< The dimension itself as a value (for "unit" parameters)

    // -- 20..39 fields -------------------------------------------------------
    kScalarField = 20,   ///< Scalar field (a baked-domain product)
    kVectorField = 21,   ///< Vector field (a baked-domain product)
    kFieldTable = 22,    ///< Time-series field table

    // -- 40..59 geometry and particles ---------------------------------------
    kParticleBuffer = 40,
    kGeometry = 41,

    // -- 60..79 measurement and data (where the measurement platform differs) --
    kDataset = 60,       ///< Measurement data table: value + uncertainty + timestamp + validity flag
    kFitResult = 61,     ///< Fit result: coefficients + covariance + residuals + R^2

    // -- Special -------------------------------------------------------------
    /// "Any type": **disables type checking**. Only for a purely forwarding render chain.
    /// The layer validation (core/graph/validate) warns about every use of it.
    kAny = 1000,

    /// Start of the ID range for plugin-defined types. IDs below this are reserved for the host.
    kUserTypeBase = 1024,
};

/// @brief Number of core (host builtin) types, used for the registry's static capacity.
inline constexpr std::size_t kBuiltinTypeCount = 8;

// -- Underlying numeric representation of a value -----------------------------

/**
 * @brief Numeric kind. Decides whether "same shape, different precision" can connect.
 *
 * This information is ADR-0005 landing in the type system:
 * `scalar_f32` and `scalar_f64` are different port types, but they are compatible at the
 * **value level**; the conversion happens once, at the port boundary.
 */
enum class NumericKind : std::uint8_t {
    none = 0,   ///< Not numeric (string / field / dataset, etc.)
    f32 = 1,
    f64 = 2,
    i32 = 3,
    i64 = 4,
    boolean = 5,
};

// -- Dimension constraint -----------------------------------------------------

/**
 * @brief Dimension constraint of a port.
 *
 * The three states are deliberate:
 *   - `exact`  : must equal dim. The vast majority of ports are this kind.
 *   - `any`    : no dimension constraint. Only for genuinely generic ports (e.g. "any field").
 *   - `same_as_input`: an output port's dimension is decided by some input port's dimension
 *     (e.g. an add node's output = the common dimension of its inputs). **Layer validation** resolves it.
 */
enum class DimensionConstraint : std::uint8_t {
    exact = 0,
    any = 1,
    same_as_input = 2,
};

/**
 * @brief Type description of a port.
 *
 * @ownership   owns (`name` / `choice_names` point at static storage, see below)
 * @thread      any (read-only after construction)
 * @pre         name is non-empty when id != kInvalidType
 * @post        none
 * @invariant   The description of a given id is unique within a registry
 * @errors      noexcept
 * @frozen      yes
 * @tests       ports.type_desc.basic_fields, ports.type_desc.dimension_constraint,
 *              ports.type_desc.numeric_kind_consistency
 */
struct PortTypeDesc final {
    PortTypeId id = kInvalidType;            ///< Stable ID
    std::string_view name{};                 ///< Stable short name, e.g. "scalar_f64"
    NumericKind numeric = NumericKind::none; ///< Underlying numeric kind

    DimensionConstraint constraint = DimensionConstraint::exact;

    /// Meaningful only when constraint == exact.
    qp::units::Dim dimension{};

    /// Element type of a field-like port. f32 for non-field types (meaningless there).
    /// Reuses the abi value semantics: 0 = f32, 1 = f64.
    std::uint8_t field_element = 0;

    /// Component count of a field-like port: scalar field 1, vector field 3, non-field 0.
    std::uint8_t field_components = 0;

    /// Whether this is "any type". When true type checking is disabled -- **deliberately explicit**,
    /// so layer validation can count and warn about it instead of letting the hole appear silently.
    bool is_any = false;

    [[nodiscard]] constexpr bool valid() const noexcept { return id != kInvalidType; }

    /// @brief Whether this is a field-like port.
    [[nodiscard]] constexpr bool is_field() const noexcept { return field_components > 0; }

    /// @brief Whether this is a numeric scalar (comparable with other numeric scalars).
    [[nodiscard]] constexpr bool is_numeric() const noexcept {
        switch (numeric) {
            case NumericKind::f32:
            case NumericKind::f64:
            case NumericKind::i32:
            case NumericKind::i64:
            case NumericKind::boolean:
                return true;
            case NumericKind::none:
                return false;
        }
        return false;
    }
};

}  // namespace qp::ports
