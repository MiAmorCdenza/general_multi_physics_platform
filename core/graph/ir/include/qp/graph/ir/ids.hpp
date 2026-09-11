/**
 * @file ids.hpp
 * @brief Identity of graph elements: **handle + generation**, not a bare integer.
 *
 * ## Why neither bare indices nor string IDs
 *
 * Bare indices: deleting node n3 and adding a new node reuses index 3, so every reference
 * to the old n3 (undo stack, cache key, UI selection, diagnostics) **silently points at the
 * new node**. That is the hardest bug class to chase: nothing crashes, the wrong object changes.
 *
 * String IDs: the `n3` in a student's notebook should be stable and readable, but string
 * keys make every evaluation pay hashing and comparison, and renaming easily breaks references.
 *
 * Hence: internally use `(index, generation)`, and bump the generation when a slot is reused.
 * Old references thus become a "detectable invalidation" instead of "silently pointing elsewhere".
 *
 * The stable name shown to users and to YAML is **another layer** (`NodeDesc::name`) that
 * takes no part in internal addressing.
 *
 * @frozen yes (the semantics of `index`/`generation` and their zero values are frozen)
 */
#pragma once

#include <cstdint>

namespace qp::graph {

/// @brief Slot index. 0 means "none".
using SlotIndex = std::uint32_t;

/// @brief Generation number. Bumped each time a slot is reused, to spot dead handles.
using Generation = std::uint32_t;

/// @brief Invalid index.
inline constexpr SlotIndex kNoSlot = 0;

/// @brief Invalid generation.
inline constexpr Generation kNoGeneration = 0;

/**
 * @brief Handle of a graph node.
 *
 * @ownership   pure (a value type, freely copyable)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A default-constructed handle is invalid
 * @errors      noexcept
 * @frozen      yes
 * @tests       graph.ids.node_default_is_invalid, graph.ids.node_equality,
 *              graph.ids.node_generation_matters
 */
struct NodeId final {
    SlotIndex index = kNoSlot;
    Generation generation = kNoGeneration;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kNoSlot && generation != kNoGeneration;
    }

    [[nodiscard]] friend constexpr bool operator==(NodeId a, NodeId b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    [[nodiscard]] friend constexpr bool operator!=(NodeId a, NodeId b) noexcept {
        return !(a == b);
    }
    /// @brief For ordered containers. Compares numbers only and **implies no semantic order**.
    [[nodiscard]] friend constexpr bool operator<(NodeId a, NodeId b) noexcept {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    }
};

/// @brief Number of a port inside a node. 0 means "none".
///
/// A port number is **stable** within a node type: the number of `NodeDesc::inputs[i]` is `i + 1`.
/// So a port name can change (it faces the user) while internal references stay put.
using PortIndex = std::uint32_t;

inline constexpr PortIndex kNoPort = 0;

/// @brief Port direction.
enum class PortDirection : std::uint8_t {
    input = 0,
    output = 1,
};

/**
 * @brief A location: one port of one node.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A default-constructed location is invalid
 * @errors      noexcept
 * @frozen      yes
 * @tests       graph.ids.port_ref_default_is_invalid, graph.ids.port_ref_equality
 */
struct PortRef final {
    NodeId node{};
    PortIndex port = kNoPort;
    PortDirection direction = PortDirection::input;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return node.valid() && port != kNoPort;
    }

    [[nodiscard]] friend constexpr bool operator==(PortRef a, PortRef b) noexcept {
        return a.node == b.node && a.port == b.port && a.direction == b.direction;
    }
    [[nodiscard]] friend constexpr bool operator!=(PortRef a, PortRef b) noexcept {
        return !(a == b);
    }
};

/// @brief Version number of the graph. Bumped by any structural mutation.
///
/// Uses: cache invalidation, the undo stack, incremental UI refresh, "which graph version is this snapshot?"
using GraphVersion = std::uint64_t;

}  // namespace qp::graph
