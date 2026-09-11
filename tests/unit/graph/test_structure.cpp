/**
 * @file test_structure.cpp
 * @brief Unit and property tests for core/graph/structure.
 *
 * The emphasis is on **failure branches**: every mutation must verify that "after a failure the
 * graph and the version are both unchanged". The undo stack depends on it; the contract says @post.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/structure.hpp>

#include <set>
#include <string>

using namespace qp::graph;
using qp::diag::ErrorCode;

namespace {

/// @brief Build an n1 -> n2 -> n3 chain and return the three handles.
struct Chain {
    Graph g;
    NodeId a{};
    NodeId b{};
    NodeId c{};
};

Chain make_chain() {
    Chain ch;
    ch.a = ch.g.add_node("source").value();
    ch.b = ch.g.add_node("gain").value();
    ch.c = ch.g.add_node("sink").value();
    REQUIRE(ch.g.connect(PortRef{ch.a, 1, PortDirection::output},
                         PortRef{ch.b, 1, PortDirection::input}));
    REQUIRE(ch.g.connect(PortRef{ch.b, 1, PortDirection::output},
                         PortRef{ch.c, 1, PortDirection::input}));
    return ch;
}

}  // namespace

// ===========================================================================
// Nodes
// ===========================================================================

TEST_CASE("graph.structure.empty_graph", "[graph][structure]") {
    const Graph g;
    REQUIRE(g.node_count() == 0);
    REQUIRE(g.edge_count() == 0);
    REQUIRE(g.slot_count() == 0);
    REQUIRE(g.version() == 1);
    REQUIRE_FALSE(g.has_node(NodeId{0, 1}));
    REQUIRE(g.find_node(NodeId{5, 5}) == nullptr);
}

TEST_CASE("graph.structure.add_node", "[graph][structure]") {
    Graph g;
    const GraphVersion v0 = g.version();

    const auto r = g.add_node("dipole");
    REQUIRE(r);
    const NodeId id = r.value();
    REQUIRE(id.valid());
    REQUIRE(g.node_count() == 1);
    REQUIRE(g.version() > v0);          // a successful mutation bumps the version

    const Node* n = g.find_node(id);
    REQUIRE(n != nullptr);
    REQUIRE(n->type_name == "dipole");
    REQUIRE(n->id == id);
    REQUIRE(n->name.empty());
}

TEST_CASE("graph.structure.add_node_uses_fresh_generation", "[graph][structure]") {
    // A reused slot must have a different generation, or an old handle would "come back"
    // as a new node and the undo stack / cache keys would silently point at the wrong thing.
    Graph g;
    const NodeId first = g.add_node("a").value();
    REQUIRE(g.remove_node(first));

    const NodeId second = g.add_node("b").value();
    REQUIRE(second.index == first.index);              // the same slot was reused
    REQUIRE(second.generation != first.generation);    // but the generation differs
    REQUIRE(g.find_node(first) == nullptr);            // the old handle is dead
    REQUIRE(g.find_node(second) != nullptr);
    REQUIRE_FALSE(g.has_node(first));
}

TEST_CASE("graph.structure.remove_node_invalidates_handle", "[graph][structure]") {
    Graph g;
    const NodeId id = g.add_node("x").value();
    const GraphVersion before_remove = g.version();

    REQUIRE(g.remove_node(id));
    REQUIRE(g.node_count() == 0);
    REQUIRE(g.version() > before_remove);
    REQUIRE(g.find_node(id) == nullptr);

    // Removing twice: fails and does not change the graph
    const GraphVersion v = g.version();
    const auto again = g.remove_node(id);
    REQUIRE_FALSE(again);
    REQUIRE(again.error() == ErrorCode::unknown_node);
    REQUIRE(g.version() == v);
}

TEST_CASE("graph.structure.remove_node_drops_edges", "[graph][structure]") {
    Chain ch = make_chain();
    REQUIRE(ch.g.edge_count() == 2);

    REQUIRE(ch.g.remove_node(ch.b));       // delete the middle node
    REQUIRE(ch.g.node_count() == 2);
    // Both edges touch b, so they must disappear too -- otherwise a dangling edge would remain
    REQUIRE(ch.g.edge_count() == 0);
}

TEST_CASE("graph.structure.node_count_tracks_slots", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const NodeId c = g.add_node("c").value();
    REQUIRE(g.node_count() == 3);
    REQUIRE(g.slot_count() == 3);

    REQUIRE(g.remove_node(b));
    REQUIRE(g.node_count() == 2);
    REQUIRE(g.slot_count() == 3);       // the empty slot is kept (other nodes are not moved)

    // A new node reuses the empty slot
    const NodeId d = g.add_node("d").value();
    REQUIRE(g.node_count() == 3);
    REQUIRE(g.slot_count() == 3);
    REQUIRE(d.index == b.index);
    // Other handles are unaffected
    REQUIRE(g.has_node(a));
    REQUIRE(g.has_node(c));
    REQUIRE(g.has_node(d));
}

TEST_CASE("graph.structure.find_node_by_user_name", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node_named("spring", "spring_1").value();
    const NodeId b = g.add_node_named("spring", "spring_2").value();
    REQUIRE(a != b);
    REQUIRE(g.find_node(a)->name == "spring_1");
    REQUIRE(g.find_node(b)->name == "spring_2");

    REQUIRE(g.find_node_by_name("spring_1") == a);
    REQUIRE(g.find_node_by_name("spring_2") == b);
    REQUIRE_FALSE(g.find_node_by_name("nope").valid());
    REQUIRE_FALSE(g.find_node_by_name("").valid());

    // Names must be unique, or "spring_1" in a report is ambiguous
    const auto dup = g.add_node_named("spring", "spring_1");
    REQUIRE_FALSE(dup);
    REQUIRE(dup.error() == ErrorCode::duplicate_connection);
}

// ===========================================================================
// Edges
// ===========================================================================

TEST_CASE("graph.structure.connect_and_lookup", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("source").value();
    const NodeId b = g.add_node("sink").value();

    const PortRef out{a, 1, PortDirection::output};
    const PortRef in{b, 1, PortDirection::input};
    REQUIRE(g.connect(out, in));
    REQUIRE(g.edge_count() == 1);

    const Edge* e = g.incoming(in);
    REQUIRE(e != nullptr);
    REQUIRE(e->from == out);
    REQUIRE(e->to == in);

    REQUIRE(g.edges().size() == 1);
}

TEST_CASE("graph.structure.incoming_lookup_by_input", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const NodeId c = g.add_node("c").value();

    REQUIRE(g.connect(PortRef{a, 1, PortDirection::output},
                      PortRef{b, 1, PortDirection::input}));
    REQUIRE(g.connect(PortRef{c, 1, PortDirection::output},
                      PortRef{b, 2, PortDirection::input}));

    // Different input ports of one node each have their own incoming edge
    const Edge* e1 = g.incoming(PortRef{b, 1, PortDirection::input});
    const Edge* e2 = g.incoming(PortRef{b, 2, PortDirection::input});
    REQUIRE(e1 != nullptr);
    REQUIRE(e2 != nullptr);
    REQUIRE(e1->from.node == a);
    REQUIRE(e2->from.node == c);

    // A port with no incoming edge returns nullptr
    REQUIRE(g.incoming(PortRef{b, 3, PortDirection::input}) == nullptr);
    REQUIRE(g.incoming(PortRef{a, 1, PortDirection::input}) == nullptr);
    // An output port is not a key for "incoming edge lookup"
    REQUIRE(g.incoming(PortRef{a, 1, PortDirection::output}) == nullptr);
}

TEST_CASE("graph.structure.connect_rejects_duplicate_input", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const NodeId c = g.add_node("c").value();
    const PortRef in{b, 1, PortDirection::input};

    REQUIRE(g.connect(PortRef{a, 1, PortDirection::output}, in));

    // One input port takes at most one incoming edge. Hiding that with "last connect wins"
    // would produce hard-to-reproduce result differences when a student reorders wires.
    const GraphVersion v = g.version();
    const auto r = g.connect(PortRef{c, 1, PortDirection::output}, in);
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::duplicate_connection);
    REQUIRE(g.version() == v);          // a failure does not touch the version
    REQUIRE(g.edge_count() == 1);
    REQUIRE(g.incoming(in)->from.node == a);   // the original edge is still there
}

TEST_CASE("graph.structure.connect_rejects_unknown_node", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();

    // An invalid handle
    REQUIRE_FALSE(g.connect(PortRef{}, PortRef{a, 1, PortDirection::input}));
    REQUIRE(g.connect(PortRef{}, PortRef{a, 1, PortDirection::input}).error() ==
            ErrorCode::unknown_node);

    // A deleted handle (generation mismatch)
    const NodeId dead = g.add_node("dead").value();
    REQUIRE(g.remove_node(dead));

    // Take the snapshot **after every successful mutation**: both add/remove_node bump it
    const GraphVersion v = g.version();

    const auto r = g.connect(PortRef{dead, 1, PortDirection::output},
                             PortRef{a, 1, PortDirection::input});
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::unknown_node);

    REQUIRE(g.version() == v);
    REQUIRE(g.edge_count() == 0);
}

TEST_CASE("graph.structure.connect_rejects_direction", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const GraphVersion v = g.version();

    // input -> output is reversed
    REQUIRE_FALSE(g.connect(PortRef{a, 1, PortDirection::input},
                            PortRef{b, 1, PortDirection::output}));
    // input -> input
    REQUIRE_FALSE(g.connect(PortRef{a, 1, PortDirection::input},
                            PortRef{b, 1, PortDirection::input}));
    // output -> output
    REQUIRE_FALSE(g.connect(PortRef{a, 1, PortDirection::output},
                            PortRef{b, 1, PortDirection::output}));

    REQUIRE(g.version() == v);
    REQUIRE(g.edge_count() == 0);
}

TEST_CASE("graph.structure.connect_rejects_self_loop", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const GraphVersion v = g.version();

    const auto r = g.connect(PortRef{a, 1, PortDirection::output},
                             PortRef{a, 1, PortDirection::input});
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::cycle_detected);
    REQUIRE(g.version() == v);
}

TEST_CASE("graph.structure.connect_rejects_cycle", "[graph][structure]") {
    Chain ch = make_chain();   // a -> b -> c

    // c -> a would close a cycle
    const GraphVersion v = ch.g.version();
    const auto r = ch.g.connect(PortRef{ch.c, 1, PortDirection::output},
                                PortRef{ch.a, 2, PortDirection::input});
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::cycle_detected);
    REQUIRE(ch.g.version() == v);
    REQUIRE(ch.g.edge_count() == 2);

    // A longer cycle: c -> a(2), then a(2) -> b(2)?
    // c->a was already rejected, so check an indirect cycle: try another a -> c candidate
    const auto r2 = ch.g.connect(PortRef{ch.c, 1, PortDirection::output},
                                 PortRef{ch.a, 1, PortDirection::input});
    REQUIRE_FALSE(r2);
    REQUIRE(r2.error() == ErrorCode::cycle_detected);
}

TEST_CASE("graph.structure.disconnect_removes_edge", "[graph][structure]") {
    Chain ch = make_chain();
    const PortRef mid_in{ch.b, 1, PortDirection::input};
    REQUIRE(ch.g.incoming(mid_in) != nullptr);

    const GraphVersion before = ch.g.version();
    REQUIRE(ch.g.disconnect(mid_in));
    REQUIRE(ch.g.edge_count() == 1);
    REQUIRE(ch.g.version() > before);
    REQUIRE(ch.g.incoming(mid_in) == nullptr);

    // Disconnect again: fails and does not touch the graph
    const GraphVersion v = ch.g.version();
    const auto again = ch.g.disconnect(mid_in);
    REQUIRE_FALSE(again);
    REQUIRE(again.error() == ErrorCode::not_connected);
    REQUIRE(ch.g.version() == v);

    // Calling disconnect on an output port is an argument error
    const auto wrong = ch.g.disconnect(PortRef{ch.a, 1, PortDirection::output});
    REQUIRE_FALSE(wrong);
    REQUIRE(wrong.error() == ErrorCode::invalid_argument);
}

// ===========================================================================
// Version and the strong exception guarantee
// ===========================================================================

TEST_CASE("graph.structure.version_bumps_on_success_only", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();

    const GraphVersion v0 = g.version();
    // A failed operation does not touch the version
    REQUIRE_FALSE(g.remove_node(NodeId{999, 1}));
    REQUIRE(g.version() == v0);
    REQUIRE_FALSE(g.connect(PortRef{}, PortRef{a, 1, PortDirection::input}));
    REQUIRE(g.version() == v0);
    REQUIRE_FALSE(g.add_node(""));   // empty type name
    REQUIRE(g.version() == v0);

    // A successful operation bumps it
    REQUIRE(g.add_node("b"));
    REQUIRE(g.version() > v0);
}

TEST_CASE("graph.structure.failed_mutation_is_noop", "[graph][structure]") {
    // The strong guarantee underpins the undo stack: callers must be able to reason about
    Chain ch = make_chain();

    struct Snapshot {
        std::size_t nodes;
        std::size_t edges;
        GraphVersion version;
        std::size_t slots;
    };
    const Snapshot snap{ch.g.node_count(), ch.g.edge_count(), ch.g.version(),
                        ch.g.slot_count()};

    // A batch of mutations that must all fail
    REQUIRE_FALSE(ch.g.remove_node(NodeId{999, 1}));
    REQUIRE_FALSE(ch.g.connect(PortRef{ch.c, 1, PortDirection::output},
                               PortRef{ch.a, 1, PortDirection::input}));   // would close a cycle
    REQUIRE_FALSE(ch.g.disconnect(PortRef{ch.c, 5, PortDirection::input})); // no incoming edge
    REQUIRE_FALSE(ch.g.add_node_named("", "empty_type"));                   // empty type name
    // Duplicate name: add one successfully first, then adding the same name must fail
    REQUIRE(ch.g.add_node_named("t", "same"));
    const Snapshot snap2{ch.g.node_count(), ch.g.edge_count(), ch.g.version(),
                         ch.g.slot_count()};
    REQUIRE_FALSE(ch.g.add_node_named("t", "same"));   // duplicate name
    REQUIRE(ch.g.node_count() == snap2.nodes);
    REQUIRE(ch.g.edge_count() == snap2.edges);
    REQUIRE(ch.g.version() == snap2.version);
    REQUIRE(ch.g.slot_count() == snap2.slots);

    // Back to the original snapshot for an overall check.
    // Note: one node was added in between, so nodes and slots are +1 and edges unchanged.
    REQUIRE(ch.g.edge_count() == snap.edges);
    REQUIRE(ch.g.node_count() == snap.nodes + 1);
    REQUIRE(ch.g.slot_count() == snap.slots + 1);
}

// ===========================================================================
// Determinism and scale
// ===========================================================================

TEST_CASE("graph.structure.deterministic_iteration_order", "[graph][structure]") {
    // Iteration order must be stable: UI refresh, serialization and cache keys rely on it.
    Graph g;
    std::vector<NodeId> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(g.add_node("n").value());
    }
    REQUIRE(g.slot_count() == 5);
    // slots() includes the sentinel slot at index 0, so it is one longer than slot_count
    REQUIRE(g.slots().size() == g.slot_count() + 1);
    REQUIRE_FALSE(g.slots()[0].occupied);          // the sentinel is never occupied
    for (std::size_t i = 0; i < ids.size(); ++i) {
        REQUIRE(g.slots()[i + 1].node.id == ids[i]);   // starts at index 1
        REQUIRE(g.slots()[i + 1].occupied);
    }
}

TEST_CASE("graph.structure.no_cycle_after_many_connects", "[graph][structure]") {
    // Build a long chain, then try every backward edge -- all of them must be rejected
    Graph g;
    std::vector<NodeId> chain;
    constexpr int kN = 12;
    for (int i = 0; i < kN; ++i) {
        chain.push_back(g.add_node("n").value());
    }
    for (int i = 0; i + 1 < kN; ++i) {
        REQUIRE(g.connect(PortRef{chain[i], 1, PortDirection::output},
                          PortRef{chain[i + 1], 1, PortDirection::input}));
    }
    REQUIRE(g.edge_count() == static_cast<std::size_t>(kN - 1));

    // Any edge with i > j would close a cycle
    int rejected = 0;
    for (int i = 1; i < kN; ++i) {
        for (int j = 0; j < i; ++j) {
            const auto r = g.connect(PortRef{chain[i], 2, PortDirection::output},
                                     PortRef{chain[j], 2, PortDirection::input});
            REQUIRE_FALSE(r);
            REQUIRE(r.error() == ErrorCode::cycle_detected);
            ++rejected;
        }
    }
    REQUIRE(rejected == kN * (kN - 1) / 2);
    REQUIRE(g.edge_count() == static_cast<std::size_t>(kN - 1));   // not one of them was added
}

TEST_CASE("graph.structure.clear_resets", "[graph][structure]") {
    Chain ch = make_chain();
    const GraphVersion v = ch.g.version();
    ch.g.clear();
    REQUIRE(ch.g.node_count() == 0);
    REQUIRE(ch.g.edge_count() == 0);
    REQUIRE(ch.g.slot_count() == 0);
    REQUIRE(ch.g.version() > v);        // clearing is a mutation too
    REQUIRE_FALSE(ch.g.has_node(ch.a));
}

// ===========================================================================
// Reserve / restore / version (the primitives the command bus relies on)
// ===========================================================================

TEST_CASE("graph.structure.reserve_and_fill", "[graph][structure]") {
    Graph g;
    REQUIRE(g.is_stable());
    REQUIRE(g.pending_count() == 0);

    const auto reserved = g.reserve_node();
    REQUIRE(reserved);
    const NodeId id = reserved.value();
    REQUIRE(id.valid());
    REQUIRE(g.has_node(id));              // after reserving, the handle is **usable at once**
    REQUIRE(g.find_node(id)->type_name.empty());
    REQUIRE_FALSE(g.is_stable());         // but the graph is in the pending state
    REQUIRE(g.pending_count() == 1);

    REQUIRE(g.fill_reserved(id, "dipole"));
    REQUIRE(g.is_stable());
    REQUIRE(g.pending_count() == 0);
    REQUIRE(g.find_node(id)->type_name == "dipole");
    REQUIRE(g.find_node(id)->id == id);   // the id does not change
}

TEST_CASE("graph.structure.reserve_marks_pending", "[graph][structure]") {
    Graph g;
    const NodeId a = g.reserve_node().value();
    const NodeId b = g.reserve_node().value();
    REQUIRE(g.pending_count() == 2);
    REQUIRE_FALSE(g.is_stable());
    REQUIRE(g.is_pending(a));
    REQUIRE(g.is_pending(b));

    REQUIRE(g.fill_reserved(a, "x"));
    REQUIRE(g.pending_count() == 1);
    REQUIRE_FALSE(g.is_pending(a));
    REQUIRE(g.is_pending(b));

    // Deleting a pending node must decrement the count too, or the graph never becomes stable
    REQUIRE(g.remove_node(b));
    REQUIRE(g.pending_count() == 0);
    REQUIRE(g.is_stable());

    // A handle that is not pending
    REQUIRE_FALSE(g.is_pending(NodeId{999, 1}));
    REQUIRE_FALSE(g.is_pending(NodeId{}));
}

TEST_CASE("graph.structure.fill_rejects_non_pending", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("dipole").value();

    // A slot that has already been filled cannot be filled again
    const auto twice = g.fill_reserved(a, "other");
    REQUIRE_FALSE(twice);
    REQUIRE(twice.error() == ErrorCode::duplicate_connection);

    // An empty type name is not a legal commit
    const NodeId p = g.reserve_node().value();
    const auto empty = g.fill_reserved(p, "");
    REQUIRE_FALSE(empty);
    REQUIRE(empty.error() == ErrorCode::invalid_argument);

    // An unknown handle
    const auto unknown = g.fill_reserved(NodeId{999, 1}, "x");
    REQUIRE_FALSE(unknown);
    REQUIRE(unknown.error() == ErrorCode::unknown_node);
}

TEST_CASE("graph.structure.is_pending_predicate", "[graph][structure]") {
    Graph g;
    const NodeId real = g.add_node("dipole").value();
    const NodeId pending = g.reserve_node().value();

    REQUIRE_FALSE(g.is_pending(real));
    REQUIRE(g.is_pending(pending));
    // Implication: pending => exists
    REQUIRE(g.has_node(pending));
    REQUIRE_FALSE(g.is_pending(NodeId{}));
    REQUIRE_FALSE(g.is_pending(NodeId{12345, 1}));
}

TEST_CASE("graph.structure.restore_node_preserves_id", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node_named("dipole", "d1").value();
    g.find_node_mutable(a)->set_param(1, qp::ports::Value{2.5});
    const Node snapshot = *g.find_node(a);

    REQUIRE(g.remove_node(a));
    REQUIRE_FALSE(g.has_node(a));

    // Restoring must make **the same id** valid again -- otherwise, after undo and redo,
    // every edge and cache key referring to that node becomes invalid
    REQUIRE(g.restore_node(snapshot));
    REQUIRE(g.has_node(a));
    REQUIRE(g.find_node(a)->id == a);
    REQUIRE(g.find_node(a)->type_name == "dipole");
    REQUIRE(g.find_node(a)->name == "d1");
    REQUIRE(g.find_node(a)->param(1).as_f64() == 2.5);

    // Restoring into a live position is a conflict
    const auto again = g.restore_node(snapshot);
    REQUIRE_FALSE(again);
    REQUIRE(again.error() == ErrorCode::duplicate_connection);

    // An invalid handle
    Node bad{};
    REQUIRE(g.restore_node(bad).error() == ErrorCode::invalid_argument);
}

TEST_CASE("graph.structure.restore_edge_after_restore_node", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("src").value();
    const NodeId b = g.add_node("dst").value();
    const Edge e{PortRef{a, 1, PortDirection::output}, PortRef{b, 1, PortDirection::input}};
    REQUIRE(g.connect(e.from, e.to));
    REQUIRE(g.edge_count() == 1);

    REQUIRE(g.remove_node(a));
    REQUIRE(g.edge_count() == 0);

    // The edge cannot be restored while its node is still missing
    REQUIRE(g.restore_edge(e).error() == ErrorCode::unknown_node);

    Node snapshot = *g.find_node(b);   // a placeholder to avoid an unused variable
    (void)snapshot;
    // Restore a's contents first (using the snapshot taken before the deletion)
    Graph g2;
    const NodeId a2 = g2.add_node("src").value();
    const NodeId b2 = g2.add_node("dst").value();
    const Edge e2{PortRef{a2, 1, PortDirection::output}, PortRef{b2, 1, PortDirection::input}};
    REQUIRE(g2.connect(e2.from, e2.to));
    const Node snap_a = *g2.find_node(a2);
    REQUIRE(g2.remove_node(a2));
    REQUIRE(g2.restore_node(snap_a));
    REQUIRE(g2.restore_edge(e2));
    REQUIRE(g2.edge_count() == 1);
    REQUIRE(g2.incoming(e2.to) != nullptr);

    // Restoring the same edge a second time is a conflict
    REQUIRE(g2.restore_edge(e2).error() == ErrorCode::duplicate_connection);
    // An edge with the wrong direction
    const Edge reversed{PortRef{a2, 1, PortDirection::input},
                        PortRef{b2, 1, PortDirection::output}};
    REQUIRE(g2.restore_edge(reversed).error() == ErrorCode::invalid_argument);
}

TEST_CASE("graph.structure.bump_version_is_monotonic", "[graph][structure]") {
    // When a node's contents are modified in place (find_node_mutable) the caller must bump the
    // version explicitly, or the cache would return stale results.
    Graph g;
    const NodeId a = g.add_node("x").value();
    const GraphVersion v0 = g.version();
    g.bump_version();
    REQUIRE(g.version() > v0);
    g.bump_version();
    REQUIRE(g.version() > v0 + 1);
}

TEST_CASE("graph.structure.set_node_name_enforces_uniqueness", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("t").value();
    const NodeId b = g.add_node("t").value();

    REQUIRE(g.set_node_name(a, "alpha"));
    REQUIRE(g.find_node(a)->name == "alpha");
    REQUIRE(g.find_node_by_name("alpha") == a);

    // A name collision must be rejected, and must not touch the graph
    const GraphVersion v = g.version();
    const auto dup = g.set_node_name(b, "alpha");
    REQUIRE_FALSE(dup);
    REQUIRE(dup.error() == ErrorCode::duplicate_connection);
    REQUIRE(g.version() == v);
    REQUIRE(g.find_node(b)->name.empty());

    // Setting a node's own name is idempotent
    REQUIRE(g.set_node_name(a, "alpha"));
    // Clearing the name is always allowed
    REQUIRE(g.set_node_name(a, ""));
    REQUIRE_FALSE(g.find_node_by_name("alpha").valid());

    // Unknown node
    REQUIRE(g.set_node_name(NodeId{999, 1}, "x").error() == ErrorCode::unknown_node);
}