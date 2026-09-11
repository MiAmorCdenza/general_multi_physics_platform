/**
 * @file registry.hpp
 * @brief The kernel registry: which operators exist, and who provides them.
 *
 * ## Why registration is a two-phase commit
 *
 * A plugin that fails halfway through loading must leave **no trace**. If the
 * registry installed an entry the moment a plugin announced it, then a plugin
 * that then failed to prepare would leave behind a kernel that appears in the
 * editor, can be wired into a graph, and fails at run time -- which is precisely
 * the "an unloaded plugin's node type still exists" defect the previous project
 * shipped. So:
 *
 *   - `declare` reserves an id and a name, and commits to nothing;
 *   - `commit` installs the implementation, and is the only call that makes the
 *     kernel usable;
 *   - `drop` releases a reservation, or removes a committed kernel on unload.
 *
 * The same rule as `core/ports::PortTypeRegistry`, deliberately: two registries
 * that resolve identity the same way are easier to reason about than two
 * registries that each invent their own answer.
 *
 * ## Why the registry does not own its implementations
 *
 * An `IBatchAdvancer` is owned by the plugin that provides it. If the registry
 * owned it, the registry would need to know how a plugin is unloaded, and the
 * lifetime question would move from the plugin system (which has the answer)
 * into a table of function pointers (which does not).
 *
 * @ownership   owns (the table, not the kernels it points at)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every committed entry has a valid desc(); every reserved entry has a valid id
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       kernel.registry.register_and_find, kernel.registry.rejects_bad_input,
 *              kernel.registry.declare_commit_drop, kernel.registry.failed_load_leaves_no_trace
 */
#pragma once

#include <qp/graph/kernels/kernel.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace qp::graph::kernels {

/**
 * @brief Registry of available time-stepping operators.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A name appears at most once, in at most one state
 * @errors      noexcept (all failure is reported through Result)
 * @frozen      no
 * @tests       kernel.registry.register_and_find
 */
class KernelRegistry final {
public:
    KernelRegistry() = default;

    KernelRegistry(const KernelRegistry&) = delete;
    KernelRegistry& operator=(const KernelRegistry&) = delete;

    /**
     * @brief Reserves an id and a name without making the kernel usable.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `name` is non-empty and stays alive while the entry exists
     * @post        On success the id is reserved; find(id) still reports "reserved"
     * @invariant   Two live reservations never share an id or a name
     * @errors      Returns invalid_argument for an empty name; duplicate_connection
     *              when the name is already reserved or committed
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.registry.declare_commit_drop
     */
    [[nodiscard]] diag::Result<KernelId> declare(std::string_view name);

    /**
     * @brief Installs the implementation for a reserved id. The kernel becomes usable.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `desc.impl != nullptr` and `desc.name` equals the declared name
     * @post        find(id) and find_by_name(name) return this description
     * @invariant   A committed entry's desc() is never mutated afterwards
     * @errors      Returns unknown_node when the id is not reserved;
     *              duplicate_connection when it is already committed;
     *              invalid_argument for an invalid description; plugin_incompatible
     *              when the name does not match the reservation
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.registry.declare_commit_drop
     */
    [[nodiscard]] diag::Result<void> commit(KernelId id, const KernelDesc& desc);

    /**
     * @brief Releases a reservation, or removes a committed kernel.
     *
     * Called when a plugin unloads. After the call the id is dead: a plan
     * operation still holding it resolves to nothing rather than to a dangling
     * pointer into an unloaded module.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        The name is free again; find() on the id returns null
     * @invariant   No other entry changes
     * @errors      Returns unknown_node for an id that is not live
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.registry.declare_commit_drop
     */
    [[nodiscard]] diag::Result<void> drop(KernelId id);
    /**
     * @brief One-call registration for a kernel whose implementation is ready.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `desc.valid()`
     * @post        Equivalent to declare() followed by commit()
     * @invariant   Equivalent to the two-phase path in every observable way
     * @errors      Returns invalid_argument for an empty name, a null
     *              implementation, or a duplicate name; propagates commit()'s code
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.registry.register_and_find, kernel.registry.rejects_bad_input
     */
    [[nodiscard]] diag::Result<KernelId> add(const KernelDesc& desc);

    /// @brief The description of a live kernel, or null when the id is unknown or reserved.
    [[nodiscard]] const KernelDesc* find(KernelId id) const noexcept;

    /// @brief The description registered under `name`, or null.
    [[nodiscard]] const KernelDesc* find_by_name(std::string_view name) const noexcept;

    /// @brief Whether `id` is live (committed). Reserved-but-uncommitted ids are not live.
    [[nodiscard]] bool is_live(KernelId id) const noexcept { return find(id) != nullptr; }

    /// @brief Number of committed kernels.
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief Number of reserved-but-uncommitted ids. Non-zero means a plugin is mid-load.
    [[nodiscard]] std::size_t reserved_count() const noexcept;

    /**
     * @brief Every committed kernel, in reservation order.
     *
     * Order is the reservation order, not sorted: the editor lists kernels in the
     * order plugins registered them, which is the order the user's plugins were
     * loaded, and that is stable across runs because plugin loading is ordered.
     *
     * @ownership   pure (returns descriptions by value)
     * @thread      main
     * @pre         none
     * @post        One entry per committed kernel
     * @invariant   Repeated calls without intervening mutation return an equal vector
     * @errors      noexcept; allocation failure terminates, because a registry that
     *              cannot list what it holds is not a state the host can continue from
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       kernel.registry.listing_is_reservation_ordered
     */
    [[nodiscard]] std::vector<KernelDesc> list() const noexcept;

private:
    struct Slot final {
        KernelId id{};
        std::string_view name{};   ///< Non-owning: the provider owns the string
        KernelDesc desc{};
        bool committed = false;
    };

    /// @brief Index into `slots_`, or npos.
    [[nodiscard]] std::size_t index_of(KernelId id) const noexcept;
    [[nodiscard]] std::size_t index_of_name(std::string_view name) const noexcept;

    std::vector<Slot> slots_;
    std::uint32_t next_index_ = 1;   ///< 0 is the kNoKernel sentinel
};

}  // namespace qp::graph::kernels
