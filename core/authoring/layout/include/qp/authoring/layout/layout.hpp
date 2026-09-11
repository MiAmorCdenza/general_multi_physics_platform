/**
 * @file layout.hpp
 * @brief The layout contract: positions in, positions out. The algorithms are plugins.
 *
 * ## Why the algorithm is not here
 *
 * Where to put a node is a design question with several defensible answers:
 * layered (Sugiyama), force-directed, radial, or "wherever the user dropped it,
 * and never move it again". A platform that picks one has decided for every user
 * of every domain, and the answer is wrong for most of them -- a circuit wants
 * layered, a state machine wants circular, and a teaching demo wants whatever the
 * author arranged by hand.
 *
 * So the foundation owns the **contract** and the registry, and the algorithms
 * are plugins. That is the same split as `graph/kernels`: the interface is
 * foundation, the implementations are content.
 *
 * ## Why the algorithm is pure
 *
 * `compute` takes a graph and returns positions. It cannot read a file, cannot
 * use a clock, and cannot use a random number generator unless it is given a
 * seed. Two reasons, and the second is the important one:
 *
 *   - a layout that reads a clock produces a different picture on every run,
 *     which makes a screenshot-based regression test impossible;
 *   - a layout that uses an unseeded RNG makes **the document non-reproducible**:
 *     the saved file differs between two runs of the same experiment, so a diff
 *     of two documents no longer means "something changed in the physics".
 *
 * A force-directed algorithm is still allowed to be random; it takes the seed
 * from the caller, who takes it from the run. That is what makes "same seed,
 * same picture" true.
 *
 * ## Why positions are integers
 *
 * A position is a graph-coordinate, not a screen pixel. Integers make the saved
 * document stable: a float layout that depends on iteration order or on the
 * host's floating-point mode would produce `120.00000000000001` on one machine
 * and `120.0` on another, and every save would show a spurious diff. A view that
 * needs sub-pixel placement scales on the way to the screen, where it belongs.
 *
 * @ownership   pure (contract) / owns (the registry)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A layout result never contains a node id that is not in the graph
 * @errors      noexcept
 * @complexity  --
 * @nondet      only through the seed the caller supplies
 * @frozen      no
 * @tests       authoring.layout.registry_register_and_find,
 *              authoring.layout.compute_returns_positions,
 *              authoring.layout.registry_duplicate_is_refused
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace qp::authoring {

/**
 * @brief A position in graph coordinates.
 *
 * Integers, and the type is deliberately not a floating-point pair: see the file
 * comment. A view that needs sub-pixel placement scales on the way to the screen.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   none
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.layout.compute_returns_positions
 */
struct Point final {
    std::int32_t x = 0;
    std::int32_t y = 0;

    [[nodiscard]] friend constexpr bool operator==(Point a, Point b) noexcept {
        return a.x == b.x && a.y == b.y;
    }
    [[nodiscard]] friend constexpr bool operator!=(Point a, Point b) noexcept {
        return !(a == b);
    }
};

/**
 * @brief One node's placement: where it goes, and which node it is.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `id` refers to a node that was in the graph the algorithm was given
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.layout.compute_returns_positions
 */
struct Placement final {
    graph::NodeId id{};
    Point position{};
};

/**
 * @brief The result of running an algorithm: a placement per node.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   At most one placement per node id
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.layout.compute_returns_positions
 */
struct LayoutResult final {
    std::vector<Placement> placements{};

    /// @brief The position of `id`, or (0, 0) when the result does not place it.
    [[nodiscard]] Point position_of(graph::NodeId id) const noexcept;

    /// @brief Whether the result places every node the algorithm was asked about.
    [[nodiscard]] bool covers(std::size_t node_count) const noexcept {
        return placements.size() == node_count;
    }

    /// @brief Number of placed nodes.
    [[nodiscard]] std::size_t size() const noexcept { return placements.size(); }
};

/**
 * @brief A layout algorithm.
 *
 * Implementations are plugins.
 *
 * @ownership   observes (the plugin owns itself)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `name` outlives the registration and never changes
 * @errors      `compute` reports failure through Result rather than throwing
 * @frozen      no
 * @tests       authoring.layout.compute_returns_positions
 */
class ILayoutAlgorithm {
public:
    ILayoutAlgorithm() = default;
    virtual ~ILayoutAlgorithm() = default;
    ILayoutAlgorithm(const ILayoutAlgorithm&) = delete;
    ILayoutAlgorithm& operator=(const ILayoutAlgorithm&) = delete;

    /// @brief Stable unique name, e.g. "qp.layout.layered".
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Places every node of `graph`.
     *
     * @ownership   pure (reads the graph, returns a new result)
     * @thread      main
     * @pre         `seed` is the run seed when the algorithm is random, and is
     *              ignored otherwise
     * @post        The result places every node, or the call reports failure
     * @invariant   Same graph and same seed produce the same result
     * @errors      Returns an ErrorCode rather than throwing
     * @complexity  implementation-defined
     * @nondet      only through `seed`
     * @frozen      no
     * @tests       authoring.layout.compute_returns_positions,
     *              authoring.layout.compute_is_deterministic
     */
    [[nodiscard]] virtual diag::Result<LayoutResult> compute(
        const graph::Graph& graph, std::uint64_t seed) = 0;
};

/// @brief Handle for a registered algorithm.
struct LayoutId final {
    std::uint32_t index = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    [[nodiscard]] friend constexpr bool operator==(LayoutId a, LayoutId b) noexcept {
        return a.index == b.index;
    }
};

/// @brief Description of a registered algorithm.
struct LayoutDesc final {
    std::string_view name{};
    std::string_view summary{};
    ILayoutAlgorithm* impl = nullptr;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return !name.empty() && impl != nullptr;
    }
};

/**
 * @brief Registry of layout algorithms: which ones exist, and who provides them.
 *
 * @ownership   owns (the table, never the algorithms)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A name appears at most once
 * @errors      noexcept (failure is reported through Result)
 * @frozen      no
 * @tests       authoring.layout.registry_register_and_find
 */
class LayoutRegistry final {
public:
    LayoutRegistry() = default;
    LayoutRegistry(const LayoutRegistry&) = delete;
    LayoutRegistry& operator=(const LayoutRegistry&) = delete;

    /**
     * @brief Registers an algorithm.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `desc.valid()` and `desc.name` outlives the registration
     * @post        find_by_name(desc.name) returns it
     * @invariant   On failure the registry is unchanged
     * @errors      Returns invalid_argument for an empty name or null
     *              implementation, duplicate_connection for a repeated name
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.layout.registry_register_and_find,
     *              authoring.layout.registry_duplicate_is_refused
     */
    [[nodiscard]] diag::Result<LayoutId> add(const LayoutDesc& desc);

    /**
     * @brief Removes an algorithm, for a plugin that is unloading.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        The name is free again
     * @invariant   Other entries are unaffected
     * @errors      Returns unknown_node when the id is not registered
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.layout.registry_remove
     */
    [[nodiscard]] diag::Result<void> remove(LayoutId id);

    /// @brief The algorithm registered under `name`, or null.
    [[nodiscard]] const LayoutDesc* find_by_name(std::string_view name) const noexcept;

    /// @brief The algorithm for `id`, or null.
    [[nodiscard]] const LayoutDesc* find(LayoutId id) const noexcept;

    /// @brief Every registered algorithm, in registration order.
    [[nodiscard]] std::vector<LayoutDesc> list() const noexcept;

    /// @brief Number of registered algorithms.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry final {
        LayoutId id{};
        LayoutDesc desc{};
    };

    std::vector<Entry> entries_;
    std::uint32_t next_index_ = 1;
};

}  // namespace qp::authoring
