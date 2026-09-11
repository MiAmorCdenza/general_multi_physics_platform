/**
 * @file value.hpp
 * @brief The value flowing on a port: a copyable, type-safe, dimension-neutral tagged union.
 *
 * ## Why not `std::any`
 *
 * `std::any` requires `typeid` to match before a value can be taken, and `typeid`:
 *   - is unreliable across DLL boundaries (each DLL has its own RTTI);
 *   - cannot be serialised (a student's saved experiment must open on another machine);
 *   - cannot be enumerated by a graphical editor (which must list "what can connect to this port").
 *
 * So an explicit `Kind` tag is used. It doubles as an **enumerable type list**
 * that the UI and the validators consume directly.
 *
 * ## Why the dimension does not go into `Value`
 *
 * The dimension is **a property of the port**, not of the value. The same `42.0` on a length port means
 * 42 metres, on a time port 42 seconds. Pushing the dimension into the value would cause:
 *   - Every number to drag a dimension along (cache keys full of dimensions, lower hit rate);
 *   - "The same value on ports of different dimension" to become a type error, though it is physically legal.
 *
 * Deciding that "this value suits this port" is done by `check.hpp`.
 *
 * ## Large objects always travel as handles
 *
 * A 19MB field must not appear inside a value. `field_handle` is only a POD descriptor;
 * the real data lifetime is guaranteed by the publisher in `core/abi` (see `ABI_LAYOUT.md` section 3.4).
 *
 * @ownership   pure (value type, self-contained)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   confirm the kind before using any `as_*`, otherwise a fallback value is returned instead of UB
 * @errors      noexcept (no accessor throws)
 * @frozen      yes (the numeric values of `ValueKind` are frozen)
 * @tests       ports.value.default_is_invalid, ports.value.construction,
 *              ports.value.numeric_accessors, ports.value.kind_is_exhaustive,
 *              ports.value.copy_independence, ports.value.never_throws,
 *              ports.value.widening_is_lossless_for_f32
 */
#pragma once

#include <qp/abi/lattice.hpp>
#include <qp/units.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <variant>

namespace qp::ports {

/// @brief Kind of the value. The numeric value is a stable tag and must not be reordered.
enum class ValueKind : std::uint8_t {
    invalid = 0,
    f64 = 1,
    f32 = 2,
    i64 = 3,
    boolean = 4,
    text = 5,
    dimension = 6,
    /// Field handle: descriptor only, no data. The real data is kept alive by the publisher.
    field_handle = 7,
};

/**
 * @brief The value flowing on a port.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   reading an accessor repeatedly on the same object gives the same result
 * @errors      noexcept (accessors do not throw; constructing text may allocate, and failure means terminate)
 * @frozen      yes
 */
class Value final {
public:
    /// @brief Default construction: an invalid value (not computed yet).
    Value() noexcept = default;

    /// @brief Construct from f64.
    explicit Value(double v) noexcept : kind_(ValueKind::f64), data_(v) {}
    /// @brief Construct from f32. Keeps the precision explicit (ADR-0005).
    explicit Value(float v) noexcept : kind_(ValueKind::f32), data_(v) {}
    /// @brief Construct from an integer.
    explicit Value(std::int64_t v) noexcept : kind_(ValueKind::i64), data_(v) {}
    /// @brief Construct from a boolean.
    explicit Value(bool v) noexcept : kind_(ValueKind::boolean), data_(v) {}
    /// @brief Construct from a dimension.
    explicit Value(qp::units::Dim d) noexcept : kind_(ValueKind::dimension), data_(d) {}
    /// @brief Construct from text.
    explicit Value(std::string s) : kind_(ValueKind::text), data_(std::move(s)) {}

    /// @brief Construct from a field descriptor. **Copies the descriptor only, never the data.**
    explicit Value(qp::abi::LatticeDesc lattice) noexcept
        : kind_(ValueKind::field_handle), data_(lattice) {}

    [[nodiscard]] ValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool valid() const noexcept { return kind_ != ValueKind::invalid; }

    // -- Accessors -----------------------------------------------------------
    //
    // Every one of them returns a fallback value instead of UB: a plugin that gets its kind check wrong
    // should get a diagnosable result, not a crash or random bits. @pre violations are reported by check_value.

    /// @brief Get f64. Returns 0.0 when the kind does not match.
    [[nodiscard]] double as_f64() const noexcept {
        return kind_ == ValueKind::f64 ? std::get<double>(data_) : 0.0;
    }
    /// @brief Get f32. Returns 0.0f when the kind does not match.
    [[nodiscard]] float as_f32() const noexcept {
        return kind_ == ValueKind::f32 ? std::get<float>(data_) : 0.0f;
    }
    /// @brief Get the integer. Returns 0 when the kind does not match.
    [[nodiscard]] std::int64_t as_i64() const noexcept {
        return kind_ == ValueKind::i64 ? std::get<std::int64_t>(data_) : 0;
    }
    /// @brief Get the boolean. Returns false when the kind does not match.
    [[nodiscard]] bool as_bool() const noexcept {
        return kind_ == ValueKind::boolean && std::get<bool>(data_);
    }
    /// @brief Get the dimension. Returns a dimensionless value when the kind does not match.
    [[nodiscard]] qp::units::Dim as_dimension() const noexcept {
        return kind_ == ValueKind::dimension ? std::get<qp::units::Dim>(data_)
                                             : qp::units::Dim{};
    }
    /// @brief Get the text. Returns an empty string when the kind does not match.
    [[nodiscard]] const std::string& as_text() const noexcept {
        static const std::string kEmpty{};
        return kind_ == ValueKind::text ? std::get<std::string>(data_) : kEmpty;
    }
    /// @brief Get the field descriptor. Returns a default lattice when the kind does not match.
    [[nodiscard]] qp::abi::LatticeDesc as_field() const noexcept {
        return kind_ == ValueKind::field_handle ? std::get<qp::abi::LatticeDesc>(data_)
                                                : qp::abi::LatticeDesc{};
    }

    // -- Queries -------------------------------------------------------------

    /// @brief Whether it is numeric (f64 / f32 / i64 / boolean).
    [[nodiscard]] bool is_numeric() const noexcept {
        switch (kind_) {
            case ValueKind::f64:
            case ValueKind::f32:
            case ValueKind::i64:
            case ValueKind::boolean:
                return true;
            default:
                return false;
        }
    }

    /**
     * @brief Take every numeric kind as a double. Returns 0.0 for non-numeric kinds.
     *
     * This is where ADR-0005's "the one widening at the port boundary" happens:
     * f32 and integer values are promoted to double here for in-graph computation.
     * f32 -> double is **lossless** (a property test asserts it).
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        numeric kinds return their value; non-numeric kinds return 0.0
     * @invariant   the f32 payload survives the round trip: static_cast<float>(to_double()) == as_f32()
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       ports.value.numeric_accessors, ports.value.widening_is_lossless_for_f32
     */
    [[nodiscard]] double to_double() const noexcept {
        switch (kind_) {
            case ValueKind::f64: return std::get<double>(data_);
            case ValueKind::f32: return static_cast<double>(std::get<float>(data_));
            case ValueKind::i64: return static_cast<double>(std::get<std::int64_t>(data_));
            case ValueKind::boolean: return std::get<bool>(data_) ? 1.0 : 0.0;
            default: return 0.0;
        }
    }

    /// @brief Stable short name of the kind. Used by diagnostics and the UI.
    [[nodiscard]] const char* kind_name() const noexcept {
        switch (kind_) {
            case ValueKind::invalid: return "invalid";
            case ValueKind::f64: return "f64";
            case ValueKind::f32: return "f32";
            case ValueKind::i64: return "i64";
            case ValueKind::boolean: return "boolean";
            case ValueKind::text: return "text";
            case ValueKind::dimension: return "dimension";
            case ValueKind::field_handle: return "field_handle";
        }
        return "unknown";
    }

    /**
     * @brief Equality: same kind **and** same payload.
     *
     * Written by hand instead of `data_ == other.data_` on purpose: that would require every alternative
     * type in the variant to have `operator==`, and `abi::LatticeDesc` is a pure POD with no comparison
     * operators (the ABI layer knows bytes, not semantics). Comparing per alternative keeps the decision
     * "which types are comparable" inside this file instead of leaking it to abi.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        different kinds are always unequal
     * @invariant   reflexive and symmetric
     * @errors      noexcept
     * @complexity  O(len) (text comparison)
     * @nondet      none
     * @frozen      no
     * @tests       ports.value.construction, ports.value.field_handle_carries_no_data
     */
    [[nodiscard]] friend bool operator==(const Value& a, const Value& b) noexcept {
        if (a.kind_ != b.kind_) return false;
        switch (a.kind_) {
            case ValueKind::invalid:
                return true;
            case ValueKind::f64:
                return std::get<double>(a.data_) == std::get<double>(b.data_);
            case ValueKind::f32:
                return std::get<float>(a.data_) == std::get<float>(b.data_);
            case ValueKind::i64:
                return std::get<std::int64_t>(a.data_) == std::get<std::int64_t>(b.data_);
            case ValueKind::boolean:
                return std::get<bool>(a.data_) == std::get<bool>(b.data_);
            case ValueKind::text:
                return std::get<std::string>(a.data_) == std::get<std::string>(b.data_);
            case ValueKind::dimension:
                return std::get<qp::units::Dim>(a.data_) == std::get<qp::units::Dim>(b.data_);
            case ValueKind::field_handle: {
                // LatticeDesc is a POD: comparing bytes is comparing semantics
                const auto& la = std::get<qp::abi::LatticeDesc>(a.data_);
                const auto& lb = std::get<qp::abi::LatticeDesc>(b.data_);
                return std::memcmp(&la, &lb, sizeof(la)) == 0;
            }
        }
        return false;
    }
    [[nodiscard]] friend bool operator!=(const Value& a, const Value& b) noexcept {
        return !(a == b);
    }

private:
    ValueKind kind_ = ValueKind::invalid;
    std::variant<std::monostate, double, float, std::int64_t, bool, std::string,
                 qp::units::Dim, qp::abi::LatticeDesc>
        data_{};
};

}  // namespace qp::ports
