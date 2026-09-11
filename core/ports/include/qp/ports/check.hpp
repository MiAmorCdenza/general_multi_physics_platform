/**
 * @file check.hpp
 * @brief Port type checking: decided at **load time**, not at evaluation time.
 *
 * ## Why it must happen at load time
 *
 * Pushing type/unit checking to evaluation time looks like "the student waited ten minutes
 * for an absurd curve"; refusing at load time costs one error dialog. The two differ by an
 * order of magnitude in a classroom -- a design premise of `core/graph/validate`.
 *
 * ## Three layers of checking, none optional
 *
 * 1. **Shape compatibility**: can the types themselves meet (`check_connection`)
 * 2. **Dimension agreement**: do both ends carry the same physical dimension (`check_dimensions`)
 * 3. **Value usability**: does a concrete value fit the port declaration (`check_value`)
 *
 * They are split because they happen at different moments. 1 and 2 are decided while the
 * graph is built, and 3 after every evaluation (to catch a plugin returning a wrong kind).
 *
 * ## `kAny` is a deliberate escape hatch, but it must leave a trace
 *
 * `any` voids type checking. It should appear only on a render chain that forwards data
 * untouched. So the verdict **explicitly** carries a `has_any` flag, letting the
 * inter-layer check count and warn instead of leaving a type-safety hole in silence.
 *
 * @frozen no (the checking rules may grow; the semantics for published types are frozen)
 */
#pragma once

#include <qp/diag/diagnostic.hpp>
#include <qp/diag/result.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>

#include <cstdint>

namespace qp::ports {

using qp::diag::Diagnostic;
using qp::diag::Result;
using qp::diag::SourceId;

/// @brief The verdict on a port connection.
enum class ConnectionVerdict : std::uint8_t {
    /// May be connected directly.
    ok = 0,
    /// An `any` port: allowed, but **type checking is void**. The caller should log and warn.
    ok_with_any = 1,
    /// Same shape, different width (f32 <-> f64): allowed, and **one conversion at the port boundary is required**.
    ok_with_numeric_widening = 2,
    /// Different shape: refused.
    type_mismatch = 3,
    /// Same shape, different dimension: refused.
    dimension_mismatch = 4,
    /// Either end's type is unregistered: refused.
    unknown_type = 5,
    /// Output to output / input to input: refused (the wire direction is wrong).
    direction_mismatch = 6,
};

/// @brief Port direction. A wire may only run output -> input.
enum class PortDirection : std::uint8_t {
    input = 0,
    output = 1,
};

/**
 * @brief The full verdict on a connection.
 *
 * Not just a boolean: the caller must know **why** and **whether to warn**.
 */
struct ConnectionCheck final {
    ConnectionVerdict verdict = ConnectionVerdict::type_mismatch;
    /// Whether an `any` port is involved (type checking is void; warn about it).
    bool has_any = false;
    /// Whether a numeric width conversion is needed (f32 <-> f64).
    bool needs_numeric_conversion = false;

    [[nodiscard]] constexpr bool acceptable() const noexcept {
        return verdict == ConnectionVerdict::ok || verdict == ConnectionVerdict::ok_with_any ||
               verdict == ConnectionVerdict::ok_with_numeric_widening;
    }
};

/// @brief Stable short name of a verdict.
[[nodiscard]] constexpr const char* to_string(ConnectionVerdict v) noexcept {
    switch (v) {
        case ConnectionVerdict::ok: return "ok";
        case ConnectionVerdict::ok_with_any: return "ok_with_any";
        case ConnectionVerdict::ok_with_numeric_widening: return "ok_with_numeric_widening";
        case ConnectionVerdict::type_mismatch: return "type_mismatch";
        case ConnectionVerdict::dimension_mismatch: return "dimension_mismatch";
        case ConnectionVerdict::unknown_type: return "unknown_type";
        case ConnectionVerdict::direction_mismatch: return "direction_mismatch";
    }
    return "unknown";
}

/**
 * @brief Decides whether two ports may be connected.
 *
 * @ownership   pure
 * @thread      any
 * @pre         The ids of from/to are in the registry (otherwise unknown_type is returned)
 * @post        Returns a definite verdict and never throws
 * @invariant   Swapping from/to leaves the type_mismatch / dimension_mismatch verdict unchanged
 * @errors      noexcept
 * @complexity  O(n) (registry lookup)
 * @nondet      none
 * @frozen      no
 * @tests       ports.check.connect_same_type, ports.check.connect_rejects_direction,
 *              ports.check.connect_rejects_type_mismatch,
 *              ports.check.connect_rejects_dimension_mismatch,
 *              ports.check.connect_allows_numeric_widening,
 *              ports.check.connect_any_is_flagged, ports.check.connect_unknown_type,
 *              ports.check.connect_is_symmetric_for_mismatch
 */
[[nodiscard]] ConnectionCheck check_connection(const PortTypeDesc& from, PortDirection from_dir,
                                               const PortTypeDesc& to, PortDirection to_dir) noexcept;

/**
 * @brief Decides whether the dimensions of two types are compatible.
 *
 * Rules:
 *   - either end's constraint is `any` -> compatible (unconstrained)
 *   - either end is `same_as_input` -> **not decided here** (it needs graph context;
 *     `core/graph/validate` resolves it and then calls this function)
 *   - both ends are `exact` -> the dimensions must be equal axis by axis
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A dimension is compatible with itself
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       ports.check.dimension_compatible, ports.check.dimension_any_accepts_all,
 *              ports.check.dimension_same_as_input_is_deferred
 */
[[nodiscard]] bool check_dimensions(const PortTypeDesc& a, const PortTypeDesc& b) noexcept;

/**
 * @brief Decides whether a value fits the port declaration.
 *
 * The gate that catches "a plugin returned a value of the wrong type"; it runs **after every evaluation**.
 *
 * What is checked:
 *   - whether the value's kind matches the port's numeric / field_components
 *   - an `any` port accepts any value
 *   - an f32 value on an f64 port: accepted (lossless widening)
 *   - an f64 value on an f32 port: accepted but flagged as narrowing (lossy; caller decides)
 *   - a vector field on a scalar field port: refused
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        On failure returns a diagnostic with a concrete reason, not a bare error code
 * @invariant   A successful verdict means the as_* accessors yield the expected type
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       ports.check.value_matches_port, ports.check.value_rejects_wrong_kind,
 *              ports.check.value_accepts_widening, ports.check.value_any_accepts_all,
 *              ports.check.value_rejects_invalid, ports.check.value_field_components_matter
 */
[[nodiscard]] Result<void> check_value(const PortTypeDesc& port, const Value& v,
                                       SourceId source = SourceId{"ports"});

}  // namespace qp::ports
