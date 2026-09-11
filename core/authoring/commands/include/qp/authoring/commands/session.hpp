/**
 * @file session.hpp
 * @brief The one editing session: the entry point every view mutates through, and the one place changes are broadcast from.
 *
 * ## Why a view must not hold "my graph"
 *
 * The moment a second view exists -- a node editor and a property panel, or a
 * node editor and a time-series plot -- two copies of the graph exist, and the
 * copies diverge. The divergence is not a crash: the node editor moves a slider,
 * the property panel still shows the old value, the user believes the panel, and
 * the experiment recorded is not the one they configured. That is the failure
 * this module exists to make impossible.
 *
 * So the rule is structural rather than advisory: **there is one `Graph`, one
 * `CommandBus`, and one undo stack**, and `Session` is the only object that
 * touches them. A view holds a pointer to the session, never to a graph.
 *
 * ## Why change notification carries a kind and not a payload
 *
 * A payload would have to describe every possible change, which means either a
 * variant that grows a case per command or a free-form blob that every view
 * parses differently. A `ChangeKind` plus the affected node is enough for a view
 * to answer its actual question -- "do I draw this node differently now" -- and
 * it cannot go stale the way a serialized snapshot can.
 *
 * The notification is also deliberately **not** a subscription list with
 * priorities. Views are notified in registration order, once each, after the
 * mutation has already been applied: a view that tries to mutate in response to a
 * change would recurse, and the ordering would decide the outcome. A view that
 * needs to change the graph does it in an explicit user action, not in a
 * reaction.
 *
 * ## Why the listener interface has no unregister
 *
 * Deregistration is the session's job, not the listener's: `add_listener`
 * returns a `ListenerId` and `remove_listener` takes it. A listener that removed
 * itself during notification would invalidate the iteration, which is the
 * classic way this pattern goes wrong.
 *
 * @ownership   owns (the graph, the bus, the listener list)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every successful apply() produces exactly one change notification per listener
 * @errors      noexcept (failure is reported through Result)
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       authoring.session.apply_returns_command_result,
 *              authoring.session.listeners_notified_once_per_change,
 *              authoring.session.undo_redo_through_session,
 *              authoring.session.change_kind_follows_the_command,
 *              authoring.session.empty_history_is_an_error,
 *              authoring.session.self_removing_listener_is_safe
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/mutate.hpp>
#include <qp/graph/structure.hpp>

#include <cstdint>
#include <vector>

namespace qp::authoring {

/**
 * @brief What kind of change happened, for a view that has to redraw something.
 *
 * Coarse on purpose. A finer classification would have to be extended for every
 * new command, and a view that filters on the wrong axis would silently stop
 * updating -- the exact bug this notification exists to prevent.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A change that alters the graph's version reports exactly one kind
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.session.change_kinds
 */
enum class ChangeKind : std::uint8_t {
    /// A node was added, removed, or had its name or type changed.
    node = 0,
    /// An edge was added or removed.
    edge = 1,
    /// A parameter value changed. The most frequent kind by far.
    parameter = 2,
    /// The graph structure changed wholesale: a document load, or an undo that
    /// crossed a structural boundary. Views should rebuild rather than patch.
    reset = 3,
};

/// @brief Stable short name of a change kind, for logs and diagnostics.
[[nodiscard]] constexpr const char* to_string(ChangeKind k) noexcept {
    switch (k) {
        case ChangeKind::node: return "node";
        case ChangeKind::edge: return "edge";
        case ChangeKind::parameter: return "parameter";
        case ChangeKind::reset: return "reset";
    }
    return "unknown";
}

/**
 * @brief One notification: what changed, and where.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `version` is the graph version **after** the change was applied
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.session.listeners_notified_once_per_change
 */
struct Change final {
    ChangeKind kind = ChangeKind::parameter;
    /// The node the change is about. `kInvalidNode` for a graph-wide reset.
    graph::NodeId node{};
    /// Graph version after the change. A view can compare it to drop duplicates.
    graph::GraphVersion version = 0;
    /// Commands applied since the session was created. Never decreases, not even
    /// across an undo: an undo is also an edit, and a counter that went backwards
    /// would make "has anything happened since I last looked" unanswerable.
    std::uint64_t sequence = 0;
};

/**
 * @brief A view's hook into the session.
 *
 * @ownership   observes (the session owns nothing of the listener)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A listener must not apply a command from inside on_change
 * @errors      noexcept
 * @frozen      no
 * @tests       authoring.session.listeners_notified_once_per_change
 */
class IChangeListener {
public:
    IChangeListener() = default;
    virtual ~IChangeListener() = default;
    IChangeListener(const IChangeListener&) = delete;
    IChangeListener& operator=(const IChangeListener&) = delete;

    /**
     * @brief Called once per applied change, after it has taken effect.
     *
     * @ownership   observes
     * @thread      main
     * @pre         the session is not inside a nested notification
     * @post        the listener has updated whatever it displays
     * @invariant   The graph is already consistent when this runs
     * @errors      noexcept; a listener that throws would leave the session mid-notification
     * @complexity  whatever the view needs
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.listeners_notified_once_per_change
     */
    virtual void on_change(const Change& change) noexcept = 0;
};

/// @brief Handle for a registered listener.
struct ListenerId final {
    std::uint32_t index = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    [[nodiscard]] friend constexpr bool operator==(ListenerId a, ListenerId b) noexcept {
        return a.index == b.index;
    }
};

/**
 * @brief The single editing session: one graph, one command bus, one undo stack.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The graph is only ever mutated through apply()
 * @errors      noexcept (failure is reported through Result)
 * @frozen      no
 * @tests       authoring.session.apply_returns_command_result
 */
class Session final {
public:
    /// @brief Creates an empty graph and the one bus that may mutate it.
    ///
    /// The graph is owned here and the bus borrows it. `CommandBus` deliberately
    /// does not own a graph -- it takes a reference -- precisely so that a caller
    /// cannot end up with a bus pointing at a graph it does not control.
    Session() noexcept : bus_(graph_) {}

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// @brief The graph. Const access only: mutating it directly would bypass the
    ///        bus, the undo stack, and every listener.
    [[nodiscard]] const graph::Graph& graph() const noexcept { return bus_.graph(); }

    /// @brief The command bus, for callers that need its non-mutating queries.
    [[nodiscard]] const graph::CommandBus& bus() const noexcept { return bus_; }

    /**
     * @brief Reserves the id of a new node, for a caller about to apply AddNode.
     *
     * Forwarded here so that a view never needs the non-const bus. Handing out
     * the bus would let a view reach `reserve_node` and then forget to apply the
     * command, leaving the graph permanently unstable -- a state the whole
     * reserve/fill handshake exists to keep short.
     *
     * @ownership   owns (occupies one slot in the graph)
     * @thread      main
     * @pre         the graph is stable
     * @post        On success the id is valid but pending until AddNode is applied
     * @invariant   An id is never handed out twice
     * @errors      Returns graph_busy when the graph is already unstable
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.apply_returns_command_result
     */
    [[nodiscard]] diag::Result<graph::NodeId> reserve_node();

    /**
     * @brief Applies a command, broadcasts the change, and records it for undo.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success the graph is one command further along, every
     *              listener has been notified exactly once, and the sequence
     *              number has advanced by one
     * @invariant   On failure nothing changes: no listener is notified, the graph
     *              version is unchanged, and the sequence number does not advance
     * @errors      Returns the command's own error code
     * @complexity  O(listeners) plus the command's cost
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.apply_returns_command_result,
     *              authoring.session.failed_apply_notifies_nobody
     */
    [[nodiscard]] diag::Result<void> apply(const graph::Command& command);

    /**
     * @brief Undoes the most recent command and broadcasts a change.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success the graph is back to its previous state and every
     *              listener has been notified once
     * @invariant   An empty history is reported as an error, not as a silent no-op
     * @errors      Returns unknown_node when there is nothing to undo
     * @complexity  O(listeners)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.undo_redo_through_session
     */
    [[nodiscard]] diag::Result<void> undo();

    /**
     * @brief Redoes the most recently undone command and broadcasts a change.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success the graph is one command further along and every
     *              listener has been notified once
     * @invariant   Redo keeps node ids identical to the original application
     * @errors      Returns unknown_node when there is nothing to redo
     * @complexity  O(listeners)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.undo_redo_through_session
     */
    [[nodiscard]] diag::Result<void> redo();

    /// @brief Whether undo() would succeed.
    [[nodiscard]] bool can_undo() const noexcept { return bus_.can_undo(); }

    /// @brief Whether redo() would succeed.
    [[nodiscard]] bool can_redo() const noexcept { return bus_.can_redo(); }

    /// @brief Commands applied through this session, undos and redos included.
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }

    /**
     * @brief Registers a listener. Returns an invalid id when the list is full.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `listener` outlives the registration or is removed first
     * @post        The listener receives every subsequent change
     * @invariant   A listener is notified at most once per change
     * @errors      noexcept; allocation failure terminates. A registry that cannot
     *              record a listener is not a state the view layer can continue from
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.listeners_notified_once_per_change
     */
    [[nodiscard]] ListenerId add_listener(IChangeListener& listener) noexcept;

    /**
     * @brief Removes a listener. Safe to call from inside a notification.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        The listener receives no further changes
     * @invariant   Other listeners keep their ids and their order
     * @errors      noexcept; returns false when the id was already removed
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.remove_listener_stops_notifications
     */
    [[nodiscard]] bool remove_listener(ListenerId id) noexcept;

    /**
     * @brief Number of registered listeners.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Counts live listeners only; a slot awaiting compaction is not counted
     * @invariant   Never exceeds the number of successful add_listener calls
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       authoring.session.remove_listener_stops_notifications
     */
    [[nodiscard]] std::size_t listener_count() const noexcept;

private:
    /// @brief Broadcasts one change to every live listener, in registration order.
    void notify(ChangeKind kind, graph::NodeId node);

    graph::Graph graph_;
    graph::CommandBus bus_;
    std::vector<IChangeListener*> listeners_;
    /// Parallel to `listeners_`: the id each slot was registered under. Zero means
    /// the slot is dead but still occupying its index, which is what makes
    /// removal during a notification safe.
    std::vector<std::uint32_t> listener_generation_;
    std::uint64_t sequence_ = 0;
    std::uint32_t next_listener_ = 1;
    bool notifying_ = false;
};

}  // namespace qp::authoring
