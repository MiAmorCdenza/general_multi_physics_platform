/**
 * @file session.cpp
 * @brief Implementation of the editing session.
 */
#include <qp/authoring/commands/session.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <variant>

namespace qp::authoring {
namespace {

/// @brief The change kind implied by a command.
///
/// Derived from the command variant rather than from its label: a label is a
/// user-facing string that may be reworded, and a view that stopped refreshing
/// because a label changed would be a defect with no visible cause.
[[nodiscard]] ChangeKind kind_of(const graph::Command& command) noexcept {
    return std::visit(
        [](const auto& c) -> ChangeKind {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, graph::AddNode> ||
                          std::is_same_v<T, graph::RemoveNode> ||
                          std::is_same_v<T, graph::SetNodeName> ||
                          std::is_same_v<T, graph::SetBypass>) {
                return ChangeKind::node;
            } else if constexpr (std::is_same_v<T, graph::Connect> ||
                                 std::is_same_v<T, graph::Disconnect>) {
                return ChangeKind::edge;
            } else {
                return ChangeKind::parameter;
            }
        },
        command);
}

}  // namespace

diag::Result<graph::NodeId> Session::reserve_node() { return bus_.reserve_node(); }

diag::Result<void> Session::apply(const graph::Command& command) {
    auto outcome = bus_.apply(command);
    if (!outcome) return outcome.error();

    // An idempotent command reports success with changed == false -- setting a
    // parameter to the value it already has. Broadcasting then would make every
    // listener redraw on every frame of a slider drag that is not moving, and
    // would advance the sequence number without anything having happened, so
    // "has anything changed since I last looked" would stop meaning anything.
    const graph::ApplyOutcome& result = outcome.value();
    if (!result.changed) return {};

    const std::optional<graph::NodeId> target = graph::command_target(command);
    notify(kind_of(command), target.value_or(graph::NodeId{}));
    return {};
}

diag::Result<void> Session::undo() {
    if (!bus_.can_undo()) return diag::ErrorCode::unknown_node;

    // Captured before the undo runs: afterwards the record has moved to the redo
    // stack, so the moment for reading which node it affected has passed.
    const graph::NodeId target = bus_.history().next_undo_target();

    auto done = bus_.undo();
    if (!done) return done.error();
    if (!done.value()) return diag::ErrorCode::unknown_node;

    // Always `reset` rather than a finer kind. An undo may revert a compound
    // change whose exact shape the session no longer knows, and telling a view
    // "this was only a parameter change" when it was not is worse than telling it
    // to rebuild.
    notify(ChangeKind::reset, target);
    return {};
}

diag::Result<void> Session::redo() {
    if (!bus_.can_redo()) return diag::ErrorCode::unknown_node;

    const graph::NodeId target = bus_.history().next_redo_target();

    auto done = bus_.redo();
    if (!done) return done.error();
    if (!done.value()) return diag::ErrorCode::unknown_node;

    notify(ChangeKind::reset, target);
    return {};
}

ListenerId Session::add_listener(IChangeListener& listener) noexcept {
    const std::uint32_t index = next_listener_++;
    listeners_.push_back(&listener);
    listener_generation_.push_back(index);

    ListenerId id;
    id.index = index;
    return id;
}

bool Session::remove_listener(ListenerId id) noexcept {
    if (!id.valid()) return false;
    for (std::size_t i = 0; i < listener_generation_.size(); ++i) {
        if (listener_generation_[i] != id.index) continue;

        // A listener that removes itself during a notification would invalidate
        // the iteration, so removal clears the slot instead of erasing it. The
        // dead slots are compacted once notification has finished.
        listeners_[i] = nullptr;
        listener_generation_[i] = 0;
        if (!notifying_) {
            listeners_.erase(listeners_.begin() + static_cast<std::ptrdiff_t>(i));
            listener_generation_.erase(listener_generation_.begin() +
                                       static_cast<std::ptrdiff_t>(i));
        }
        return true;
    }
    return false;
}

std::size_t Session::listener_count() const noexcept {
    std::size_t n = 0;
    for (const IChangeListener* l : listeners_) {
        if (l != nullptr) ++n;
    }
    return n;
}

void Session::notify(ChangeKind kind, graph::NodeId node) {
    ++sequence_;

    Change change;
    change.kind = kind;
    change.node = node;
    change.version = bus_.graph().version();
    change.sequence = sequence_;

    notifying_ = true;
    for (IChangeListener* listener : listeners_) {
        if (listener != nullptr) listener->on_change(change);
    }
    notifying_ = false;

    // Compact now that the iteration is over, so a self-removing listener cannot
    // make the next notification skip its neighbour.
    const auto first_dead = std::remove(listeners_.begin(), listeners_.end(), nullptr);
    if (first_dead != listeners_.end()) {
        const std::size_t dead = static_cast<std::size_t>(listeners_.end() - first_dead);
        listeners_.erase(first_dead, listeners_.end());
        listener_generation_.erase(listener_generation_.end() -
                                       static_cast<std::ptrdiff_t>(dead),
                                   listener_generation_.end());
    }
}

}  // namespace qp::authoring
