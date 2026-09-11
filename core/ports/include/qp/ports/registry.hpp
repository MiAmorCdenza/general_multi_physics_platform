/**
 * @file registry.hpp
 * @brief The port-type registry: the **single source of truth** for types.
 *
 * ## Why an instance and not a global singleton
 *
 * "Mutable global state" is the most common physical shape of unclear ownership, and it lets
 * tests pollute each other (see `qp-no-mutable-global` in `standards/enforcement.md` section 3).
 * The registry is therefore an explicitly passed object: the host holds one, each test holds one.
 *
 * The cost is that every function needing a type lookup must be handed the registry reference.
 * That cost is worth paying: it makes "who registered what" a traceable fact, not implicit state.
 *
 * ## Three-way registration semantics
 *
 * A plugin may register the same ID twice. Three cases must be told apart:
 *   - **identical** (idempotent): allowed. A plugin being loaded twice is normal.
 *   - **conflict** (same ID, different description): rejected, naming the conflicting fields.
 *   - **an ID below kUserTypeBase**: rejected -- that is the host-reserved range.
 *
 * "Silent overwrite" is the most dangerous default: two plugins' type definitions trample each
 * other, and the symptom only shows up far away from the cause.
 *
 * @ownership   owns (holds value copies of every entry)
 * @thread      registration must complete single-threaded (plugin load); queries: any thread
 * @pre         none
 * @post        none
 * @invariant   a registered ID is never silently overwritten
 * @errors      register_type returns Result<void> and does not throw
 * @frozen      no (extensible; the semantics of registered types are frozen)
 * @tests       ports.registry.builtins_present, ports.registry.add_custom_type,
 *              ports.registry.rejects_duplicate_id, ports.registry.idempotent_reregister,
 *              ports.registry.rejects_reserved_range, ports.registry.rejects_invalid_id,
 *              ports.registry.count_and_lookup, ports.registry.deterministic_order
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace qp::ports {

using qp::diag::Result;

/**
 * @brief The port-type registry.
 *
 * @ownership   owns
 * @thread      registration: single thread; query: any thread (read-only once registered)
 * @pre         none
 * @post        none
 * @invariant   any ID corresponds to at most one PortTypeDesc
 * @errors      does not throw
 * @frozen      no
 */
class PortTypeRegistry final {
public:
    /// @brief Construct and register every host built-in type.
    PortTypeRegistry();

    PortTypeRegistry(const PortTypeRegistry&) = delete;
    PortTypeRegistry& operator=(const PortTypeRegistry&) = delete;

    /**
     * @brief Register one port type.
     *
     * @ownership   owns (copies the description)
     * @thread      main (plugin load time)
     * @pre         desc.id != kInvalidType; desc.name is not empty
     * @post        on success find(desc.id) returns a copy of desc
     * @invariant   on success count() grows by 1; on failure the registry is unchanged (strong)
     * @errors      Result<void>; see the error codes below
     *   - invalid_argument        the id is invalid / the name is empty
     *   - out_of_range            the id falls in the host-reserved range (< kUserTypeBase)
     *   - duplicate_connection    the id is already taken by a **different** description
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       ports.registry.add_custom_type, ports.registry.rejects_duplicate_id,
     *              ports.registry.idempotent_reregister, ports.registry.rejects_reserved_range,
     *              ports.registry.rejects_invalid_id
     */
    Result<void> register_type(const PortTypeDesc& desc);

    /**
     * @brief Look up by ID. Returns nullptr when unregistered.
     *
     * @ownership   observes (the pointer points into the registry; invalid once it dies)
     * @thread      any
     * @pre         none
     * @post        returns nullptr or a pointer to a registered description
     * @invariant   two lookups of one ID with no registration between them agree
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       ports.registry.count_and_lookup
     */
    [[nodiscard]] const PortTypeDesc* find(PortTypeId id) const noexcept;

    /**
     * @brief Look up by stable short name. Returns nullptr when unregistered.
     *
     * Name lookup exists for the YAML loader and script bindings: a student writes
     * `type: scalar_f64` instead of remembering a numeric ID.
     *
     * @ownership   observes
     * @thread      any
     * @pre         none
     * @post        returns nullptr or a pointer to a registered description
     * @invariant   the name-to-ID mapping is stable
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       ports.registry.lookup_by_name
     */
    [[nodiscard]] const PortTypeDesc* find_by_name(std::string_view name) const noexcept;

    /// @brief Number of registered types.
    [[nodiscard]] std::size_t count() const noexcept { return types_.size(); }

    /// @brief Read-only enumeration in registration order (for the UI and doc generation).
    [[nodiscard]] const std::vector<PortTypeDesc>& all() const noexcept { return types_; }

    /// @brief Whether a given built-in type exists (used by self-checks).
    [[nodiscard]] bool has_builtin(PortTypeId id) const noexcept;

private:
    std::vector<PortTypeDesc> types_;
};

/**
 * @brief Get the process-wide shared built-in registry.
 *
 * **Deliberately read-only**: it returns a const reference. Any caller that needs to register
 * a custom type must construct its own `PortTypeRegistry` and hold it explicitly -- that way
 * "who registered what" is always local and traceable.
 *
 * Why it exists: asking "what is the built-in scalar_f64" should not make every call site
 * build its own 8-entry table. This is **immutable** shared data, so no mutable-global rule breaks.
 *
 * @ownership   observes (a const reference to static storage, valid for the whole run)
 * @thread      any (the first call is thread-safe via C++11 static initialization)
 * @pre         none
 * @post        the returned object contains at least every built-in type
 * @invariant   the same object is returned across calls
 * @errors      noexcept
 * @complexity  O(1) (O(n) on the first call)
 * @nondet      none
 * @frozen      no
 * @tests       ports.registry.shared_builtins_is_readonly
 */
[[nodiscard]] const PortTypeRegistry& builtin_registry() noexcept;

/// @brief Construction of the built-in type descriptions. Shared to avoid drift.
[[nodiscard]] std::vector<PortTypeDesc> make_builtin_types();

}  // namespace qp::ports
