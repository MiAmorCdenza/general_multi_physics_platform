/**
 * @file graph.hpp
 * @brief Graph structure: nodes and edges, plus versions, acyclicity, strong exception safety.
 *
 * ## Three design decisions
 *
 * ### 1. Handles carry a generation, so slot reuse never silently points at the wrong object
 *
 * `NodeId` is `(index, generation)`. Deleting a node **clears the slot and moves no other
 * node**, so every existing handle stays valid (except the deleted one).
 * Thus "delete n3 and every other node's id is unchanged": undo stacks and cache keys survive.
 *
 * ### 2. After any failed mutation the graph and the version are unchanged (strong guarantee)
 *
 * Every mutating function has a `@post` that describes its failure branch specifically.
 * Reason: callers (the command bus, the undo stack) must be able to reason about "did anything
 * change when it failed?". A mutation that "maybe changed half" destroys the undo stack.
 *
 * ### 3. The version increments only after a **successful** mutation
 *
 * This is the only basis for cache invalidation. Incrementing on failure would clear the
 * cache for nothing; not incrementing on success would make the cache return stale results.
 *
 * ## What this class deliberately does not do
 *
 * | Not done         | Owned by                                   |
 * |------------------|--------------------------------------------|
 * | Evaluation       | `core/graph/eval`                          |
 * | Cache            | `core/graph/eval`                          |
 * | Node coordinates | the view layer (per-view `view_layouts`)   |
 * | Mutation history | `core/graph/mutate` (undo stack)           |
 *
 * The coordinates row matters most: **a node's x/y is layout output, not a graph property**.
 *
 * @ownership   owns (owns every node and edge)
 * @thread      main (mutation and reads are on the main thread; the eval thread reads a snapshot)
 * @pre         none
 * @post        none
 * @invariant   The graph is always acyclic; every input port has at most one incoming edge
 * @errors      Mutating functions return `Result<void>` and never throw
 * @frozen      no
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace qp::graph {

using qp::diag::Result;

/// @brief A node slot. Empty slots give O(1) removal and handle stability.
struct NodeSlot final {
    Node node{};
    Generation generation = 0;   ///< 0 means an empty slot
    bool occupied = false;
};

/**
 * @brief The graph: a container of nodes and edges.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   See the class documentation
 * @errors      Does not throw
 * @frozen      no
 * @tests       graph.structure.empty_graph, graph.structure.add_node,
 *              graph.structure.add_node_uses_fresh_generation,
 *              graph.structure.remove_node_invalidates_handle,
 *              graph.structure.remove_node_drops_edges,
 *              graph.structure.connect_and_lookup,
 *              graph.structure.connect_rejects_duplicate_input,
 *              graph.structure.connect_rejects_unknown_node,
 *              graph.structure.connect_rejects_direction,
 *              graph.structure.connect_rejects_cycle,
 *              graph.structure.connect_rejects_self_loop,
 *              graph.structure.disconnect_removes_edge,
 *              graph.structure.version_bumps_on_success_only,
 *              graph.structure.failed_mutation_is_noop,
 *              graph.structure.node_count_tracks_slots,
 *              graph.structure.incoming_lookup_by_input,
 *              graph.structure.find_node_by_user_name,
 *              graph.structure.deterministic_iteration_order,
 *              graph.structure.clear_resets
 */
class Graph final {
public:
    /// @brief Constructs an empty graph. Seeds one sentinel slot (see the implementation).
    Graph();

    // Not copyable: copying a graph means copying every node, edge and generation counter,
    // and the generations of the copies would diverge -- handles would not work across them.
    // For a snapshot call to_snapshot() **explicitly** (a later phase); the semantics are clearer.
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    // But **movable**: a move creates no second generation space and handles stay valid, so
    // "return it after construction", "put it in a container" and "store it in an undo record" work.
    Graph(Graph&&) noexcept = default;
    Graph& operator=(Graph&&) noexcept = default;
    ~Graph() = default;

    // -- Nodes ---------------------------------------------------------------

    /**
     * @brief Adds a node.
     *
     * @ownership   owns
     * @thread      main
     * @pre         type_name is not empty
     * @post        On success returns a valid handle, `version()` increments, node count +1
     * @post        **On failure the graph and version are unchanged**
     * @invariant   The returned handle's generation matches the slot's current generation
     * @errors      Result<NodeId>; empty type_name -> invalid_argument
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.add_node, graph.structure.add_node_uses_fresh_generation
     */
    Result<NodeId> add_node(std::string_view type_name);

    /**
     * @brief Adds a node and sets its user name at the same time.
     *
     * @ownership   owns
     * @thread      main
     * @pre         type_name is not empty; name is unique in this graph (when non-empty)
     * @post        Same as add_node, and `node.name == name`
     * @post        On failure the graph and version are unchanged
     * @invariant   A non-empty user name is unique across the graph
     * @errors      invalid_argument (empty type_name) / duplicate_connection (repeated name)
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.find_node_by_user_name
     */
    Result<NodeId> add_node_named(std::string_view type_name, std::string_view name);

    /**
     * @brief Reserves a slot. The returned handle is **valid immediately**, but has no type name yet.
     *
     * Why it is needed:
     *   the command bus (`core/graph/mutate`) must **turn the NodeId carried by a command
     *   into a valid handle first**, so that both apply and undo refer to the node by the
     *   same id. If undo-then-redo changed the id, every edge referencing that node, every
     *   cache key and the UI selection state would break -- that is unacceptable.
     *
     * Two-phase usage:
     * ```
     // valid at once, pending
     // supply the type name, commit
     * ```
     * Both phases **increment the version separately**: the intermediate state is brief, but it
     * is real and observable (another view might refresh exactly then). Hiding the intermediate
     * state would turn "the version changed but nothing else did" into an unexplainable event.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success: valid handle, version increments, node count +1, `is_stable()` false
     * @post        On failure the graph and version are unchanged
     * @invariant   The returned handle's generation matches the slot's current generation
     * @errors      Result<NodeId>; there is nothing to get wrong, so it always succeeds
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.reserve_and_fill, graph.structure.reserve_marks_pending
     */
    Result<NodeId> reserve_node();

    /**
     * @brief Completes the type name of a reserved slot (the commit phase).
     *
     * @ownership   owns
     * @thread      main
     * @pre         id points at a pending slot; type_name is not empty
     * @post        On success the node's type name is type_name and `is_stable()` is true again,
     *              and the version increments
     * @post        On failure the graph and version are unchanged
     * @invariant   One slot is never filled twice
     * @errors      Result<void>; invalid id -> unknown_node; empty type_name ->
     *              invalid_argument; a non-pending slot -> duplicate_connection
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.reserve_and_fill,
     *              graph.structure.fill_rejects_non_pending
     */
    Result<void> fill_reserved(NodeId id, std::string_view type_name);

    /// @brief Whether the graph has no pending nodes (that is, it is stable).
    [[nodiscard]] bool is_stable() const noexcept { return pending_nodes_ == 0; }

    /// @brief The current number of pending nodes.
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_nodes_; }

    /**
     * @brief Whether a handle points at a slot that is **reserved but not yet completed**.
     *
     * The command bus needs it to tell two situations apart:
     *   - `has_node(id) && !is_pending(id)` -> the id is taken by a real node, reject
     *   - `has_node(id) &&  is_pending(id)` -> the caller reserved it as agreed, fill it
     *
     * With `has_node` as the only predicate these two cannot be told apart, so the
     * **standard usage** "reserve first, then apply" would be misjudged as a conflict.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Returns false for an invalid handle or a non-pending slot
     * @invariant   `is_pending(id)` => `has_node(id)`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.is_pending_predicate
     */
    [[nodiscard]] bool is_pending(NodeId id) const noexcept {
        const Node* n = find_node(id);
        return n != nullptr && n->type_name.empty();
    }

    /**
     * @brief Restores a node under a **specified handle** (for undoing a deletion).
     *
     * Why it is needed: undo a deletion and redo it, and if the node got a new id, every
     * edge referencing it, every cache key and the UI selection state would break. Undo must
     * keep **the id unchanged**, or the redone graph is not semantically the same graph.
     *
     * The caller (`core/graph/mutate`) guarantees the handle once belonged to this graph and
     * has not been reused; this function still checks and rejects conflicts.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `id.generation != 0`; the slot is empty and its generation is not above id.generation
     * @post        On success `has_node(id)` is true, the node holds the snapshot, version increments
     * @post        On failure the graph and version are unchanged
     * @invariant   After the restore `find_node(id)->id == id`
     * @errors      Result<void>; invalid id -> invalid_argument;
     *              the slot is occupied or the generation conflicts -> duplicate_connection
     * @complexity  O(number of slots)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.restore_node_preserves_id,
     *              graph.structure.restore_node_preserves_id
     */
    Result<void> restore_node(const Node& snapshot);

    /**
     * @brief Restores an edge under a **specified endpoint pair** (for undoing a disconnect).
     *
     * @ownership   owns
     * @thread      main
     * @pre         Both endpoint nodes exist; the direction is output -> input
     * @post        On success the edge exists and the version increments
     * @post        On failure the graph and version are unchanged
     * @invariant   The graph is still acyclic and that input port still has at most one incoming edge
     * @errors      Result<void>; no such node -> unknown_node; wrong direction ->
     *              invalid_argument; an incoming edge already exists -> duplicate_connection;
     *              a cycle -> cycle_detected
     * @complexity  O(V + E)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.restore_edge_after_restore_node
     */
    Result<void> restore_edge(const Edge& e);

    /**
     * @brief Removes a node and every edge connected to it.
     *
     * @ownership   owns
     * @thread      main
     * @pre         id is valid
     * @post        On success the handle is invalidated and its slot generation increments (an
     *              old handle never comes back), the related edges are gone, version increments
     * @post        On failure the graph and version are unchanged
     * @invariant   After removal no edge references that node
     * @errors      Result<void>; invalid or already invalidated id -> unknown_node
     * @complexity  O(V + E)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.remove_node_invalidates_handle,
     *              graph.structure.remove_node_drops_edges
     */
    Result<void> remove_node(NodeId id);

    /// @brief Fetches a node. Returns nullptr for an invalid handle.
    [[nodiscard]] const Node* find_node(NodeId id) const noexcept;
    /// @brief Fetches a node (mutable). For parameters and flags only; **never type_name**.
    [[nodiscard]] Node* find_node_mutable(NodeId id) noexcept;

    /**
     * @brief Increments the version. For callers that modified node content in place.
     *
     * Why it exists: the pointer from `find_node_mutable` allows parameter and flag edits,
     * and those edits must invalidate the cache too. Without this entry point a caller has
     * only two routes: bypass the version (the cache returns stale results) or fake an
     * unrelated mutation (the version jumps for nothing). Both are worse.
     *
     * @ownership   pure (touches the version only)
     * @thread      main
     * @pre         none
     * @post        `version()` is strictly greater than before the call
     * @invariant   The version is monotonically increasing and never goes back
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.bump_version_is_monotonic
     */
    void bump_version() noexcept { ++version_; }

    /**
     * @brief Sets a node's user name.
     *
     * @ownership   owns (copies the name)
     * @thread      main
     * @pre         id is valid; a non-empty name is unique in this graph
     * @post        On success `find_node(id)->name == name` and the version increments
     * @post        On failure the graph and version are unchanged
     * @invariant   A non-empty user name is unique across the graph
     * @errors      Result<void>; invalid id -> unknown_node; a name clash ->
     *              duplicate_connection
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.set_node_name_enforces_uniqueness
     */
    Result<void> set_node_name(NodeId id, std::string_view name);

    /// @brief Fetches a node by user name. Returns an invalid handle when not found.
    [[nodiscard]] NodeId find_node_by_name(std::string_view name) const noexcept;
    /// @brief Whether a node handle is still valid (generations match).
    [[nodiscard]] bool has_node(NodeId id) const noexcept;

    // -- Edges ---------------------------------------------------------------

    /**
     * @brief Creates a connection.
     *
     * Validation order (fixed, so tests and diagnostics stay stable):
     *   1. both handles are valid                  -> unknown_node
     *   2. the direction is output-to-input        -> invalid_argument
     *   3. the input port has no incoming edge yet -> duplicate_connection
     *   4. it forms no self-loop                   -> cycle_detected
     *   5. it forms no cycle                       -> cycle_detected
     *
     * @ownership   owns
     * @thread      main
     * @pre         none (validation covers everything)
     * @post        On success the edge exists, the version increments, the graph is still acyclic
     * @post        **On failure the graph and version are unchanged**
     * @invariant   The graph is still acyclic after success
     * @errors      Result<void>; see the validation order above
     * @complexity  O(V + E) (cycle detection)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.connect_and_lookup,
     *              graph.structure.connect_rejects_duplicate_input,
     *              graph.structure.connect_rejects_unknown_node,
     *              graph.structure.connect_rejects_direction,
     *              graph.structure.connect_rejects_cycle,
     *              graph.structure.connect_rejects_self_loop
     */
    Result<void> connect(PortRef from, PortRef to);

    /**
     * @brief Disconnects the incoming edge of one input port.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        On success that input port's incoming edge is removed, the version increments
     * @post        On failure (the port had no incoming edge) the graph and version are unchanged
     * @invariant   After the disconnect that input port has no incoming edge
     * @errors      Result<void>; no incoming edge -> not_connected
     * @complexity  O(E)
     * @nondet      none
     * @frozen      no
     * @tests       graph.structure.disconnect_removes_edge
     */
    Result<void> disconnect(PortRef input);

    /// @brief Looks up the incoming edge of an input port. Returns nullptr when there is none.
    [[nodiscard]] const Edge* incoming(PortRef input) const noexcept;
    /// @brief Total number of edges.
    [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }
    /// @brief Read-only iteration over all edges (stable order: insertion order).
    [[nodiscard]] const std::vector<Edge>& edges() const noexcept { return edges_; }

    // -- Version and size ----------------------------------------------------

    /// @brief The current version. Increments on every successful mutation.
    [[nodiscard]] GraphVersion version() const noexcept { return version_; }
    /// @brief Number of live nodes.
    [[nodiscard]] std::size_t node_count() const noexcept { return live_nodes_; }
    /// @brief Total number of usable slots (**excluding** the sentinel slot at index 0).
    [[nodiscard]] std::size_t slot_count() const noexcept {
        return slots_.empty() ? 0 : slots_.size() - 1;
    }

    /// @brief Read-only iteration over all slots. **Includes** the sentinel at index 0, stable order.
    ///
    /// Therefore `slots().size() == slot_count() + 1`. See slot_count.
    [[nodiscard]] const std::vector<NodeSlot>& slots() const noexcept { return slots_; }

    /// @brief Clears the whole graph (the version increments).
    void clear() noexcept;

private:
    [[nodiscard]] Node* node_at(NodeId id) noexcept;
    /// @brief Whether starting at `to` reaches `from` (cycle check before connecting an edge).
    [[nodiscard]] bool reaches(NodeId from, NodeId target) const noexcept;

    std::vector<NodeSlot> slots_;
    std::vector<Edge> edges_;
    Generation next_generation_ = 1;
    GraphVersion version_ = 1;
    std::size_t live_nodes_ = 0;
    std::size_t pending_nodes_ = 0;   ///< Slots reserved but not yet given a type name
};

}  // namespace qp::graph
