/**
 * @file bus.hpp
 * @brief Command bus: the single entry point for **all** graph mutation, holding the single undo stack.
 *
 * ## Why the undo stack lives in the foundation, not in the editor plugin
 *
 * This is the central conclusion of `docs/plan-tree.md` §5.3, worth restating:
 *
 * The user can edit the same graph in the node editor, the YAML text view, and the block view.
 * If the undo stack lived in the editor, three failures would follow:
 *   - changes made in the YAML view **cannot be undone** (their commands never enter that stack);
 *   - the two views would each undo their own way and the document would jump back and forth;
 *   - the undo history would be lost when switching views.
 *
 * Hence: **exactly one undo stack, belonging to the graph's owner**. Views only translate gestures into commands.
 *
 * ## Undo works by "inverse command", not by snapshot
 *
 * On every successful apply the bus captures, **at that moment**, the data the inverse operation needs.
 * Capture must happen before the apply (for example `SetParam` must remember the old value), so the
 * "read the old value, then apply" order in the implementation is part of the contract.
 *
 * Compared with a whole-graph snapshot: memory drops from O(graph size) to O(one operation),
 * and "what was undone" is readable (`command_name()`).
 *
 * ## Merging on the same node
 *
 * Dragging a slider produces dozens of consecutive `SetParam` commands. Undoing them one by one
 * means the user must press Ctrl+Z dozens of times to get back to the pre-drag state -- that is
 * not undo, it is punishment. So `UndoStack` merges consecutive same-kind commands on one node.
 *
 * @ownership   owns (owns the graph and the undo stack)
 * @thread      main (all methods)
 * @pre         none
 * @post        none
 * @invariant   Every structural graph change goes through this class; the undo stack matches the graph state
 * @errors      does not throw; failures go through Result
 * @frozen      no
 */
#pragma once

#include <qp/graph/mutate/command.hpp>
#include <qp/graph/structure.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qp::graph {

using qp::diag::Result;

/**
 * @brief One undoable edit record.
 *
 * It holds a pair of "apply / revert" operations. The pair is **closures** capturing only the command
 * and the captured old state, never a graph pointer: the bus owns the graph, so records travel freely.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   revert and apply are inverse operations on the same graph
 * @errors      does not throw
 * @frozen      no
 * @tests       graph.mutate.undo_set_param, graph.mutate.redo_restores
 */
class UndoEntry final {
public:
    UndoEntry(std::string label, NodeId target, std::function<Result<void>(Graph&)> apply,
              std::function<Result<void>(Graph&)> revert)
        : label_(std::move(label)),
          target_(target),
          apply_(std::move(apply)),
          revert_(std::move(revert)) {}

    [[nodiscard]] const std::string& label() const noexcept { return label_; }
    /// @brief The node this record targets. Used for the same-node merge decision.
    [[nodiscard]] NodeId target() const noexcept { return target_; }

    /// @brief Whether this record can merge with the one that immediately follows.
    [[nodiscard]] bool mergeable_with(const UndoEntry& next) const noexcept {
        return target_.valid() && target_ == next.target_ && label_ == next.label_;
    }

    /// @brief Merge with the next record (the next becomes the new apply, this one keeps revert).
    void merge_with(const UndoEntry& next) {
        apply_ = next.apply_;
    }

    /// @brief Apply (redo).
    Result<void> apply(Graph& g) const { return apply_(g); }
    /// @brief Revert (undo).
    Result<void> revert(Graph& g) const { return revert_(g); }

private:
    std::string label_;
    NodeId target_{};
    std::function<Result<void>(Graph&)> apply_;
    std::function<Result<void>(Graph&)> revert_;
};

/**
 * @brief Undo/redo stack. Usable only through `CommandBus`.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `undo_size() + redo_size()` monotonically reflects the number of recorded edits
 * @errors      does not throw
 * @frozen      no
 * @tests       graph.mutate.undo_redo_roundtrip, graph.mutate.new_edit_clears_redo,
 *              graph.mutate.merge_consecutive_set_param
 */
class UndoStack final {
public:
    /// @brief Record one edit. Clears the redo stack.
    void push(UndoEntry entry, bool allow_merge);

    /// @brief Undo one step. Returns false when there is no history.
    Result<bool> undo(Graph& g);
    /// @brief Redo one step. Returns false when there is nothing to redo.
    Result<bool> redo(Graph& g);

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undo_size() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_size() const noexcept { return redo_.size(); }

    /// @brief Label of the next edit to undo. Empty string on an empty stack.
    [[nodiscard]] const std::string& next_undo_label() const noexcept;
    /// @brief Label of the next edit to redo. Empty string on an empty stack.
    [[nodiscard]] const std::string& next_redo_label() const noexcept;

    /**
     * @brief The node the next undo would affect. Invalid on an empty stack.
     *
     * Needed by a caller that has to say what changed **before** performing the
     * undo: afterwards the record has moved to the other stack, so the moment for
     * reading it has passed.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Returns the target of the most recent edit, or an invalid id
     * @invariant   Consistent with next_undo_label() being non-empty
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.mutate.next_target_matches_history
     */
    [[nodiscard]] NodeId next_undo_target() const noexcept;

    /**
     * @brief The node the next redo would affect. Invalid on an empty stack.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Returns the target of the most recently undone edit, or an invalid id
     * @invariant   Consistent with next_redo_label() being non-empty
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.mutate.next_target_matches_history
     */
    [[nodiscard]] NodeId next_redo_target() const noexcept;

    /// @brief Clear the history on both sides.
    void clear() noexcept;

private:
    std::vector<UndoEntry> undo_;
    std::vector<UndoEntry> redo_;
};

/**
 * @brief Result of applying a command on the bus.
 */
struct ApplyOutcome final {
    /// The node the command affected, if any.
    std::optional<NodeId> node{};
    /// The graph's new version number.
    GraphVersion version = 0;
    /// Whether the graph actually changed (an idempotent command may not, e.g. setting a parameter to the same value).
    bool changed = false;
};

/**
 * @brief Command bus: the graph's only edit entry point + the only undo stack.
 *
 * @ownership   owns (the graph and the undo stack)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   After a successful apply the version strictly increases; on failure graph and version are unchanged
 * @errors      does not throw
 * @frozen      no
 * @tests       graph.mutate.apply_add_node, graph.mutate.apply_remove_node,
 *              graph.mutate.apply_set_param, graph.mutate.apply_connect,
 *              graph.mutate.apply_rejects_invalid, graph.mutate.failed_apply_is_noop,
 *              graph.mutate.reserve_id_is_stable_across_undo,
 *              graph.mutate.rejects_apply_while_unstable,
 *              graph.mutate.graph_accessor_is_readonly
 */
class CommandBus final {
public:
    explicit CommandBus(Graph& graph) noexcept : graph_(&graph) {}

    CommandBus(const CommandBus&) = delete;
    CommandBus& operator=(const CommandBus&) = delete;

    /**
     * @brief Reserve the id of a new node (used by `AddNode`).
     *
     * @ownership   owns (occupies one slot in the graph)
     * @thread      main
     * @pre         The graph is stable (`is_stable()`)
     * @post        On success the returned handle is immediately valid but pending
     * @post        On failure (unstable graph) the graph and the version are unchanged
     * @invariant   The same id is never handed out twice
     * @errors      Result<NodeId>; unstable graph -> graph_busy
     * @complexity  O(1) amortized
     * @nondet      none
     * @frozen      no
     * @tests       graph.mutate.reserve_id_is_stable_across_undo,
     *              graph.mutate.rejects_apply_while_unstable
     */
    Result<NodeId> reserve_node();

    /**
     * @brief Apply one command.
     *
     * @ownership   borrows (keeps no reference beyond the command or the graph)
     * @thread      main
     * @pre         The graph is stable
     * @post        On success the graph changed, the version increased, and one record was pushed
     * @post        **On failure the graph, the version and the undo stack are all unchanged**
     * @invariant   outcome.node on success matches command_target
     * @errors      Result<ApplyOutcome>; see each command's validation rules
     * @complexity  Depends on the command; Connect is O(V+E)
     * @nondet      none
     * @frozen      no
     * @tests       graph.mutate.apply_add_node, graph.mutate.apply_remove_node,
     *              graph.mutate.apply_set_param, graph.mutate.apply_connect,
     *              graph.mutate.apply_rejects_invalid
     */
    Result<ApplyOutcome> apply(const Command& c, bool allow_merge = true);

    /// @brief Undo one step. Returns false (not an error) when there is no history.
    Result<bool> undo() { return undo_.undo(*graph_); }
    /// @brief Redo one step. Returns false (not an error) when there is no history.
    Result<bool> redo() { return undo_.redo(*graph_); }

    [[nodiscard]] bool can_undo() const noexcept { return undo_.can_undo(); }
    [[nodiscard]] bool can_redo() const noexcept { return undo_.can_redo(); }
    [[nodiscard]] const UndoStack& history() const noexcept { return undo_; }

    /**
     * @brief Discards the whole undo history, leaving the graph alone.
     *
     * For the one caller that replaces the graph wholesale -- opening a document. An undo entry holds
     * node ids and an inverse operation for a graph that **no longer exists**, and the ids are exactly
     * the kind that a fresh graph reuses: running such an entry would apply the previous document's edits
     * to the current one. That is not undo, it is corruption with a plausible name, and the ids make it
     * silent.
     *
     * The symmetry is deliberate: the bus owns the graph *and* the history, so it is also the object
     * that must be told when the graph underneath it is swapped. A caller that could replace the graph
     * without this would have to reach into the stack itself, and the invariant "the undo stack matches
     * the graph state" would become a verbal agreement.
     *
     * @ownership   pure (touches the history only)
     * @thread      main
     * @pre         none
     * @post        `can_undo()` and `can_redo()` are both false, and `history().undo_size() == 0`
     * @invariant   The graph and its version are untouched
     * @errors      noexcept
     * @complexity  O(entries)
     * @nondet      none
     * @frozen      no
     * @tests       graph.mutate.forgetting_history_leaves_the_graph
     */
    void forget_history() noexcept { undo_.clear(); }

    /// @brief Read-only access to the owned graph. **Mutation must go through apply().**
    ///
    /// A non-const accessor is deliberately not provided: it would reduce the "all mutation
    /// goes through the bus" invariant to a verbal agreement.
    [[nodiscard]] const Graph& graph() const noexcept { return *graph_; }

private:
    Graph* graph_;
    UndoStack undo_;
};

}  // namespace qp::graph
