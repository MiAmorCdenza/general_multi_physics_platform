/**
 * @file descriptor.hpp
 * @brief Node-type descriptions: `NodeDesc` / `PortDesc` / `ParamDesc`.
 *
 * ## Core decision: **Param and Port are unified**
 *
 * In the old project's `engine/ports.py`, `Port` and `Param` were two mechanisms:
 * `Port` was both a wiring socket and a parameter; `Param` was a parameter with no wiring.
 * Each needed its own validation, UI and serialization -- the same job done twice over.
 *
 * This project unifies them into `PortDesc`, told apart by one `connectable` flag:
 *   - `connectable == true`  : can be wired (has a socket)
 *   - `connectable == false` : value only (a "parameter")
 *
 * Upside: validation, UI rendering, YAML serialization and type checking all have one code path.
 * So wiring a parameter up is **naturally supported or naturally rejected**, rather than
 * "not implemented over in the other mechanism".
 *
 * ## A node has only three optional hooks
 *
 * `compute` / `validate` / `on_param`. This is a **ceiling**, not a starting point.
 * A fourth hook requires an ADR first: every hook added must be understood by every plugin author.
 *
 * @frozen yes (`NodeDesc`'s shape is frozen; a new optional field needs an ADR)
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>
#include <qp/units.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace qp::graph {

/// @brief Sequence number of a port/parameter (1-based; 0 means none).
using PortNumber = std::uint32_t;

/**
 * @brief Description of one port (or parameter).
 *
 * With `connectable == false` it is the old project's `Param`.
 *
 * @ownership   owns (name / label / description / choice_names are self-held)
 * @thread      any (read-only after construction)
 * @pre         none
 * @post        none
 * @invariant   a PortDesc's number is unique within its owning NodeDesc
 * @errors      noexcept
 * @frozen      yes
 * @tests       graph.desc.port_basic, graph.desc.port_connectable_flag,
 *              graph.desc.port_numeric_bounds, graph.desc.port_choices
 */
struct PortDesc final {
    PortNumber number = 0;                 ///< sequence number within the node (1-based)
    std::string name;                      ///< programmatic identifier (YAML key, script name)
    std::string label;                     ///< user-facing display name (may be empty -> use name)
    std::string description;               ///< help text

    qp::ports::PortTypeId type = qp::ports::kScalarF64;

    /// Whether it can be wired. `false` means "parameter" -- value only, set in the panel.
    ///
    /// Input ports may be parameters; **output ports must be connectable** (else they are pointless).
    bool connectable = true;

    /// Whether it is required. When absent the node cannot be evaluated (reported at validation).
    bool required = false;

    // -- UI hints (not evaluated, but drive automatic property-panel rendering) --
    /// Numeric lower bound (meaningful for numeric types only).
    double min_value = 0.0;
    /// Numeric upper bound.
    double max_value = 0.0;
    /// Whether the numeric range applies (false: the range is not checked).
    bool has_range = false;
    /// Suggested step (for the UI; 0 means no suggestion).
    double step = 0.0;
    /// Unit conversion factor: what the user types in the UI x this factor = the SI value.
    /// E.g. the user types centimeters, factor = 0.01. Default 1.0 (the user types SI values).
    double unit_factor = 1.0;
    /// Unit symbol (display only). Empty means it is derived from the port's dimension.
    std::string unit_symbol;
    /// Stable names of the enum choices (meaningful when type == kEnum).
    std::vector<std::string> choice_names;
    /// Display labels of the enum choices (one-to-one with choice_names).
    std::vector<std::string> choice_labels;

    [[nodiscard]] bool valid() const noexcept { return number != 0 && !name.empty(); }
};

/**
 * @brief Evaluation result: port number -> value.
 *
 * A `std::vector` rather than a map: a node has few ports (a handful to a few dozen),
 * and evaluation is on the hot path, where a linear scan beats hashing by a wide margin.
 */
using PortValues = std::vector<std::pair<PortNumber, qp::ports::Value>>;

/// @brief Input set: a read-only view doing a linear lookup by port number.
class InputView final {
public:
    explicit InputView(const PortValues& values) noexcept : values_(&values) {}

    /**
     * @brief Look up an input by key. A missing key returns an invalid Value.
     *
     * @ownership   observes
     * @thread      any
     * @pre         none
     * @post        a port that was not supplied returns `Value{}` (kind == invalid)
     * @invariant   the underlying collection is not modified
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       graph.desc.input_view_lookup
     */
    [[nodiscard]] qp::ports::Value get(PortNumber number) const noexcept {
        for (const auto& [n, v] : *values_) {
            if (n == number) return v;
        }
        return qp::ports::Value{};
    }

    /// @brief Read an f64 input. A missing one returns 0.0.
    [[nodiscard]] double f64(PortNumber number) const noexcept { return get(number).as_f64(); }
    /// @brief Read an i64 input. A missing one returns 0.
    [[nodiscard]] std::int64_t i64(PortNumber number) const noexcept { return get(number).as_i64(); }
    /// @brief Read a boolean input. A missing one returns false.
    [[nodiscard]] bool boolean(PortNumber number) const noexcept { return get(number).as_bool(); }
    /// @brief Read a text input. Missing or wrongly typed returns an empty string.
    ///
    /// The reference points into the **underlying collection**, so it lives as long as this view.
    [[nodiscard]] const std::string& text(PortNumber number) const noexcept {
        for (const auto& [n, v] : *values_) {
            if (n == number) return v.as_text();
        }
        static const std::string kEmpty{};
        return kEmpty;
    }

private:
    const PortValues* values_;
};

/**
 * @brief Description of a node type.
 *
 * @ownership   owns
 * @thread      any (read-only once registered)
 * @pre         none
 * @post        none
 * @invariant   one type_name is unique within the registry
 * @errors      noexcept
 * @frozen      yes
 * @tests       graph.desc.node_basic, graph.desc.node_port_lookup,
 *              graph.desc.node_output_count, graph.desc.node_has_hooks
 */
struct NodeDesc final {
    /// Stable type name (the YAML `type:`, the plugin registration key). Never rename later.
    std::string type_name;
    /// User-facing display name.
    std::string label;
    /// Help text.
    std::string description;
    /// Category (groups the node palette), e.g. "mechanics" / "signal".
    std::string category;

    /// Plugin origin id. Empty for built-in nodes. Used for attribution and "which plugin".
    std::string source;

    /// Implementation version. Used to trigger migration when a plugin is upgraded (core/plugin).
    std::uint32_t version = 1;

    std::vector<PortDesc> inputs;
    std::vector<PortDesc> outputs;

    /// Whether it may appear in the baked (field) domain. Allowed by default.
    bool allow_in_field_domain = true;
    /// Whether it may appear in the real-time (particle) domain.
    ///
    /// The real-time domain runs every frame, so it **forbids** anything that may block or allocate.
    /// Declared explicitly by the plugin; denied by default (conservative).
    bool allow_in_particle_domain = false;

    /// Whether it has an evaluation implementation. Without one a node is declaration-only.
    bool has_compute = false;

    // -- Lookup helpers ------------------------------------------------------

    /// @brief Look up a port by number (inputs and outputs). Returns nullptr when not found.
    [[nodiscard]] const PortDesc* find_port(PortNumber number, bool is_output) const noexcept;
    /// @brief Look up a port by name. Returns nullptr when not found.
    [[nodiscard]] const PortDesc* find_by_name(std::string_view name,
                                               bool is_output) const noexcept;
    /// @brief Number of output ports (a quick test for whether evaluation is needed).
    [[nodiscard]] std::size_t output_count() const noexcept { return outputs.size(); }

    [[nodiscard]] bool valid() const noexcept { return !type_name.empty(); }
};

/// @brief Node-type registry interface: from a type name to its description.
///
/// Deliberately an **interface**, not a concrete container: the host implements it, plugins
/// consume it read-only. Tests can then inject a fake registry without building a plugin system.
class INodeCatalog {
public:
    INodeCatalog() = default;
    virtual ~INodeCatalog() = default;
    INodeCatalog(const INodeCatalog&) = delete;
    INodeCatalog& operator=(const INodeCatalog&) = delete;

    /// @brief Look up a description by type name. Returns nullptr when unregistered.
    [[nodiscard]] virtual const NodeDesc* find(std::string_view type_name) const noexcept = 0;
    /// @brief Number of registered types.
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

}  // namespace qp::graph
