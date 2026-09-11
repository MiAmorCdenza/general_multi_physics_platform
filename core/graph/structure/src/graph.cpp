/**
 * @file graph.cpp
 * @brief Implementation of the graph structure.
 *
 * Every mutation follows the same template:
 *   1. Validate (return immediately on failure, the graph is untouched)
 *   2. Apply (no failure is possible at this point)
 *   3. Bump the version number
 *
 * "The apply phase cannot fail" is the key: it makes the strong exception guarantee **structural**
 * instead of relying on a try/catch safety net.
 */
#include <qp/graph/structure/graph.hpp>

#include <algorithm>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

}  // namespace

Graph::Graph() {
    // Slot 0 is a **sentinel** and is never occupied.
    // Reason: NodeId::index == 0 means "none"; if 0 were a legal slot,
    // the first node added would get index 0 and be judged an invalid handle at once.
    // A permanently empty sentinel slot keeps "none" and "the 0th" fully apart.
    slots_.resize(1);
}

// -- Nodes -------------------------------------------------------------------

Node* Graph::node_at(NodeId id) noexcept {
    if (!id.valid() || id.index >= slots_.size()) return nullptr;
    NodeSlot& s = slots_[id.index];
    if (!s.occupied || s.generation != id.generation) return nullptr;
    return &s.node;
}

const Node* Graph::find_node(NodeId id) const noexcept {
    // Looked up directly (not through node_at): the const version must stay const-correct,
    // and calling the non-const version via const_cast makes GCC report -fpermissive errors.
    if (!id.valid() || id.index >= slots_.size()) return nullptr;
    const NodeSlot& s = slots_[id.index];
    if (!s.occupied || s.generation != id.generation) return nullptr;
    return &s.node;
}

Node* Graph::find_node_mutable(NodeId id) noexcept { return node_at(id); }

bool Graph::has_node(NodeId id) const noexcept { return find_node(id) != nullptr; }

Result<NodeId> Graph::add_node(std::string_view type_name) {
    if (type_name.empty()) {
        return Result<NodeId>{ErrorCode::invalid_argument};
    }

    // Find a free slot: reuse deleted slots first (so slots_ does not grow without bound).
    // Starts at 1: slot 0 is the sentinel (see the constructor).
    for (std::size_t i = 1; i < slots_.size(); ++i) {
        NodeSlot& s = slots_[i];
        if (s.occupied) continue;
        s.occupied = true;
        s.generation = next_generation_++;
        s.node = Node{};
        s.node.id = NodeId{static_cast<SlotIndex>(i), s.generation};
        s.node.type_name = std::string{type_name};
        ++live_nodes_;
        ++version_;
        return Result<NodeId>{s.node.id};
    }

    // No free slot: append.
    // Note: **push_back first, then read the id from slots_.back()**. An earlier version built
    // the id in a local before push_back, and the reallocation that push_back performs invalidated
    // that Node -- so the returned handle pointed at an already destroyed copy.
    {
        NodeSlot s{};
        s.occupied = true;
        s.generation = next_generation_++;
        s.node.type_name = std::string{type_name};
        s.node.id = NodeId{static_cast<SlotIndex>(slots_.size()), s.generation};
        slots_.push_back(std::move(s));
    }
    ++live_nodes_;
    ++version_;
    return Result<NodeId>{slots_.back().node.id};
}

Result<NodeId> Graph::add_node_named(std::string_view type_name, std::string_view name) {
    if (type_name.empty()) {
        return Result<NodeId>{ErrorCode::invalid_argument};
    }
    if (!name.empty() && find_node_by_name(name).valid()) {
        return Result<NodeId>{ErrorCode::duplicate_connection};
    }

    // Inline add_node's slot allocation so that setting the name and bumping the version happen in **one place**.
    // The earlier form called add_node and then set the name, and that second step forgot the version
    // bump, so "renaming" did not invalidate the cache -- caught by failed_mutation_is_noop.
    NodeId id{};
    for (std::size_t i = 1; i < slots_.size(); ++i) {
        NodeSlot& s = slots_[i];
        if (s.occupied) continue;
        s.occupied = true;
        s.generation = next_generation_++;
        s.node = Node{};
        s.node.id = NodeId{static_cast<SlotIndex>(i), s.generation};
        s.node.type_name = std::string{type_name};
        s.node.name = std::string{name};
        id = s.node.id;
        ++live_nodes_;
        ++version_;
        return Result<NodeId>{id};
    }
    {
        NodeSlot s{};
        s.occupied = true;
        s.generation = next_generation_++;
        s.node.type_name = std::string{type_name};
        s.node.name = std::string{name};
        s.node.id = NodeId{static_cast<SlotIndex>(slots_.size()), s.generation};
        slots_.push_back(std::move(s));
    }
    ++live_nodes_;
    ++version_;
    return Result<NodeId>{slots_.back().node.id};
}

Result<NodeId> Graph::reserve_node() {
    // An empty type name means pending. The only difference from add_node is **no type-name validation** --
    // reserving means exactly "I want to hold an id; I will tell you the type later".
    for (std::size_t i = 1; i < slots_.size(); ++i) {
        NodeSlot& s = slots_[i];
        if (s.occupied) continue;
        s.occupied = true;
        s.generation = next_generation_++;
        s.node = Node{};
        s.node.id = NodeId{static_cast<SlotIndex>(i), s.generation};
        ++live_nodes_;
        ++pending_nodes_;
        ++version_;
        return Result<NodeId>{s.node.id};
    }
    {
        NodeSlot s{};
        s.occupied = true;
        s.generation = next_generation_++;
        s.node.id = NodeId{static_cast<SlotIndex>(slots_.size()), s.generation};
        slots_.push_back(std::move(s));
    }
    ++live_nodes_;
    ++pending_nodes_;
    ++version_;
    return Result<NodeId>{slots_.back().node.id};
}

Result<void> Graph::fill_reserved(NodeId id, std::string_view type_name) {
    if (type_name.empty()) {
        return Result<void>{ErrorCode::invalid_argument};
    }
    Node* n = node_at(id);
    if (n == nullptr) {
        return Result<void>{ErrorCode::unknown_node};
    }
    if (!n->type_name.empty()) {
        // Already filled in once: a repeated fill is the caller's error
        return Result<void>{ErrorCode::duplicate_connection};
    }
    n->type_name = std::string{type_name};
    --pending_nodes_;
    ++version_;
    return Result<void>{};
}

Result<void> Graph::remove_node(NodeId id) {
    Node* n = node_at(id);
    if (n == nullptr) {
        return Result<void>{ErrorCode::unknown_node};
    }
    const bool was_pending = n->type_name.empty();

    // Delete the related edges first (the apply phase cannot fail)
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [id](const Edge& e) {
                                    return e.from.node == id || e.to.node == id;
                                }),
                 edges_.end());

    NodeSlot& s = slots_[id.index];
    s.node = Node{};
    s.occupied = false;
    // **The generation is bumped on release too**.
    //
    // Both uses of the generation require this step:
    //   1. An old handle never comes back to life -- it records the generation of the **previous** occupant;
    //   2. Undoing a "delete" must restore the node to **the same id**, and restore_node
    //      decides whether that id is still usable by comparing "slot generation vs target generation".
    //      Without a bump on release, the slot generation would equal the deleted node's generation,
    //      and restore_node's ABA check would report one **legal** restore as a conflict.
    // Caught by graph.mutate.undo_redo_roundtrip:
    // undo followed by redo failed with duplicate_connection.
    ++s.generation;
    if (next_generation_ <= s.generation) {
        next_generation_ = s.generation + 1;
    }
    --live_nodes_;
    if (was_pending) --pending_nodes_;
    ++version_;
    return Result<void>{};
}

Result<void> Graph::set_node_name(NodeId id, std::string_view name) {
    Node* n = node_at(id);
    if (n == nullptr) {
        return Result<void>{ErrorCode::unknown_node};
    }
    if (!name.empty()) {
        const NodeId existing = find_node_by_name(name);
        if (existing.valid() && existing != id) {
            return Result<void>{ErrorCode::duplicate_connection};
        }
    }
    n->name = std::string{name};
    ++version_;
    return Result<void>{};
}

NodeId Graph::find_node_by_name(std::string_view name) const noexcept {
    if (name.empty()) return NodeId{};
    for (const auto& s : slots_) {
        if (s.occupied && s.node.name == name) return s.node.id;
    }
    return NodeId{};
}

// -- Edges -------------------------------------------------------------------

bool Graph::reaches(NodeId from, NodeId target) const noexcept {
    if (from == target) return true;
    // Depth-first, walking along the edge direction. Graphs are small (usually < 100 nodes), so no cleverer algorithm is needed.
    std::vector<NodeId> stack{from};
    std::vector<NodeId> seen;
    while (!stack.empty()) {
        const NodeId cur = stack.back();
        stack.pop_back();
        if (std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back(cur);
        for (const auto& e : edges_) {
            if (e.from.node != cur) continue;
            if (e.to.node == target) return true;
            stack.push_back(e.to.node);
        }
    }
    return false;
}

Result<void> Graph::connect(PortRef from, PortRef to) {
    // -- 1. Handles are valid --
    if (!from.valid() || !to.valid()) {
        return Result<void>{ErrorCode::unknown_node};
    }
    if (node_at(from.node) == nullptr || node_at(to.node) == nullptr) {
        return Result<void>{ErrorCode::unknown_node};
    }

    // -- 2. Direction --
    if (from.direction != PortDirection::output || to.direction != PortDirection::input) {
        return Result<void>{ErrorCode::invalid_argument};
    }

    // -- 3. At most one incoming edge per input port --
    for (const auto& e : edges_) {
        if (e.to == to) {
            return Result<void>{ErrorCode::duplicate_connection};
        }
    }

    // -- 4. Self-loop --
    if (from.node == to.node) {
        return Result<void>{ErrorCode::cycle_detected};
    }

    // -- 5. Cycle check: if to.node already reaches from.node, this edge would close a cycle --
    if (reaches(to.node, from.node)) {
        return Result<void>{ErrorCode::cycle_detected};
    }

    // -- Apply (no failure possible from here on) --
    edges_.push_back(Edge{from, to});
    ++version_;
    return Result<void>{};
}

Result<void> Graph::disconnect(PortRef input) {
    if (input.direction != PortDirection::input) {
        return Result<void>{ErrorCode::invalid_argument};
    }
    const auto it = std::find_if(edges_.begin(), edges_.end(),
                                 [&input](const Edge& e) { return e.to == input; });
    if (it == edges_.end()) {
        return Result<void>{ErrorCode::not_connected};
    }
    edges_.erase(it);
    ++version_;
    return Result<void>{};
}

const Edge* Graph::incoming(PortRef input) const noexcept {
    for (const auto& e : edges_) {
        if (e.to == input) return &e;
    }
    return nullptr;
}

Result<void> Graph::restore_node(const Node& snapshot) {
    if (!snapshot.id.valid()) {
        return Result<void>{ErrorCode::invalid_argument};
    }
    const SlotIndex idx = snapshot.id.index;

    // Grow the slot array far enough first
    if (idx >= slots_.size()) {
        slots_.resize(static_cast<std::size_t>(idx) + 1);
    }
    NodeSlot& s = slots_[idx];
    if (s.occupied) {
        // The slot is occupied. If the occupant's generation is exactly the target generation, this id
        // is already live in this slot -- a repeated restore is the caller's error.
        // If the occupant's generation is higher, it is a later node, and it must not be overwritten either.
        if (s.generation >= snapshot.id.generation) {
            return Result<void>{ErrorCode::duplicate_connection};
        }
        return Result<void>{ErrorCode::duplicate_connection};
    }

    // The slot is free: **no id owns it**, so restoring is safe.
    //
    // Generations are deliberately **not compared** here. The old form was `if (gen >= target) reject`,
    // which was wrong: deleting bumps the generation of the freed slot, so "undo a delete, then restore"
    // pushed its own slot generation above the target generation and was then rejected by its own check.
    // Generation comparison is meaningful only on the **occupied** path (who the occupant is);
    // on the free path there is no "other id" to speak of. Caught by
    // graph.mutate.undo_redo_roundtrip.
    s.occupied = true;
    s.generation = snapshot.id.generation;
    s.node = snapshot;
    // Defensive: the generation counter must always stay ahead of any allocated generation
    if (next_generation_ <= snapshot.id.generation) {
        next_generation_ = snapshot.id.generation + 1;
    }
    ++live_nodes_;
    if (snapshot.type_name.empty()) {
        ++pending_nodes_;   // what is restored may be a pending node too
    }
    ++version_;
    return Result<void>{};
}

Result<void> Graph::restore_edge(const Edge& e) {
    if (!e.valid()) {
        return Result<void>{ErrorCode::invalid_argument};
    }
    if (node_at(e.from.node) == nullptr || node_at(e.to.node) == nullptr) {
        return Result<void>{ErrorCode::unknown_node};
    }
    for (const auto& existing : edges_) {
        if (existing.to == e.to) {
            return Result<void>{ErrorCode::duplicate_connection};
        }
    }
    if (e.from.node == e.to.node || reaches(e.to.node, e.from.node)) {
        return Result<void>{ErrorCode::cycle_detected};
    }
    edges_.push_back(e);
    ++version_;
    return Result<void>{};
}

void Graph::clear() noexcept {
    slots_.clear();
    edges_.clear();
    live_nodes_ = 0;
    pending_nodes_ = 0;
    ++version_;
}

}  // namespace qp::graph
