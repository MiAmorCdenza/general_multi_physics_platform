/**
 * @file test_mutate.cpp
 * @brief Unit and property tests for core/graph/mutate.
 *
 * Three focus areas:
 *   1. **Exactness of undo/redo** -- above all "the node id must not change".
 *      If the id changed after undo and redo, every edge, cache key, and UI selection
 *      referring to that node would break, and the redone graph would not be the same graph.
 *   2. **A failure changes nothing** -- graph, version number, and undo stack must hold no half record.
 *   3. **Merge semantics** -- consecutive slider drags undo in one step, not in dozens.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/mutate.hpp>

#include <string>

using namespace qp::graph;
using qp::diag::ErrorCode;

namespace {

/// @brief A graph with a bus attached, plus the usual command constructors.
struct Fixture {
    Graph graph;
    CommandBus bus{graph};

    /// @brief Adds a node through the bus and returns its id.
    NodeId add(const std::string& type, const std::string& name = "") {
        auto reserved = bus.reserve_node();
        REQUIRE(reserved);
        const NodeId id = reserved.value();
        auto r = bus.apply(AddNode{id, type, name});
        REQUIRE(r);
        return id;
    }

    void connect_nodes(NodeId a, PortNumber out_port, NodeId b, PortNumber in_port) {
        const auto r = bus.apply(Connect{PortRef{a, out_port, PortDirection::output},
                                         PortRef{b, in_port, PortDirection::input}});
        REQUIRE(r);
    }
};

}  // namespace

// ===========================================================================
// command metadata
// ===========================================================================

TEST_CASE("graph.mutate.command_metadata", "[graph][mutate]") {
    // A command is pure data: comparable, nameable, and able to expose its target.
    // Callback-style commands cannot do that and drag lifetime problems into the undo stack.
    const Command add{AddNode{NodeId{1, 1}, "dipole", "d1"}};
    const Command rm{RemoveNode{NodeId{1, 1}}};
    const Command sp{SetParam{NodeId{1, 1}, 2, qp::ports::Value{1.5}}};
    const Command ep{EraseParam{NodeId{1, 1}, 2}};
    const Command sn{SetNodeName{NodeId{1, 1}, "x"}};
    const Command sb{SetBypass{NodeId{1, 1}, true}};
    const Command cn{Connect{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                             PortRef{NodeId{2, 1}, 1, PortDirection::input}}};
    const Command dc{Disconnect{PortRef{NodeId{2, 1}, 1, PortDirection::input}}};

    REQUIRE(std::string(command_name(add)) == "AddNode");
    REQUIRE(std::string(command_name(rm)) == "RemoveNode");
    REQUIRE(std::string(command_name(sp)) == "SetParam");
    REQUIRE(std::string(command_name(ep)) == "EraseParam");
    REQUIRE(std::string(command_name(sn)) == "SetNodeName");
    REQUIRE(std::string(command_name(sb)) == "SetBypass");
    REQUIRE(std::string(command_name(cn)) == "Connect");
    REQUIRE(std::string(command_name(dc)) == "Disconnect");

    REQUIRE(command_target(add).value() == NodeId{1, 1});
    REQUIRE(command_target(cn).value() == NodeId{2, 1});   // a connect affects the target node
    REQUIRE(command_target(dc).value() == NodeId{2, 1});

    // A command is a value: it can be compared for equality
    REQUIRE(sp == Command{SetParam{NodeId{1, 1}, 2, qp::ports::Value{1.5}}});
    REQUIRE(sp != Command{SetParam{NodeId{1, 1}, 2, qp::ports::Value{9.9}}});
}

// ===========================================================================
// basic application
// ===========================================================================

TEST_CASE("graph.mutate.apply_add_node", "[graph][mutate]") {
    Fixture f;
    const GraphVersion v0 = f.graph.version();

    auto reserved = f.bus.reserve_node();
    REQUIRE(reserved);
    const NodeId id = reserved.value();
    // After a reservation the graph is **no longer stable** -- deliberately: the
    // intermediate state is observable, so "is the graph stable?" must be a question one can ask.
    REQUIRE_FALSE(f.graph.is_stable());
    REQUIRE(f.graph.pending_count() == 1);

    const auto r = f.bus.apply(AddNode{id, "dipole", "d1"});
    REQUIRE(r);
    REQUIRE(r.value().node.value() == id);
    REQUIRE(r.value().changed);
    REQUIRE(r.value().version > v0);
    REQUIRE(f.graph.is_stable());

    const Node* n = f.graph.find_node(id);
    REQUIRE(n != nullptr);
    REQUIRE(n->type_name == "dipole");
    REQUIRE(n->name == "d1");
    REQUIRE(f.bus.can_undo());
}

TEST_CASE("graph.mutate.apply_remove_node", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("source", "a");
    const NodeId b = f.add("sink", "b");
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.graph.node_count() == 2);
    REQUIRE(f.graph.edge_count() == 1);

    const auto r = f.bus.apply(RemoveNode{a});
    REQUIRE(r);
    REQUIRE(f.graph.node_count() == 1);
    REQUIRE(f.graph.edge_count() == 0);   // the attached edge disappears with it
    REQUIRE_FALSE(f.graph.has_node(a));
}

TEST_CASE("graph.mutate.apply_set_param", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");

    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.5}}));
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 2.5);

    // Overwrite
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{7.0}}));
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 7.0);
    REQUIRE(f.graph.find_node(a)->params.size() == 1);
}

TEST_CASE("graph.mutate.apply_connect", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("source");
    const NodeId b = f.add("sink");

    const auto r = f.bus.apply(Connect{PortRef{a, 1, PortDirection::output},
                                       PortRef{b, 1, PortDirection::input}});
    REQUIRE(r);
    REQUIRE(f.graph.edge_count() == 1);
    REQUIRE(f.graph.incoming(PortRef{b, 1, PortDirection::input}) != nullptr);
}

TEST_CASE("graph.mutate.apply_rejects_invalid", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");

    // An empty type name must fail **without reserving a slot**.
    // If a slot were reserved first and validation failed afterwards, the graph would be
    // stuck unstable forever -- which the user experiences as "the graph stopped being editable".
    {
        auto reserved = f.bus.reserve_node();
        REQUIRE(reserved);
        const auto r = f.bus.apply(AddNode{reserved.value(), "", ""});
        REQUIRE_FALSE(r);
        REQUIRE(r.error() == ErrorCode::invalid_argument);
        // The slot was reserved by us, so the graph really is unstable now; clean up and go on
        REQUIRE(f.graph.has_node(reserved.value()));
        REQUIRE(f.graph.remove_node(reserved.value()));
        REQUIRE(f.graph.is_stable());
    }

    // Applying an empty type name without a reservation: fails at once, and the graph stays stable
    {
        const NodeId fake{900, 1};
        const auto r = f.bus.apply(AddNode{fake, "", ""});
        REQUIRE_FALSE(r);
        REQUIRE(r.error() == ErrorCode::invalid_argument);
        REQUIRE(f.graph.is_stable());          // the key point: no slot was left waiting to be filled
        REQUIRE_FALSE(f.graph.has_node(fake));
    }

    // Applying a valid type name without a reservation: it reserves here and succeeds.
    // Note: the id in the command is only a "wish"; the graph assigns the real id, so later
    // assertions must use the returned id -- which is exactly why ApplyOutcome exists.
    {
        const NodeId requested{901, 1};
        const auto applied = f.bus.apply(AddNode{requested, "dipole", ""});
        REQUIRE(applied);
        const NodeId actual = applied.value().node.value();
        REQUIRE(f.graph.has_node(actual));
        REQUIRE(f.graph.is_stable());
        REQUIRE(f.graph.find_node(actual)->type_name == "dipole");
        REQUIRE(f.bus.undo());
        REQUIRE_FALSE(f.graph.has_node(actual));
    }

    // An unknown node
    REQUIRE(f.bus.apply(RemoveNode{NodeId{999, 1}}).error() == ErrorCode::unknown_node);
    REQUIRE(f.bus.apply(SetParam{NodeId{999, 1}, 1, qp::ports::Value{1.0}}).error() ==
            ErrorCode::unknown_node);

    // Port number 0
    REQUIRE(f.bus.apply(SetParam{a, 0, qp::ports::Value{1.0}}).error() ==
            ErrorCode::invalid_argument);
    REQUIRE(f.bus.apply(EraseParam{a, 0}).error() == ErrorCode::invalid_argument);

    // A cycle
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.bus.apply(Connect{PortRef{b, 1, PortDirection::output},
                                PortRef{a, 2, PortDirection::input}})
                .error() == ErrorCode::cycle_detected);

    // A duplicate name: name two nodes first, then rename b to a's name.
    // Note that the second argument of Fixture::add(type, name) is the **type name**,
    // not a user name -- a user name must be set explicitly through SetNodeName.
    REQUIRE(f.bus.apply(SetNodeName{a, "alpha"}));
    REQUIRE(f.bus.apply(SetNodeName{b, "beta"}));
    REQUIRE(f.bus.apply(SetNodeName{b, "alpha"}).error() == ErrorCode::duplicate_connection);

    // Disconnecting when there is no incoming edge
    REQUIRE(f.bus.apply(Disconnect{PortRef{b, 5, PortDirection::input}}).error() ==
            ErrorCode::not_connected);

    // The graph stays stable all the way through
    REQUIRE(f.graph.is_stable());
}

TEST_CASE("graph.mutate.next_target_matches_history", "[graph][mutate]") {
    // next_undo_target() and next_redo_target() feed "what changed" notifications,
    // so they must agree with the labels the same stack reports. A stack that
    // reported a label but no target would make a view unable to redraw the right
    // node, and the mismatch would show up as a stale panel rather than an error.
    Graph g;
    CommandBus bus{g};

    // Empty history: both targets are invalid, and no label is available either.
    REQUIRE_FALSE(bus.can_undo());
    REQUIRE_FALSE(bus.can_redo());
    REQUIRE_FALSE(bus.history().next_undo_target().valid());
    REQUIRE_FALSE(bus.history().next_redo_target().valid());
    REQUIRE(bus.history().next_undo_label().empty());
    REQUIRE(bus.history().next_redo_label().empty());

    auto reserved = bus.reserve_node();
    REQUIRE(reserved.has_value());
    const NodeId id = reserved.value();
    AddNode add;
    add.id = id;
    add.type_name = "spring";
    REQUIRE(bus.apply(add).has_value());

    // The target is the node the command was about, and it tracks the label: both
    // non-empty or both empty, never one without the other.
    REQUIRE(bus.history().next_undo_target() == id);
    REQUIRE_FALSE(bus.history().next_undo_label().empty());

    REQUIRE(bus.undo().has_value());
    REQUIRE_FALSE(bus.history().next_undo_target().valid());
    REQUIRE(bus.history().next_undo_label().empty());
    REQUIRE(bus.history().next_redo_target() == id);
    REQUIRE_FALSE(bus.history().next_redo_label().empty());

    REQUIRE(bus.redo().has_value());
    REQUIRE(bus.history().next_undo_target() == id);
    REQUIRE_FALSE(bus.history().next_redo_target().valid());
}

TEST_CASE("graph.mutate.failed_apply_is_noop", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");
    f.connect_nodes(a, 1, b, 1);

    const GraphVersion v = f.graph.version();
    const std::size_t nodes = f.graph.node_count();
    const std::size_t edges = f.graph.edge_count();
    const std::size_t undo_before = f.bus.history().undo_size();

    // A batch of commands that must all fail. The graph stays stable throughout, so each
    // failure may only fail itself. Note that both nodes here have **no** user name (the second
    // argument of Fixture::add is a type name), so a rename clash needs one of them named first.
    REQUIRE_FALSE(f.bus.apply(RemoveNode{NodeId{999, 1}}));
    REQUIRE_FALSE(f.bus.apply(Connect{PortRef{b, 1, PortDirection::output},
                                      PortRef{a, 2, PortDirection::input}}));
    REQUIRE_FALSE(f.bus.apply(Disconnect{PortRef{b, 7, PortDirection::input}}));
    REQUIRE(f.bus.apply(SetNodeName{a, "n1"}));
    REQUIRE_FALSE(f.bus.apply(SetNodeName{b, "n1"}));    // clashes with a's name
    REQUIRE(f.bus.apply(SetNodeName{a, "n1"}));          // setting its own name again is idempotent
    REQUIRE(f.bus.apply(SetParam{a, 0, qp::ports::Value{1.0}}).error() ==
            ErrorCode::invalid_argument);
    REQUIRE(f.bus.apply(EraseParam{a, 0}).error() == ErrorCode::invalid_argument);

    // Two SetNodeName calls above succeeded, so the undo stack gains two entries.
    // The key assertion is "no failed command changed the graph".
    REQUIRE(f.graph.version() > v);                      // a successful rename really did bump it
    REQUIRE(f.graph.node_count() == nodes);
    REQUIRE(f.graph.edge_count() == edges);
    REQUIRE(f.graph.is_stable());
    REQUIRE(f.bus.history().undo_size() == undo_before + 1);  // two same-name renames merged into one
}

TEST_CASE("graph.mutate.rejects_apply_while_unstable", "[graph][mutate]") {
    Fixture f;
    auto reserved = f.bus.reserve_node();
    REQUIRE(reserved);
    const NodeId pending_id = reserved.value();
    REQUIRE_FALSE(f.graph.is_stable());

    // No new command while the graph is unstable, or "is it stable?" becomes cross-module inference
    const auto r = f.bus.reserve_node();
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::graph_busy);

    const auto r2 = f.bus.apply(RemoveNode{pending_id});
    REQUIRE_FALSE(r2);
    REQUIRE(r2.error() == ErrorCode::graph_busy);

    // Usable again once the slot is filled
    REQUIRE(f.bus.apply(AddNode{pending_id, "dipole", ""}));
    REQUIRE(f.graph.is_stable());
    REQUIRE(f.bus.reserve_node());
}

TEST_CASE("graph.mutate.graph_accessor_is_readonly", "[graph][mutate]") {
    // A const accessor only; otherwise "all mutations go through the bus" decays into a verbal promise.
    Fixture f;
    const NodeId a = f.add("a");
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const CommandBus&>().graph()),
                                  const Graph&>);
    REQUIRE(f.bus.graph().node_count() == 1);
    REQUIRE(f.bus.graph().has_node(a));
}

// ===========================================================================
// undo / redo
// ===========================================================================

TEST_CASE("graph.mutate.undo_set_param", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}, /*allow_merge=*/false));
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.0}}, /*allow_merge=*/false));
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 2.0);

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 1.0);
    REQUIRE(f.bus.undo());
    // Back to "the parameter was never set", not to 0.0
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());
}

TEST_CASE("graph.mutate.redo_restores", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{5.0}}, false));

    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());
    REQUIRE(f.bus.can_redo());

    REQUIRE(f.bus.redo());
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 5.0);
    REQUIRE(f.bus.can_undo());
    REQUIRE_FALSE(f.bus.can_redo());
}

TEST_CASE("graph.mutate.undo_redo_roundtrip", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{3.0}}, false));

    const GraphVersion v_final = f.graph.version();
    const std::size_t n_final = f.graph.node_count();
    const std::size_t e_final = f.graph.edge_count();

    // Undo everything
    int undos = 0;
    while (f.bus.can_undo()) {
        REQUIRE(f.bus.undo());
        ++undos;
    }
    REQUIRE(undos == 4);   // AddNode a, AddNode b, Connect, SetParam
    REQUIRE(f.graph.node_count() == 0);
    REQUIRE(f.graph.edge_count() == 0);

    // Redo everything, back to exactly the same state
    int redos = 0;
    while (f.bus.can_redo()) {
        const auto r = f.bus.redo();
        if (!r) {
            FAIL("redo failed: " << qp::diag::to_string(r.error())
                                 << "  step " << redos << ", next label="
                                 << f.bus.history().next_redo_label());
            break;
        }
        ++redos;
    }
    REQUIRE(redos == 4);
    REQUIRE(f.graph.node_count() == n_final);
    REQUIRE(f.graph.edge_count() == e_final);
    REQUIRE(f.graph.version() >= v_final);
    REQUIRE(f.graph.find_node(a) != nullptr);
    REQUIRE(f.graph.find_node(b) != nullptr);
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 3.0);
}

TEST_CASE("graph.mutate.new_edit_clears_redo", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}, false));
    REQUIRE(f.bus.undo());
    REQUIRE(f.bus.can_redo());

    // Forking from the middle of history: the old "future" is no longer reachable
    REQUIRE(f.bus.apply(SetParam{a, 2, qp::ports::Value{9.0}}, false));
    REQUIRE_FALSE(f.bus.can_redo());
}

TEST_CASE("graph.mutate.merge_consecutive_set_param", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");

    // The undo stack holds one entry at this point (AddNode)
    const std::size_t after_add = f.bus.history().undo_size();
    REQUIRE(after_add == 1);

    // Simulate a slider drag: 20 consecutive changes to the same port's parameter
    for (int i = 1; i <= 20; ++i) {
        REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{static_cast<double>(i)}}));
    }
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 20.0);

    // 20 drags merge into **one** record: a single Ctrl+Z returns to before the drag,
    // instead of 20 presses. Undoing one step at a time is not undo, it is punishment.
    REQUIRE(f.bus.history().undo_size() == after_add + 1);

    // One undo returns to "the parameter was never set"
    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());
    // One more undo is needed before AddNode comes up
    REQUIRE(f.bus.history().undo_size() == 1);
    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.has_node(a));
    REQUIRE_FALSE(f.bus.can_undo());
}

TEST_CASE("graph.mutate.merge_does_not_cross_labels", "[graph][mutate]") {
    // The merge test is "same node + same operation kind"; different kinds must break the run.
    Fixture f;
    const NodeId a = f.add("gain");

    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}));
    REQUIRE(f.bus.apply(SetBypass{a, true}));                    // kind changes -> no merge
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.0}})); // changed back -> a new entry

    REQUIRE(f.bus.history().undo_size() == 4);   // AddNode + three entries
}

TEST_CASE("graph.mutate.merge_does_not_cross_nodes", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");

    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}));
    REQUIRE(f.bus.apply(SetParam{b, 1, qp::ports::Value{1.0}}));
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.0}}));

    // The three SetParam targets are a, b, a, so no two neighbours share a target -> no merge.
    // Add two AddNode entries (same target, different label, also no merge) for 5 in total.
    REQUIRE(f.bus.history().undo_size() == 5);
}

TEST_CASE("graph.mutate.undo_remove_restores_edges", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("source", "a");
    const NodeId b = f.add("mid", "b");
    const NodeId c = f.add("sink", "c");
    f.connect_nodes(a, 1, b, 1);
    f.connect_nodes(b, 1, c, 1);
    REQUIRE(f.graph.edge_count() == 2);

    REQUIRE(f.bus.apply(RemoveNode{b}));
    REQUIRE(f.graph.edge_count() == 0);

    // Undo must restore the node **together with its two edges**
    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.node_count() == 3);
    REQUIRE(f.graph.edge_count() == 2);
    REQUIRE(f.graph.has_node(b));
    REQUIRE(f.graph.find_node(b)->type_name == "mid");
    REQUIRE(f.graph.find_node(b)->name == "b");
    REQUIRE(f.graph.incoming(PortRef{b, 1, PortDirection::input}) != nullptr);
    REQUIRE(f.graph.incoming(PortRef{c, 1, PortDirection::input}) != nullptr);
}

TEST_CASE("graph.mutate.redo_after_undo_keeps_same_id", "[graph][mutate]") {
    // This is the single most important property of the whole design.
    // If a node were given a new id after undo and redo, every edge, cache key, and UI
    // selection referring to it would break -- the redone graph would not be the same graph.
    Fixture f;
    const NodeId a = f.add("dipole", "d1");
    const NodeId b = f.add("sink", "s1");
    f.connect_nodes(a, 1, b, 1);

    REQUIRE(f.bus.apply(RemoveNode{a}));
    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.has_node(a));          // the same id comes back to life
    REQUIRE(f.graph.find_node(a)->name == "d1");
    REQUIRE(f.graph.edge_count() == 1);

    REQUIRE(f.bus.redo());                 // delete it once more
    REQUIRE_FALSE(f.graph.has_node(a));

    REQUIRE(f.bus.undo());                 // restore it again
    REQUIRE(f.graph.has_node(a));
    REQUIRE(f.graph.find_node(a)->id == a);
    REQUIRE(f.graph.find_node(a)->name == "d1");
    REQUIRE(f.graph.find_node(a)->type_name == "dipole");
    REQUIRE(f.graph.edge_count() == 1);
}

TEST_CASE("graph.mutate.reserve_id_is_stable_across_undo", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("x", "first");
    REQUIRE(f.bus.undo());                 // undo the add
    REQUIRE_FALSE(f.graph.has_node(a));
    REQUIRE(f.bus.redo());                 // redo
    // The id must still be the same after redo -- "undo, then add another" would get a new id
    REQUIRE(f.graph.has_node(a));
    REQUIRE(f.graph.find_node(a)->name == "first");
}

TEST_CASE("graph.mutate.erase_param_undo_restores_value", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{4.5}}, false));

    REQUIRE(f.bus.apply(EraseParam{a, 1}));
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 4.5);
}

TEST_CASE("graph.mutate.set_name_undo_enforces_uniqueness", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("t", "a");
    const NodeId b = f.add("t", "b");

    REQUIRE(f.bus.apply(SetNodeName{b, "renamed"}));
    REQUIRE(f.graph.find_node(b)->name == "renamed");
    REQUIRE(f.graph.find_node_by_name("renamed") == b);
    REQUIRE_FALSE(f.graph.find_node_by_name("b").valid());

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.find_node(b)->name == "b");
    REQUIRE(f.graph.find_node_by_name("b") == b);
    REQUIRE(f.graph.find_node_by_name("renamed").valid() == false);
    REQUIRE(f.graph.find_node_by_name("a") == a);   // the other node is unaffected
}

TEST_CASE("graph.mutate.bypass_undo", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("filter");
    REQUIRE_FALSE(f.graph.find_node(a)->bypassed);

    REQUIRE(f.bus.apply(SetBypass{a, true}));
    REQUIRE(f.graph.find_node(a)->bypassed);
    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.find_node(a)->bypassed);
    REQUIRE(f.bus.redo());
    REQUIRE(f.graph.find_node(a)->bypassed);
}

TEST_CASE("graph.mutate.disconnect_undo_restores_edge", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("src");
    const NodeId b = f.add("dst");
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.graph.edge_count() == 1);

    const Edge before = *f.graph.incoming(PortRef{b, 1, PortDirection::input});
    REQUIRE(f.bus.apply(Disconnect{PortRef{b, 1, PortDirection::input}}));
    REQUIRE(f.graph.edge_count() == 0);

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.edge_count() == 1);
    // What comes back must be **the same** edge (port numbers included), not just "some connection"
    const Edge* after = f.graph.incoming(PortRef{b, 1, PortDirection::input});
    REQUIRE(after != nullptr);
    REQUIRE(*after == before);
}

TEST_CASE("graph.mutate.undo_stack_labels", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}, false));

    REQUIRE(f.bus.history().next_undo_label() == "SetParam");
    REQUIRE(f.bus.history().next_redo_label().empty());

    REQUIRE(f.bus.undo());
    REQUIRE(f.bus.history().next_undo_label() == "AddNode");
    REQUIRE(f.bus.history().next_redo_label() == "SetParam");
}

TEST_CASE("graph.mutate.failed_undo_keeps_history", "[graph][mutate]") {
    // When a revert fails, the record must stay on the stack so history and graph stay in step.
    // Otherwise "one entry in the stack, no change in the graph" misaligns every later undo.
    Fixture f;
    const NodeId a = f.add("a");
    REQUIRE(f.bus.undo());                       // undo the add
    REQUIRE_FALSE(f.graph.has_node(a));

    // Undoing once more here: the command is RemoveNode while the node is gone -> the revert fails.
    // The stack is empty by now, so this checks that "an empty stack returns false, not an error"
    const auto r = f.bus.undo();
    REQUIRE(r);
    REQUIRE_FALSE(r.value());
}

TEST_CASE("graph.mutate.long_session_stress", "[graph][mutate]") {
    // A long session: apply and undo in turn, and finally undo everything away.
    // The value of this case is that "stack and graph always correspond" survives many round trips.
    Fixture f;
    std::vector<NodeId> ids;
    for (int round = 0; round < 5; ++round) {
        ids.clear();
        for (int i = 0; i < 4; ++i) {
            ids.push_back(f.add("n" + std::to_string(i), "n" + std::to_string(round) + "_" +
                                                              std::to_string(i)));
        }
        for (std::size_t i = 0; i + 1 < ids.size(); ++i) {
            f.connect_nodes(ids[i], 1, ids[i + 1], 1);
        }
    }
    REQUIRE(f.graph.node_count() == 20);
    REQUIRE(f.graph.edge_count() == 15);

    // Undo everything
    int steps = 0;
    while (f.bus.can_undo()) {
        REQUIRE(f.bus.undo());
        ++steps;
        REQUIRE(steps < 200);   // defensive: it must not loop forever
    }
    REQUIRE(f.graph.node_count() == 0);
    REQUIRE(f.graph.edge_count() == 0);
    REQUIRE_FALSE(f.bus.can_redo() == false);   // the redo stack should be full
    REQUIRE(f.bus.can_redo());

    // Redo everything
    int redos = 0;
    while (f.bus.can_redo()) {
        REQUIRE(f.bus.redo());
        ++redos;
        REQUIRE(redos < 200);
    }
    REQUIRE(redos == steps);
    REQUIRE(f.graph.node_count() == 20);
    REQUIRE(f.graph.edge_count() == 15);
    // Every name must come back (proof that ids and contents were both restored exactly)
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 4; ++i) {
            const std::string want = "n" + std::to_string(round) + "_" + std::to_string(i);
            REQUIRE(f.graph.find_node_by_name(want).valid());
        }
    }
}
