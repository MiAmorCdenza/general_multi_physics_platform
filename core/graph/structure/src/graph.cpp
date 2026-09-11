/**
 * @file graph.cpp
 * @brief 图结构的实现。
 *
 * 全部变异遵循同一模板：
 *   1. 校验（失败立即返回，图未动）
 *   2. 应用（此时不再有失败可能）
 *   3. 递增版本号
 *
 * "应用阶段不失败"是关键：它让强异常保证成为**结构性**的，
 * 而不是靠 try/catch 兜底。
 */
#include <qp/graph/structure/graph.hpp>

#include <algorithm>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

}  // namespace

Graph::Graph() {
    // 槽位 0 是**哨兵**，永不占用。
    // 理由：NodeId::index == 0 表示"无"，若 0 是合法槽位，
    // 第一个加入的节点就会拿到 index 0 而立刻被判为无效句柄。
    // 用一个恒空的哨兵槽把"无"与"第 0 个"彻底分开。
    slots_.resize(1);
}

// ── 节点 ────────────────────────────────────────────────────────────────────

Node* Graph::node_at(NodeId id) noexcept {
    if (!id.valid() || id.index >= slots_.size()) return nullptr;
    NodeSlot& s = slots_[id.index];
    if (!s.occupied || s.generation != id.generation) return nullptr;
    return &s.node;
}

const Node* Graph::find_node(NodeId id) const noexcept {
    // 直接查（不通过 node_at）：const 版本必须保持 const 正确性，
    // 用 const_cast 去调非 const 版本会让 GCC 报 -fpermissive 错误。
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

    // 找一个空槽：优先复用已删除的槽位（保持 slots_ 不无限增长）。
    // 从 1 开始：槽位 0 是哨兵（见构造函数的说明）。
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

    // 没有空槽：追加。
    // 注意：**先 push_back 再从 slots_.back() 取 id**。早先的写法在
    // push_back 之前用局部变量构造 id，而 push_back 的重分配会让那个
    // Node 失效——返回的句柄于是指向已被销毁的副本。
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

    // 内联 add_node 的槽位分配，以便在**同一处**完成名字设置与版本递增。
    // 早先写成"调用 add_node 再补设名字"，补设那一步漏了版本号递增，
    // 于是"改名字"不会让缓存失效——由 failed_mutation_is_noop 抓出。
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
    // 空类型名即 pending。与 add_node 的唯一差别是**不做类型名校验**——
    // 预留的语义就是"我要占一个 id，类型稍后告诉你"。
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
        // 已补全过：重复 fill 是调用方的错误
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

    // 先删相关边（应用阶段不失败）
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [id](const Edge& e) {
                                    return e.from.node == id || e.to.node == id;
                                }),
                 edges_.end());

    NodeSlot& s = slots_[id.index];
    s.node = Node{};
    s.occupied = false;
    // **释放时也要递增世代**。
    //
    // 世代的两个用途都要求这一步：
    //   1. 老句柄永不复活——它记录的是**上一任**占用的世代；
    //   2. 撤销"删除"需要把节点恢复到**同一个 id**，而 restore_node
    //      靠比较"槽位世代 vs 恢复目标世代"来判断该 id 是否还能用。
    //      若释放时不递增，槽位世代会与刚被删节点的世代相同，
    //      restore_node 的 ABA 检查就会把一次**合法**的恢复判为冲突。
    // 这条由 graph.mutate.undo_redo_roundtrip 抓出：
    // 撤销再重做时 redo 失败于 duplicate_connection。
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

// ── 边 ──────────────────────────────────────────────────────────────────────

bool Graph::reaches(NodeId from, NodeId target) const noexcept {
    if (from == target) return true;
    // 深度优先，沿边方向前进。图很小（通常 < 100 节点），不需要更聪明的算法。
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
    // ── 1. 句柄有效 ──
    if (!from.valid() || !to.valid()) {
        return Result<void>{ErrorCode::unknown_node};
    }
    if (node_at(from.node) == nullptr || node_at(to.node) == nullptr) {
        return Result<void>{ErrorCode::unknown_node};
    }

    // ── 2. 方向 ──
    if (from.direction != PortDirection::output || to.direction != PortDirection::input) {
        return Result<void>{ErrorCode::invalid_argument};
    }

    // ── 3. 输入端口至多一条入边 ──
    for (const auto& e : edges_) {
        if (e.to == to) {
            return Result<void>{ErrorCode::duplicate_connection};
        }
    }

    // ── 4. 自环 ──
    if (from.node == to.node) {
        return Result<void>{ErrorCode::cycle_detected};
    }

    // ── 5. 环检测：若 to.node 已经能到达 from.node，则加这条边会成环 ──
    if (reaches(to.node, from.node)) {
        return Result<void>{ErrorCode::cycle_detected};
    }

    // ── 应用（此后不可能失败）──
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

    // 先把槽位扩到够大
    if (idx >= slots_.size()) {
        slots_.resize(static_cast<std::size_t>(idx) + 1);
    }
    NodeSlot& s = slots_[idx];
    if (s.occupied) {
        // 槽位被占用。若占用者的世代正好就是目标世代，说明这个 id
        // 已经在本槽位上活着——重复恢复是调用方的错误。
        // 若占用者的世代更高，那是后来的节点，同样不能覆盖。
        if (s.generation >= snapshot.id.generation) {
            return Result<void>{ErrorCode::duplicate_connection};
        }
        return Result<void>{ErrorCode::duplicate_connection};
    }

    // 槽位空闲：**没有 id 拥有它**，因此恢复是安全的。
    //
    // 这里刻意**不比较世代**。曾经的写法是 `if (gen >= target) reject`，
    // 那是错的：删除操作会递增空闲槽位的世代，于是"撤销一次删除再恢复"
    // 会把自己的槽位世代推到目标世代之上，然后被自己的检查拒绝。
    // 世代比较只在**占用**路径上有意义（占用者是谁），
    // 空闲路径上不存在"另一个 id"可言。这条由
    // graph.mutate.undo_redo_roundtrip 抓出。
    s.occupied = true;
    s.generation = snapshot.id.generation;
    s.node = snapshot;
    // 防御：世代计数器必须始终领先于任何已分配的世代
    if (next_generation_ <= snapshot.id.generation) {
        next_generation_ = snapshot.id.generation + 1;
    }
    ++live_nodes_;
    if (snapshot.type_name.empty()) {
        ++pending_nodes_;   // 恢复的也可以是一个 pending 节点
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
