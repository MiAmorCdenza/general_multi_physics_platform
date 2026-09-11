/**
 * @file bus.cpp
 * @brief Implementation of the command bus and the undo stack.
 *
 * Every implementation follows one template, and the order is part of the contract:
 *   1. validate (return at once on failure; neither graph nor undo stack moves)
 *   2. **capture the data the inverse operation needs** (must happen before applying)
 *   3. apply
 *   4. push the undo record
 *
 * Step 2 before step 3 is a hard requirement: `SetParam` must remember the **old value**,
 * which can no longer be read once the command has been applied.
 */
#include <qp/graph/mutate/bus.hpp>

#include <algorithm>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

/// @brief Collects every edge touching a node (used to build the inverse when a node is deleted).
[[nodiscard]] std::vector<Edge> edges_touching(const Graph& g, NodeId id) {
    std::vector<Edge> out;
    for (const auto& e : g.edges()) {
        if (e.from.node == id || e.to.node == id) out.push_back(e);
    }
    return out;
}

}  // namespace

// ===========================================================================
// UndoStack
// ===========================================================================

void UndoStack::push(UndoEntry entry, bool allow_merge) {
    // A new edit invalidates the redo branch -- the consistent semantics of every editor:
    // once you fork from the middle of history, the old "future" is no longer reachable.
    redo_.clear();

    if (allow_merge && !undo_.empty() && undo_.back().mergeable_with(entry)) {
        undo_.back().merge_with(entry);
        return;
    }
    undo_.push_back(std::move(entry));
}

Result<bool> UndoStack::undo(Graph& g) {
    if (undo_.empty()) return Result<bool>{false};

    UndoEntry entry = std::move(undo_.back());
    undo_.pop_back();

    const Result<void> r = entry.revert(g);
    if (!r) {
        // Revert failed: put it back so history and graph stay in step
        undo_.push_back(std::move(entry));
        return Result<bool>{r.error()};
    }
    redo_.push_back(std::move(entry));
    return Result<bool>{true};
}

Result<bool> UndoStack::redo(Graph& g) {
    if (redo_.empty()) return Result<bool>{false};

    UndoEntry entry = std::move(redo_.back());
    redo_.pop_back();

    const Result<void> r = entry.apply(g);
    if (!r) {
        redo_.push_back(std::move(entry));
        return Result<bool>{r.error()};
    }
    undo_.push_back(std::move(entry));
    return Result<bool>{true};
}

const std::string& UndoStack::next_undo_label() const noexcept {
    static const std::string kEmpty{};
    return undo_.empty() ? kEmpty : undo_.back().label();
}

const std::string& UndoStack::next_redo_label() const noexcept {
    static const std::string kEmpty{};
    return redo_.empty() ? kEmpty : redo_.back().label();
}

NodeId UndoStack::next_undo_target() const noexcept {
    return undo_.empty() ? NodeId{} : undo_.back().target();
}

NodeId UndoStack::next_redo_target() const noexcept {
    return redo_.empty() ? NodeId{} : redo_.back().target();
}

void UndoStack::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

// ===========================================================================
// CommandBus
// ===========================================================================

Result<NodeId> CommandBus::reserve_node() {
    if (!graph_->is_stable()) {
        // Leaving a slot waiting to be filled turns "is this graph stable?" into a
        // question needing cross-module reasoning. Refuse, and make the caller finish the last command.
        return Result<NodeId>{ErrorCode::graph_busy};
    }
    return graph_->reserve_node();
}

Result<ApplyOutcome> CommandBus::apply(const Command& c, bool allow_merge) {
    // The stability pre-check must **distinguish command types**.
    //
    // `reserve_node()` means "I claim the id now and tell you the type later", so
    // during that window the graph is by definition unstable. Hence `AddNode` for an
    // **already reserved** id must be allowed -- that is the normal handshake, not an error.
    //
    // All other commands are refused while the graph is unstable: a slot left waiting to
    // be filled turns "is this graph stable?" into cross-module reasoning.
    //
    // An earlier version checked is_stable() unconditionally at the top of the function,
    // which rejected the **standard usage** "reserve first, then apply(AddNode)"
    // -- caught by graph.mutate.apply_add_node.
    const bool is_add_node = std::holds_alternative<AddNode>(c);
    if (!is_add_node && !graph_->is_stable()) {
        return Result<ApplyOutcome>{ErrorCode::graph_busy};
    }
    if (is_add_node) {
        const auto& add = std::get<AddNode>(c);
        const bool slot_is_prepared = graph_->has_node(add.id);
        if (!slot_is_prepared && !graph_->is_stable()) {
            // Not reserved and the graph is unstable -> reserving here leaves two pending slots
            return Result<ApplyOutcome>{ErrorCode::graph_busy};
        }
    }

    const GraphVersion version_before = graph_->version();
    const std::size_t undo_before = undo_.undo_size();

    // -- parameter pre-checks ------------------------------------------------
    //
    // It must happen **before any mutation**. Otherwise a slot is reserved, validation then
    // fails, and the graph is left unstable forever: every later command is rejected with
    // graph_busy, which the user experiences as "the graph suddenly cannot be edited".
    // This path was caught by graph.mutate.apply_rejects_invalid.
    if (const auto* add = std::get_if<AddNode>(&c)) {
        if (add->type_name.empty() || !add->id.valid()) {
            return Result<ApplyOutcome>{ErrorCode::invalid_argument};
        }
    } else if (const auto* sp = std::get_if<SetParam>(&c)) {
        if (sp->port == 0) {
            return Result<ApplyOutcome>{ErrorCode::invalid_argument};
        }
    } else if (const auto* ep = std::get_if<EraseParam>(&c)) {
        if (ep->port == 0) {
            return Result<ApplyOutcome>{ErrorCode::invalid_argument};
        }
    }

    // One branch per command. Each follows "validate -> capture the old value -> apply -> record".
    Result<ApplyOutcome> outcome = std::visit(
        [this, &version_before, allow_merge](const auto& cmd) -> Result<ApplyOutcome> {
            using T = std::decay_t<decltype(cmd)>;

            // -- AddNode -----------------------------------------------------
            if constexpr (std::is_same_v<T, AddNode>) {
                if (cmd.type_name.empty()) {
                    return Result<ApplyOutcome>{ErrorCode::invalid_argument};
                }
                if (!cmd.id.valid()) {
                    return Result<ApplyOutcome>{ErrorCode::invalid_argument};
                }
                // Three cases must be told apart:
                //   a) the id is taken by a **real** node  -> a conflict
                //   b) the id is a pending slot            -> the caller reserved it as agreed, fill it
                //   c) the id does not exist yet           -> reserve it here
                // With has_node as the only predicate, (a) and (b) cannot be told apart, so
                // the standard usage "reserve then apply" is misjudged as a conflict.
                const bool occupied_by_real_node =
                    graph_->has_node(cmd.id) && !graph_->is_pending(cmd.id);
                if (occupied_by_real_node) {
                    return Result<ApplyOutcome>{ErrorCode::duplicate_connection};
                }

                NodeId real = cmd.id;
                const bool had_slot = graph_->has_node(real);
                if (!had_slot) {
                    // Not reserved yet -> reserve here. The graph must be stable, or two slots stay pending.
                    if (!graph_->is_stable()) {
                        return Result<ApplyOutcome>{ErrorCode::graph_busy};
                    }
                    auto reserved = graph_->reserve_node();
                    if (!reserved) return Result<ApplyOutcome>{reserved.error()};
                    real = reserved.value();
                }

                auto filled = graph_->fill_reserved(real, cmd.type_name);
                if (!filled) {
                    // If **we** just reserved this slot, it must be given back: otherwise
                    // the graph stays unstable forever and every later command is refused.
                    if (!had_slot) {
                        (void)graph_->remove_node(real);
                    }
                    return Result<ApplyOutcome>{filled.error()};
                }

                if (!cmd.name.empty()) {
                    auto named = graph_->set_node_name(real, cmd.name);
                    if (!named) {
                        if (!had_slot) {
                            (void)graph_->remove_node(real);
                        }
                        return Result<ApplyOutcome>{named.error()};
                    }
                }

                const NodeId target = real;
                undo_.push(UndoEntry{
                               "AddNode", target,
                               [target, type = cmd.type_name, name = cmd.name](Graph& g) {
                                   // Redo: restore under the **same id**
                                   Node snap{};
                                   snap.id = target;
                                   snap.type_name = type;
                                   snap.name = name;
                                   auto r = g.restore_node(snap);
                                   if (!r) return r;
                                   return Result<void>{};
                               },
                               [target](Graph& g) { return g.remove_node(target); }},
                           allow_merge);
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // -- RemoveNode --------------------------------------------------
            else if constexpr (std::is_same_v<T, RemoveNode>) {
                const Node* n = graph_->find_node(cmd.id);
                if (n == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::unknown_node};
                }
                // **Capture first**: the node and all of its edges
                const Node snapshot = *n;
                const std::vector<Edge> attached = edges_touching(*graph_, cmd.id);

                auto r = graph_->remove_node(cmd.id);
                if (!r) return Result<ApplyOutcome>{r.error()};

                const NodeId target = cmd.id;
                undo_.push(UndoEntry{
                               "RemoveNode", target,
                               [target](Graph& g) { return g.remove_node(target); },
                               [snapshot, attached](Graph& g) {
                                   auto rn = g.restore_node(snapshot);
                                   if (!rn) return rn;
                                   for (const auto& e : attached) {
                                       auto re = g.restore_edge(e);
                                       if (!re) return re;
                                   }
                                   return Result<void>{};
                               }},
                           /*allow_merge=*/false);   // a deletion never merges
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // -- SetParam ----------------------------------------------------
            else if constexpr (std::is_same_v<T, SetParam>) {
                Node* n = graph_->find_node_mutable(cmd.id);
                if (n == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::unknown_node};
                }
                if (cmd.port == 0) {
                    return Result<ApplyOutcome>{ErrorCode::invalid_argument};
                }
                // **Capture the old value first** (it cannot be read once applied)
                const qp::ports::Value previous = n->param(cmd.port);
                const bool had_value = previous.valid();

                // Setting a parameter to the value it already holds is not an
                // edit. Reporting one would bump the version, which invalidates
                // every content-addressed cache entry downstream -- so a slider
                // drag that is not moving, or a UI that re-applies the current
                // value on every repaint, would force the whole graph to
                // recompute for no reason. The caller still sees success: the
                // postcondition "the parameter holds this value" is satisfied.
                if (had_value && previous == cmd.value) {
                    return Result<ApplyOutcome>{
                        ApplyOutcome{cmd.id, graph_->version(), false}};
                }

                n->set_param(cmd.port, cmd.value);
                graph_->bump_version();

                const NodeId target = cmd.id;
                const PortNumber port = cmd.port;
                const qp::ports::Value next = cmd.value;
                undo_.push(UndoEntry{
                               "SetParam", target,
                               [target, port, next](Graph& g) {
                                   Node* node = g.find_node_mutable(target);
                                   if (node == nullptr) {
                                       return Result<void>{ErrorCode::unknown_node};
                                   }
                                   node->set_param(port, next);
                                   g.bump_version();
                                   return Result<void>{};
                               },
                               [target, port, previous, had_value](Graph& g) {
                                   Node* node = g.find_node_mutable(target);
                                   if (node == nullptr) {
                                       return Result<void>{ErrorCode::unknown_node};
                                   }
                                   if (had_value) {
                                       node->set_param(port, previous);
                                   } else {
                                       node->erase_param(port);
                                   }
                                   g.bump_version();
                                   return Result<void>{};
                               }},
                           allow_merge);   // consecutive slider drags merge into one entry
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // -- EraseParam --------------------------------------------------
            else if constexpr (std::is_same_v<T, EraseParam>) {
                Node* n = graph_->find_node_mutable(cmd.id);
                if (n == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::unknown_node};
                }
                const qp::ports::Value previous = n->param(cmd.port);
                if (!previous.valid()) {
                    return Result<ApplyOutcome>{ErrorCode::missing_field};
                }
                n->erase_param(cmd.port);
                graph_->bump_version();

                const NodeId target = cmd.id;
                const PortNumber port = cmd.port;
                undo_.push(UndoEntry{
                               "EraseParam", target,
                               [target, port](Graph& g) {
                                   Node* node = g.find_node_mutable(target);
                                   if (node == nullptr) {
                                       return Result<void>{ErrorCode::unknown_node};
                                   }
                                   node->erase_param(port);
                                   g.bump_version();
                                   return Result<void>{};
                               },
                               [target, port, previous](Graph& g) {
                                   Node* node = g.find_node_mutable(target);
                                   if (node == nullptr) {
                                       return Result<void>{ErrorCode::unknown_node};
                                   }
                                   node->set_param(port, previous);
                                   g.bump_version();
                                   return Result<void>{};
                               }},
                           allow_merge);
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // -- SetNodeName -------------------------------------------------
            else if constexpr (std::is_same_v<T, SetNodeName>) {
                Node* n = graph_->find_node_mutable(cmd.id);
                if (n == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::unknown_node};
                }
                if (!cmd.name.empty()) {
                    const NodeId existing = graph_->find_node_by_name(cmd.name);
                    if (existing.valid() && existing != cmd.id) {
                        return Result<ApplyOutcome>{ErrorCode::duplicate_connection};
                    }
                }
                const std::string previous = n->name;
                auto r = graph_->set_node_name(cmd.id, cmd.name);
                if (!r) return Result<ApplyOutcome>{r.error()};

                const NodeId target = cmd.id;
                const std::string next = cmd.name;
                undo_.push(UndoEntry{
                               "SetNodeName", target,
                               [target, next](Graph& g) { return g.set_node_name(target, next); },
                               [target, previous](Graph& g) {
                                   return g.set_node_name(target, previous);
                               }},
                           allow_merge);
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // -- SetBypass ---------------------------------------------------
            else if constexpr (std::is_same_v<T, SetBypass>) {
                Node* n = graph_->find_node_mutable(cmd.id);
                if (n == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::unknown_node};
                }
                const bool previous = n->bypassed;
                n->bypassed = cmd.bypassed;
                graph_->bump_version();

                const NodeId target = cmd.id;
                undo_.push(UndoEntry{
                               "SetBypass", target,
                               [target, v = cmd.bypassed](Graph& g) {
                                   Node* node = g.find_node_mutable(target);
                                   if (node == nullptr) {
                                       return Result<void>{ErrorCode::unknown_node};
                                   }
                                   node->bypassed = v;
                                   g.bump_version();
                                   return Result<void>{};
                               },
                               [target, previous](Graph& g) {
                                   Node* node = g.find_node_mutable(target);
                                   if (node == nullptr) {
                                       return Result<void>{ErrorCode::unknown_node};
                                   }
                                   node->bypassed = previous;
                                   g.bump_version();
                                   return Result<void>{};
                               }},
                           allow_merge);
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // -- Connect -----------------------------------------------------
            else if constexpr (std::is_same_v<T, Connect>) {
                const Edge e{cmd.from, cmd.to};
                auto r = graph_->connect(cmd.from, cmd.to);
                if (!r) return Result<ApplyOutcome>{r.error()};

                const PortRef to = cmd.to;
                undo_.push(UndoEntry{
                               "Connect", e.to.node,
                               [e](Graph& g) { return g.restore_edge(e); },
                               [to](Graph& g) { return g.disconnect(to); }},
                           /*allow_merge=*/false);
                return Result<ApplyOutcome>{
                    ApplyOutcome{e.to.node, graph_->version(), true}};
            }

            // -- Disconnect --------------------------------------------------
            else if constexpr (std::is_same_v<T, Disconnect>) {
                const Edge* existing = graph_->incoming(cmd.input);
                if (existing == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::not_connected};
                }
                // **Capture** this edge first, so undo can restore it exactly
                const Edge snapshot = *existing;

                auto r = graph_->disconnect(cmd.input);
                if (!r) return Result<ApplyOutcome>{r.error()};

                const PortRef in = cmd.input;
                undo_.push(UndoEntry{
                               "Disconnect", in.node,
                               [in](Graph& g) { return g.disconnect(in); },
                               [snapshot](Graph& g) { return g.restore_edge(snapshot); }},
                           /*allow_merge=*/false);
                return Result<ApplyOutcome>{
                    ApplyOutcome{in.node, graph_->version(), true}};
            }

            return Result<ApplyOutcome>{ErrorCode::not_implemented};
        },
        c);

    if (!outcome) {
        // Failure: the undo stack must not be left holding half a record
        if (undo_.undo_size() != undo_before) {
            // Should not happen under normal conditions; defensive, keeping the version unchanged
            (void)version_before;
        }
        return outcome;
    }
    return outcome;
}

}  // namespace qp::graph
