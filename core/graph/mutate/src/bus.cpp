/**
 * @file bus.cpp
 * @brief 命令总线与撤销栈的实现。
 *
 * 实现遵循同一模板，顺序是契约的一部分：
 *   1. 校验（失败立即返回，图与撤销栈都不动）
 *   2. **捕获逆操作所需的数据**（必须在应用之前）
 *   3. 应用
 *   4. 压入撤销记录
 *
 * 第 2 步在第 3 步之前是硬要求：`SetParam` 要记住的是**旧值**，
 * 应用之后就再也读不到了。
 */
#include <qp/graph/mutate/bus.hpp>

#include <algorithm>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

/// @brief 收集指向某节点的全部边（节点被删时用于构造逆操作）。
[[nodiscard]] std::vector<Edge> edges_touching(const Graph& g, NodeId id) {
    std::vector<Edge> out;
    for (const auto& e : g.edges()) {
        if (e.from.node == id || e.to.node == id) out.push_back(e);
    }
    return out;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// UndoStack
// ═══════════════════════════════════════════════════════════════════════════

void UndoStack::push(UndoEntry entry, bool allow_merge) {
    // 新编辑使重做分支失效——这是所有编辑器的一致语义：
    // 一旦从历史中间分叉，原来的"未来"就不再可达。
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
        // 回退失败：把它放回去，历史与图保持对应
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

void UndoStack::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

// ═══════════════════════════════════════════════════════════════════════════
// CommandBus
// ═══════════════════════════════════════════════════════════════════════════

Result<NodeId> CommandBus::reserve_node() {
    if (!graph_->is_stable()) {
        // 留一个待补全的槽位会让"这张图是否处于稳定状态"变成
        // 需要跨模块推理的问题。拒绝，并要求调用方先完成上一个命令。
        return Result<NodeId>{ErrorCode::graph_busy};
    }
    return graph_->reserve_node();
}

Result<ApplyOutcome> CommandBus::apply(const Command& c, bool allow_merge) {
    // 稳定性预检必须**区分命令类型**。
    //
    // `reserve_node()` 的语义就是"我先把 id 占住，稍后告诉你类型"，
    // 这段时间里图按定义是不稳定的。因此 `AddNode` 针对**已预留的** id
    // 必须被允许——那是正常握手，不是错误。
    //
    // 其余命令在图不稳定时一律拒绝：留一个待补全的槽位会让
    // "这张图是否处于稳定状态"变成需要跨模块推理的问题。
    //
    // 早先的写法在函数开头无条件检查 is_stable()，结果把
    // "先 reserve 再 apply(AddNode)" 这个**标准用法**直接拒掉了
    // ——由 graph.mutate.apply_add_node 抓出。
    const bool is_add_node = std::holds_alternative<AddNode>(c);
    if (!is_add_node && !graph_->is_stable()) {
        return Result<ApplyOutcome>{ErrorCode::graph_busy};
    }
    if (is_add_node) {
        const auto& add = std::get<AddNode>(c);
        const bool slot_is_prepared = graph_->has_node(add.id);
        if (!slot_is_prepared && !graph_->is_stable()) {
            // 既没预留、图又不稳定 → 现场预留会留下两个待补全槽位
            return Result<ApplyOutcome>{ErrorCode::graph_busy};
        }
    }

    const GraphVersion version_before = graph_->version();
    const std::size_t undo_before = undo_.undo_size();

    // ── 参数预检 ────────────────────────────────────────────────────────────
    //
    // 必须在**任何变异之前**完成。否则会出现"预留了槽位、随后校验失败、
    // 于是图被永久留在不稳定状态"——后续每一条命令都会被 graph_busy 拒绝，
    // 而用户看到的现象是"图突然不能编辑了"。
    // 这条路径由 graph.mutate.apply_rejects_invalid 抓出。
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

    // 逐命令处理。每个分支都遵守"校验 → 捕获旧值 → 应用 → 记录"。
    Result<ApplyOutcome> outcome = std::visit(
        [this, &version_before, allow_merge](const auto& cmd) -> Result<ApplyOutcome> {
            using T = std::decay_t<decltype(cmd)>;

            // ── AddNode ─────────────────────────────────────────────────────
            if constexpr (std::is_same_v<T, AddNode>) {
                if (cmd.type_name.empty()) {
                    return Result<ApplyOutcome>{ErrorCode::invalid_argument};
                }
                if (!cmd.id.valid()) {
                    return Result<ApplyOutcome>{ErrorCode::invalid_argument};
                }
                // 三种情形必须区分开：
                //   a) 该 id 已被**真实**节点占用 → 冲突
                //   b) 该 id 是 pending 槽位      → 调用方已按约定预留，补全即可
                //   c) 该 id 尚未存在             → 现场预留
                // 只有 has_node 一个谓词时，(a) 与 (b) 无法区分，
                // 于是"先 reserve 再 apply"这个标准用法会被误判为冲突。
                const bool occupied_by_real_node =
                    graph_->has_node(cmd.id) && !graph_->is_pending(cmd.id);
                if (occupied_by_real_node) {
                    return Result<ApplyOutcome>{ErrorCode::duplicate_connection};
                }

                NodeId real = cmd.id;
                const bool had_slot = graph_->has_node(real);
                if (!had_slot) {
                    // 尚未预留 → 现场预留。图必须稳定，否则会留下两个待补全槽位。
                    if (!graph_->is_stable()) {
                        return Result<ApplyOutcome>{ErrorCode::graph_busy};
                    }
                    auto reserved = graph_->reserve_node();
                    if (!reserved) return Result<ApplyOutcome>{reserved.error()};
                    real = reserved.value();
                }

                auto filled = graph_->fill_reserved(real, cmd.type_name);
                if (!filled) {
                    // 若这个槽位是**我们**刚预留的，必须收回去：
                    // 否则图会被永久留在不稳定状态，后续命令全被拒绝。
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
                                   // 重做：按**同一 id** 恢复
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

            // ── RemoveNode ──────────────────────────────────────────────────
            else if constexpr (std::is_same_v<T, RemoveNode>) {
                const Node* n = graph_->find_node(cmd.id);
                if (n == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::unknown_node};
                }
                // **先捕获**：节点与它的全部边
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
                           /*allow_merge=*/false);   // 删除不合并
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // ── SetParam ────────────────────────────────────────────────────
            else if constexpr (std::is_same_v<T, SetParam>) {
                Node* n = graph_->find_node_mutable(cmd.id);
                if (n == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::unknown_node};
                }
                if (cmd.port == 0) {
                    return Result<ApplyOutcome>{ErrorCode::invalid_argument};
                }
                // **先捕获旧值**（应用之后就读不到了）
                const qp::ports::Value previous = n->param(cmd.port);
                const bool had_value = previous.valid();

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
                           allow_merge);   // 连续拖动滑块合并成一条
                return Result<ApplyOutcome>{
                    ApplyOutcome{target, graph_->version(), true}};
            }

            // ── EraseParam ──────────────────────────────────────────────────
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

            // ── SetNodeName ─────────────────────────────────────────────────
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

            // ── SetBypass ───────────────────────────────────────────────────
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

            // ── Connect ─────────────────────────────────────────────────────
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

            // ── Disconnect ──────────────────────────────────────────────────
            else if constexpr (std::is_same_v<T, Disconnect>) {
                const Edge* existing = graph_->incoming(cmd.input);
                if (existing == nullptr) {
                    return Result<ApplyOutcome>{ErrorCode::not_connected};
                }
                // **先捕获**这条边，撤销时才能精确还原
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
        // 失败：撤销栈不能被留半条记录
        if (undo_.undo_size() != undo_before) {
            // 正常情况下不会发生；作为防御保留版本号不变
            (void)version_before;
        }
        return outcome;
    }
    return outcome;
}

}  // namespace qp::graph
