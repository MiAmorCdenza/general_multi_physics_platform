/**
 * @file test_session.cpp
 * @brief Tests for the one editing session.
 *
 * Test case ids match the @tests fields in the commands headers byte for byte.
 *
 * The cases that matter are the ones where the session's guarantees are the only
 * thing standing between a user and a wrong experiment:
 *
 *   - a failed command notifies nobody, so a view cannot redraw into a state the
 *     graph never reached;
 *   - an idempotent command does not notify either, because a listener that
 *     redraws on every frame of a stationary slider drag turns a cheap edit into
 *     a busy loop;
 *   - undo and redo each notify exactly once, because a view that missed one
 *     would keep showing the state before the edit.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/authoring/commands.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace qp::authoring;
using namespace qp::graph;

namespace {

/// @brief Records every change it is told about, so counts can be asserted.
class RecordingListener final : public IChangeListener {
public:
    void on_change(const Change& change) noexcept override { changes.push_back(change); }

    [[nodiscard]] std::size_t count() const noexcept { return changes.size(); }
    [[nodiscard]] ChangeKind last_kind() const { return changes.back().kind; }

    std::vector<Change> changes;
};

/// @brief A listener that removes itself the first time it is notified.
class SelfRemovingListener final : public IChangeListener {
public:
    explicit SelfRemovingListener(Session& session) : session_(&session) {}

    void on_change(const Change&) noexcept override {
        ++calls;
        if (calls == 1) (void)session_->remove_listener(id);
    }

    void set_id(ListenerId i) { id = i; }

    Session* session_;
    ListenerId id{};
    int calls = 0;
};

/// @brief Adds a node named `type` to the session and returns its id.
NodeId add_node(Session& s, const std::string& type) {
    auto reserved = s.reserve_node();
    REQUIRE(reserved.has_value());
    AddNode cmd;
    cmd.id = reserved.value();
    cmd.type_name = type;
    REQUIRE(s.apply(cmd).has_value());
    return reserved.value();
}

}  // namespace

// ===========================================================================
// Vocabulary
// ===========================================================================

TEST_CASE("authoring.session.change_kinds", "[authoring]") {
    STATIC_REQUIRE(static_cast<std::uint8_t>(ChangeKind::node) == 0);
    STATIC_REQUIRE(std::string(to_string(ChangeKind::node)) == "node");
    STATIC_REQUIRE(std::string(to_string(ChangeKind::edge)) == "edge");
    STATIC_REQUIRE(std::string(to_string(ChangeKind::parameter)) == "parameter");
    STATIC_REQUIRE(std::string(to_string(ChangeKind::reset)) == "reset");

    // Every kind has a name, so a log line can never say "unknown" about a kind
    // the platform itself produced.
    for (const ChangeKind k : {ChangeKind::node, ChangeKind::edge, ChangeKind::parameter,
                               ChangeKind::reset}) {
        REQUIRE(std::string(to_string(k)) != "unknown");
    }
}

// ===========================================================================
// Applying commands
// ===========================================================================

TEST_CASE("authoring.session.apply_returns_command_result", "[authoring]") {
    Session session;
    REQUIRE(session.sequence() == 0);
    REQUIRE(session.graph().node_count() == 0);

    const NodeId id = add_node(session, "spring");
    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(session.sequence() == 1);

    // The graph is reachable, and the node is really there rather than merely
    // counted.
    const Node* node = session.graph().find_node(id);
    REQUIRE(node != nullptr);
    REQUIRE(node->type_name == "spring");

    // A failing command returns the bus's own error code and changes nothing.
    SetParam missing;
    missing.id = NodeId{999};           // never reserved
    missing.port = 1;                   // port 0 is the "no port" sentinel and is rejected outright
    const auto failed = session.apply(missing);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(session.sequence() == 1);
}

TEST_CASE("authoring.session.failed_apply_notifies_nobody", "[authoring]") {
    Session session;
    RecordingListener listener;
    const ListenerId lid = session.add_listener(listener);
    REQUIRE(lid.valid());
    REQUIRE(session.listener_count() == 1);

    const NodeId id = add_node(session, "spring");
    REQUIRE(listener.count() == 1);
    REQUIRE(listener.last_kind() == ChangeKind::node);
    REQUIRE(listener.changes.back().node == id);
    REQUIRE(listener.changes.back().sequence == 1);

    // A failed command must notify nobody. A viewer that redrew here would be
    // drawing a state the graph never reached.
    SetParam bad;
    bad.id = NodeId{999};
    bad.port = 1;
    REQUIRE_FALSE(session.apply(bad).has_value());
    REQUIRE(listener.count() == 1);
    REQUIRE(session.sequence() == 1);

    // Running the SAME command again is also silent. This is the case that
    // matters in practice: a UI that re-applies the current value on every
    // repaint, or a slider drag that is not moving. Without this, the graph
    // version bumps on every frame and every downstream cache entry is
    // invalidated for nothing.
    const std::size_t after_first = listener.count();
    SetParam same;
    same.id = id;
    same.port = 1;
    same.value = qp::ports::Value{1.5};
    REQUIRE(session.apply(same).has_value());   // first time: the value appears
    REQUIRE(listener.count() == after_first + 1);
    REQUIRE(session.apply(same).has_value());   // the same value again: no change
    REQUIRE(listener.count() == after_first + 1);
    REQUIRE(session.sequence() == after_first + 1);
}

TEST_CASE("authoring.session.change_kind_follows_the_command", "[authoring]") {
    Session session;
    RecordingListener listener;
    (void)session.add_listener(listener);

    const NodeId first = add_node(session, "spring");
    const NodeId second = add_node(session, "mass");
    REQUIRE(listener.changes.back().kind == ChangeKind::node);

    SetParam param;
    param.id = first;
    param.port = 1;
    param.value = qp::ports::Value{1.5};
    REQUIRE(session.apply(param).has_value());
    REQUIRE(listener.changes.back().kind == ChangeKind::parameter);
    REQUIRE(listener.changes.back().node == first);

    Connect connect;
    connect.from = PortRef{first, 1, PortDirection::output};
    connect.to = PortRef{second, 1, PortDirection::input};
    REQUIRE(session.apply(connect).has_value());
    REQUIRE(listener.changes.back().kind == ChangeKind::edge);

    // The version carried by the change is the graph's version after the change,
    // so a view can compare it against what it last drew and drop duplicates.
    REQUIRE(listener.changes.back().version == session.graph().version());
}

TEST_CASE("authoring.session.listeners_notified_once_per_change", "[authoring]") {
    Session session;
    RecordingListener first;
    RecordingListener second;
    (void)session.add_listener(first);
    (void)session.add_listener(second);
    REQUIRE(session.listener_count() == 2);

    (void)add_node(session, "spring");

    // Exactly once each, not once per registered listener in total and not twice
    // for one listener. A view that missed a notification would keep showing the
    // state before the edit, which is the defect this broadcast exists to
    // prevent.
    REQUIRE(first.count() == 1);
    REQUIRE(second.count() == 1);

    // Several changes in a row produce one notification each.
    (void)add_node(session, "mass");
    (void)add_node(session, "damper");
    REQUIRE(first.count() == 3);
    REQUIRE(second.count() == 3);

    // The sequence number is shared and increasing, so "has anything happened
    // since I last looked" is answerable.
    REQUIRE(first.changes.back().sequence == 3);
    REQUIRE(first.changes.back().sequence == second.changes.back().sequence);
}

TEST_CASE("authoring.session.remove_listener_stops_notifications", "[authoring]") {
    Session session;
    RecordingListener listener;
    const ListenerId id = session.add_listener(listener);
    (void)add_node(session, "spring");
    REQUIRE(listener.count() == 1);

    REQUIRE(session.remove_listener(id));
    REQUIRE(session.listener_count() == 0);
    (void)add_node(session, "mass");
    REQUIRE(listener.count() == 1);   // nothing new

    // Removing twice reports failure rather than silently succeeding: a
    // double-removal is a host bug, and a silent success would hide it.
    REQUIRE_FALSE(session.remove_listener(id));
    REQUIRE_FALSE(session.remove_listener(ListenerId{}));
}

TEST_CASE("authoring.session.self_removing_listener_is_safe", "[authoring]") {
    // A listener that removes itself during a notification must not invalidate
    // the iteration, and must not make the next notification skip its
    // neighbour -- the classic failure of this pattern.
    Session session;
    SelfRemovingListener self{session};
    RecordingListener other;
    const ListenerId self_id = session.add_listener(self);
    self.set_id(self_id);
    (void)session.add_listener(other);

    (void)add_node(session, "spring");
    REQUIRE(self.calls == 1);
    REQUIRE(other.count() == 1);

    // The self-removing listener is gone, and the other one keeps working.
    REQUIRE(session.listener_count() == 1);
    (void)add_node(session, "mass");
    REQUIRE(self.calls == 1);
    REQUIRE(other.count() == 2);
    REQUIRE(other.changes.back().node != NodeId{});
}

// ===========================================================================
// Undo and redo
// ===========================================================================

TEST_CASE("authoring.session.undo_redo_through_session", "[authoring]") {
    Session session;
    RecordingListener listener;
    (void)session.add_listener(listener);

    REQUIRE_FALSE(session.can_undo());
    REQUIRE_FALSE(session.can_redo());

    const NodeId id = add_node(session, "spring");
    REQUIRE(session.can_undo());
    REQUIRE(listener.count() == 1);

    // Undo goes through the session so that views are told. A caller that
    // reached the bus directly would leave every listener displaying the state
    // before the undo, which looks exactly like a refresh bug and is not one.
    REQUIRE(session.undo().has_value());
    REQUIRE(listener.count() == 2);
    REQUIRE(listener.changes.back().kind == ChangeKind::reset);
    REQUIRE(listener.changes.back().node == id);
    REQUIRE(session.graph().node_count() == 0);
    REQUIRE(session.can_redo());

    // Redo keeps the node id identical to the original application. This is the
    // property the whole design turns on: a saved document, a cache key, and a
    // measurement record all refer to a node by id, and an id that changed across
    // an undo would silently invalidate all three.
    REQUIRE(session.redo().has_value());
    REQUIRE(listener.count() == 3);
    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(session.graph().find_node(id) != nullptr);
    REQUIRE(listener.changes.back().node == id);

    // The sequence number advances on undo and redo as well: an undo is an edit,
    // and a counter that went backwards would make "has anything changed since I
    // looked" unanswerable.
    REQUIRE(session.sequence() == 3);
    REQUIRE(listener.changes.back().sequence == 3);
}

TEST_CASE("authoring.session.empty_history_is_an_error", "[authoring]") {
    Session session;

    // Reported, not silently ignored: a host that undoes when there is nothing to
    // undo has a bug, and a no-op success would hide it behind a stale UI.
    const auto undo_result = session.undo();
    REQUIRE_FALSE(undo_result.has_value());
    REQUIRE(undo_result.error() == qp::diag::ErrorCode::unknown_node);
    REQUIRE(session.sequence() == 0);

    const auto redo_result = session.redo();
    REQUIRE_FALSE(redo_result.has_value());
    REQUIRE(redo_result.error() == qp::diag::ErrorCode::unknown_node);
    REQUIRE(session.sequence() == 0);

    // A change that was applied and then undone still leaves nothing to redo
    // after a new edit: the redo branch is dropped, which is what makes the
    // history a line rather than a tree.
    const NodeId id = add_node(session, "spring");
    REQUIRE(session.undo().has_value());
    REQUIRE(session.can_redo());
    (void)id;
    (void)add_node(session, "mass");
    REQUIRE_FALSE(session.can_redo());
    REQUIRE_FALSE(session.redo().has_value());
}

// ===========================================================================
// Opening a document: the graph is replaced, not edited
// ===========================================================================

TEST_CASE("authoring.session.replacing_the_graph_resets_the_history", "[authoring]") {
    // The property that makes opening a document safe. An undo entry holds node ids and an inverse
    // operation written for a graph that no longer exists, and a fresh graph **reuses** those ids -- so an
    // entry that survived the swap would apply the previous document's edits to the new one. Nothing
    // would fail visibly: the ids match, so it would just happen.
    Session session;
    RecordingListener listener;
    (void)session.add_listener(listener);

    const NodeId old_node = add_node(session, "spring");
    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(session.can_undo());

    // A second document: a different graph, with its own node in the slot the first one used.
    Graph replacement;
    const auto fresh = replacement.add_node("mass");
    REQUIRE(fresh.has_value());
    REQUIRE(fresh.value().index == old_node.index);   // the same handle index, deliberately

    session.replace_graph(std::move(replacement));

    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(session.graph().find_node(fresh.value()) != nullptr);
    REQUIRE(session.graph().find_node(fresh.value())->type_name == "mass");

    // The collision, asserted rather than assumed away, because it is the reason the history has to go: a
    // fresh graph **reuses slot indices and generations**, so the handle that named the previous
    // document's "spring" now names this document's "mass". `has_node` cannot tell the two apart -- it
    // answers "this handle is live in this graph", which is true and useless here -- so nothing may
    // survive a document load by handle. Views rebuild on `reset`; the undo stack is discarded.
    REQUIRE(fresh.value() == old_node);
    REQUIRE(session.graph().has_node(old_node));
    REQUIRE(session.graph().find_node(old_node)->type_name == "mass");

    // Nothing to undo, and undoing anyway is refused rather than applying the previous document's edit to
    // the node that now answers to that handle.
    REQUIRE_FALSE(session.can_undo());
    REQUIRE_FALSE(session.can_redo());
    REQUIRE_FALSE(session.undo().has_value());
    REQUIRE(session.graph().find_node(fresh.value())->type_name == "mass");

    // The new document is editable in the ordinary way: the bus still points at the session's graph.
    REQUIRE(session.apply(SetParam{fresh.value(), 2, qp::ports::Value{1.5}}).has_value());
    REQUIRE(session.can_undo());
}

TEST_CASE("authoring.session.replacing_the_graph_notifies_reset_once", "[authoring]") {
    // Views rebuild on `reset` and patch on everything else, so the kind is not decoration: a document
    // load reported as `node` would leave every view showing the previous document's nodes, and the
    // window's own status line would keep counting them.
    Session session;
    RecordingListener listener;
    (void)session.add_listener(listener);

    (void)add_node(session, "spring");
    (void)add_node(session, "mass");
    const std::size_t before = listener.count();
    const std::uint64_t sequence_before = session.sequence();

    Graph replacement;
    (void)replacement.add_node("incline");
    session.replace_graph(std::move(replacement));

    // Exactly one change, of kind `reset`, with no node attached: a graph-wide event.
    REQUIRE(listener.count() == before + 1);
    REQUIRE(listener.last_kind() == ChangeKind::reset);
    REQUIRE_FALSE(listener.changes.back().node.valid());
    // The version and the sequence move forward, because a view compares them to drop duplicates: a
    // reload that kept the old version would look like nothing had happened.
    REQUIRE(listener.changes.back().version == session.graph().version());
    REQUIRE(listener.changes.back().sequence == sequence_before + 1);
    REQUIRE(session.sequence() == sequence_before + 1);

    // A listener that reads the graph during the notification sees the new document, not the old one with
    // the announcement in flight.
    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(listener.changes.back().version != 0);
}
