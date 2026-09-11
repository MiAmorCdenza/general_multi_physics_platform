/**
 * @file graph.hpp
 * @brief 图结构：节点与边的容器，外加三件事——版本号、无环、强异常保证。
 *
 * ## 三条设计决定
 *
 * ### 1. 句柄含世代，槽位复用不会静默指错对象
 *
 * `NodeId` 是 `(index, generation)`。删除节点时**只清空槽位、不移动其他节点**，
 * 于是所有既存句柄保持有效（除了被删的那个）。
 * 这样"删掉 n3，其他节点的 id 不变"，撤销栈与缓存键不会整体失效。
 *
 * ### 2. 任何变异失败后，图与版本号都不变（强异常保证）
 *
 * 契约里每个变异函数都有一个 `@post` 专门描述失败分支。
 * 理由：调用方（命令总线、撤销栈）必须能推理"失败了到底变了没有"。
 * 一个"可能改了一半"的变异会让撤销栈彻底失效。
 *
 * ### 3. 版本号只在**成功**变异后递增
 *
 * 这是缓存失效的唯一依据。若失败也递增，缓存会被无谓地清空；
 * 若成功不递增，缓存会返回过期结果。
 *
 * ## 明确不做的事
 *
 * | 不做 | 归谁 |
 * |---|---|
 * | 求值 | `core/graph/eval` |
 * | 缓存 | `core/graph/eval` |
 * | 节点坐标 | 视图层（按视图 id 分槽的 `view_layouts`） |
 * | 变异的历史记录 | `core/graph/mutate`（撤销栈） |
 *
 * 坐标那条尤其重要：**节点的 x/y 是布局算法的输出，不是图的性质**。
 *
 * @ownership   owns（拥有全部节点与边）
 * @thread      main（变异与读取都在主线程；求值线程读的是快照）
 * @pre         none
 * @post        none
 * @invariant   图永远无环；每个输入端口至多一条入边
 * @errors      变异函数返回 `Result<void>`，不抛
 * @frozen      否
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace qp::graph {

using qp::diag::Result;

/// @brief 节点槽位。空槽用于支持 O(1) 删除与句柄稳定性。
struct NodeSlot final {
    Node node{};
    Generation generation = 0;   ///< 0 表示空槽
    bool occupied = false;
};

/**
 * @brief 图：节点与边的容器。
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   见类文档
 * @errors      不抛
 * @frozen      否
 * @tests       graph.structure.empty_graph, graph.structure.add_node,
 *              graph.structure.add_node_uses_fresh_generation,
 *              graph.structure.remove_node_invalidates_handle,
 *              graph.structure.remove_node_drops_edges,
 *              graph.structure.connect_and_lookup,
 *              graph.structure.connect_rejects_duplicate_input,
 *              graph.structure.connect_rejects_unknown_node,
 *              graph.structure.connect_rejects_direction,
 *              graph.structure.connect_rejects_cycle,
 *              graph.structure.connect_rejects_self_loop,
 *              graph.structure.disconnect_removes_edge,
 *              graph.structure.version_bumps_on_success_only,
 *              graph.structure.failed_mutation_is_noop,
 *              graph.structure.node_count_tracks_slots,
 *              graph.structure.incoming_lookup_by_input,
 *              graph.structure.find_node_by_user_name,
 *              graph.structure.deterministic_iteration_order,
 *              graph.structure.clear_resets
 */
class Graph final {
public:
    /// @brief 构造空图。会预置一个哨兵槽（见实现说明）。
    Graph();

    // 不可复制：复制一张图意味着复制全部节点、边与世代计数器，
    // 而任何一份副本的世代都会与另一份发散——句柄将无法跨副本使用。
    // 需要快照请**显式**调用 to_snapshot()（P3 后续），语义更清楚。
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    // 但**可移动**：移动不产生第二份世代空间，句柄保持有效。
    // 这让"构造后返回""放进容器""装进撤销记录"都能正常工作。
    Graph(Graph&&) noexcept = default;
    Graph& operator=(Graph&&) noexcept = default;
    ~Graph() = default;

    // ── 节点 ────────────────────────────────────────────────────────────────

    /**
     * @brief 加入一个节点。
     *
     * @ownership   owns
     * @thread      main
     * @pre         type_name 非空
     * @post        成功时返回有效句柄，`version()` 递增，节点数 +1
     * @post        **失败时图与版本号均不变**
     * @invariant   返回的句柄世代与槽位的当前世代一致
     * @errors      Result<NodeId>；type_name 为空 → invalid_argument
     * @complexity  O(1) 摊销
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.add_node, graph.structure.add_node_uses_fresh_generation
     */
    Result<NodeId> add_node(std::string_view type_name);

    /**
     * @brief 加入一个节点并同时设置用户名字。
     *
     * @ownership   owns
     * @thread      main
     * @pre         type_name 非空；name 在本图内唯一（若非空）
     * @post        同 add_node，且 `node.name == name`
     * @post        失败时图与版本号均不变
     * @invariant   非空的用户名字全图唯一
     * @errors      invalid_argument（type_name 为空）/ duplicate_connection（名字重复）
     * @complexity  O(n)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.find_node_by_user_name
     */
    Result<NodeId> add_node_named(std::string_view type_name, std::string_view name);

    /**
     * @brief 预留一个槽位。返回的句柄**立即有效**，但类型名尚未确定。
     *
     * 为什么需要它：
     *   命令总线（`core/graph/mutate`）必须**先把命令里携带的 NodeId 变成
     *   有效句柄**，才能在应用与撤销两个方向都用同一个 id 引用节点。
     *   撤销再重做时若 id 变了，所有引用该节点的边、缓存键、UI 选中状态
     *   都会失效——这是不可接受的。
     *
     * 两阶段用法：
     * ```
     *   auto id = g.reserve_node();       // 立即有效，pending
     *   g.fill_reserved(id, "dipole");    // 补全类型名，commit
     * ```
     * 两个阶段**各自递增版本号**：中间态虽短暂，却是一个真实且可被观察到的
     * 状态（另一视图可能恰好在此刻刷新）。把中间态藏起来会让
     * "版本号变了但内容没变"变成无法解释的现象。
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        成功时返回有效句柄，版本号递增，节点数 +1，`is_stable()` 变 false
     * @post        失败时图与版本号均不变
     * @invariant   返回句柄的世代与槽位当前世代一致
     * @errors      Result<NodeId>；无参数可错，恒成功
     * @complexity  O(1) 摊销
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.reserve_and_fill, graph.structure.reserve_marks_pending
     */
    Result<NodeId> reserve_node();

    /**
     * @brief 补全一个预留槽位的类型名（commit 阶段）。
     *
     * @ownership   owns
     * @thread      main
     * @pre         id 指向一个 pending 槽位；type_name 非空
     * @post        成功时该节点类型名为 type_name、`is_stable()` 恢复 true，
     *              版本号递增
     * @post        失败时图与版本号均不变
     * @invariant   同一槽位不会被 fill 两次
     * @errors      Result<void>；id 无效 → unknown_node；type_name 为空 →
     *              invalid_argument；槽位非 pending → duplicate_connection
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.reserve_and_fill,
     *              graph.structure.fill_rejects_non_pending
     */
    Result<void> fill_reserved(NodeId id, std::string_view type_name);

    /// @brief 图中是否没有 pending 节点（即处于稳定状态）。
    [[nodiscard]] bool is_stable() const noexcept { return pending_nodes_ == 0; }

    /// @brief 当前 pending 节点数。
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_nodes_; }

    /**
     * @brief 指定句柄是否指向一个**已预留但未补全**的槽位。
     *
     * 命令总线需要它来区分两种情形：
     *   - `has_node(id) && !is_pending(id)` → 该 id 已被真实节点占用，拒绝
     *   - `has_node(id) &&  is_pending(id)` → 调用方已按约定预留，可以补全
     *
     * 只有 `has_node` 一个谓词时，这两种情形无法区分，
     * 于是"先 reserve 再 apply"这个**标准用法**会被误判为冲突。
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        句柄无效或不是 pending 时返回 false
     * @invariant   `is_pending(id)` ⟹ `has_node(id)`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.is_pending_predicate
     */
    [[nodiscard]] bool is_pending(NodeId id) const noexcept {
        const Node* n = find_node(id);
        return n != nullptr && n->type_name.empty();
    }

    /**
     * @brief 按**指定的句柄**恢复一个节点（撤销"删除"专用）。
     *
     * 为什么需要它：撤销一次删除再重做，如果节点拿到新 id，
     * 所有引用它的边、缓存键、UI 选中状态都会失效。撤销必须让
     * **id 保持不变**，否则重做后的图在语义上不是同一张图。
     *
     * 调用方（`core/graph/mutate`）负责保证句柄曾属于本图且尚未被复用；
     * 本函数仍会检查并拒绝冲突。
     *
     * @ownership   owns
     * @thread      main
     * @pre         `id.generation != 0`；槽位当前为空且世代不大于 id.generation
     * @post        成功时 `has_node(id)` 为真，节点内容为传入快照，版本号递增
     * @post        失败时图与版本号均不变
     * @invariant   恢复后 `find_node(id)->id == id`
     * @errors      Result<void>；id 无效 → invalid_argument；
     *              槽位被占用或世代冲突 → duplicate_connection
     * @complexity  O(槽位数)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.restore_node_preserves_id,
     *              graph.structure.restore_node_preserves_id
     */
    Result<void> restore_node(const Node& snapshot);

    /**
     * @brief 按**指定的端点**恢复一条边（撤销"断开"专用）。
     *
     * @ownership   owns
     * @thread      main
     * @pre         两个端点节点都存在；方向为 output → input
     * @post        成功时该边存在，版本号递增
     * @post        失败时图与版本号均不变
     * @invariant   恢复后图仍无环、该输入端口仍至多一条入边
     * @errors      Result<void>；节点不存在 → unknown_node；方向错 →
     *              invalid_argument；已有入边 → duplicate_connection；
     *              成环 → cycle_detected
     * @complexity  O(V + E)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.restore_edge_after_restore_node
     */
    Result<void> restore_edge(const Edge& e);

    /**
     * @brief 删除节点，同时删除所有与之相连的边。
     *
     * @ownership   owns
     * @thread      main
     * @pre         id 有效
     * @post        成功时该句柄失效，其槽位世代递增（老句柄永不复活），
     *              相关边全部移除，版本号递增
     * @post        失败时图与版本号均不变
     * @invariant   删除后不存在任何引用该节点的边
     * @errors      Result<void>；id 无效或已失效 → unknown_node
     * @complexity  O(V + E)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.remove_node_invalidates_handle,
     *              graph.structure.remove_node_drops_edges
     */
    Result<void> remove_node(NodeId id);

    /// @brief 取节点。句柄无效返回 nullptr。
    [[nodiscard]] const Node* find_node(NodeId id) const noexcept;
    /// @brief 取节点（可写）。仅用于设置参数与标志；**不得改 type_name**。
    [[nodiscard]] Node* find_node_mutable(NodeId id) noexcept;

    /**
     * @brief 递增版本号。供"就地修改了节点内容"的调用方使用。
     *
     * 存在的理由：`find_node_mutable` 返回的指针允许改参数与标志，
     * 而那类改动同样必须让缓存失效。若不给这个入口，调用方只有两条路：
     * 绕过版本号（缓存返回过期结果），或者伪造一次无关变异（版本号乱跳）。
     * 两条都更糟。
     *
     * @ownership   pure（只动版本号）
     * @thread      main
     * @pre         none
     * @post        `version()` 严格大于调用前
     * @invariant   版本号单调递增，永不回退
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.bump_version_is_monotonic
     */
    void bump_version() noexcept { ++version_; }

    /**
     * @brief 设置节点的用户名字。
     *
     * @ownership   owns（复制名字）
     * @thread      main
     * @pre         id 有效；非空的 name 在本图内唯一
     * @post        成功时 `find_node(id)->name == name`，版本号递增
     * @post        失败时图与版本号均不变
     * @invariant   非空的用户名字全图唯一
     * @errors      Result<void>；id 无效 → unknown_node；名字冲突 →
     *              duplicate_connection
     * @complexity  O(n)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.set_node_name_enforces_uniqueness
     */
    Result<void> set_node_name(NodeId id, std::string_view name);

    /// @brief 按用户名字取节点。找不到返回无效句柄。
    [[nodiscard]] NodeId find_node_by_name(std::string_view name) const noexcept;
    /// @brief 节点句柄是否仍然有效（世代匹配）。
    [[nodiscard]] bool has_node(NodeId id) const noexcept;

    // ── 边 ──────────────────────────────────────────────────────────────────

    /**
     * @brief 建立连接。
     *
     * 校验顺序（固定，便于测试与诊断稳定）：
     *   1. 两个句柄都有效          → unknown_node
     *   2. 方向是 output → input   → invalid_argument
     *   3. 该输入端口尚无入边      → duplicate_connection
     *   4. 不形成自环              → cycle_detected
     *   5. 不形成环                → cycle_detected
     *
     * @ownership   owns
     * @thread      main
     * @pre         none（全部由校验覆盖）
     * @post        成功时边存在，版本号递增，图仍无环
     * @post        **失败时图与版本号均不变**
     * @invariant   成功后图仍无环
     * @errors      Result<void>；见上方校验顺序
     * @complexity  O(V + E)（环检测）
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.connect_and_lookup,
     *              graph.structure.connect_rejects_duplicate_input,
     *              graph.structure.connect_rejects_unknown_node,
     *              graph.structure.connect_rejects_direction,
     *              graph.structure.connect_rejects_cycle,
     *              graph.structure.connect_rejects_self_loop
     */
    Result<void> connect(PortRef from, PortRef to);

    /**
     * @brief 断开某个输入端口上的入边。
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        成功时该输入端口的入边被移除，版本号递增
     * @post        失败（该端口本来就没有入边）时图与版本号均不变
     * @invariant   断开后该输入端口无入边
     * @errors      Result<void>；无入边 → not_connected
     * @complexity  O(E)
     * @nondet      none
     * @frozen      否
     * @tests       graph.structure.disconnect_removes_edge
     */
    Result<void> disconnect(PortRef input);

    /// @brief 查某个输入端口的入边。无入边返回 nullptr。
    [[nodiscard]] const Edge* incoming(PortRef input) const noexcept;
    /// @brief 边总数。
    [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }
    /// @brief 只读遍历全部边（顺序稳定：按加入顺序）。
    [[nodiscard]] const std::vector<Edge>& edges() const noexcept { return edges_; }

    // ── 版本与规模 ──────────────────────────────────────────────────────────

    /// @brief 当前版本号。每次成功变异递增。
    [[nodiscard]] GraphVersion version() const noexcept { return version_; }
    /// @brief 存活节点数。
    [[nodiscard]] std::size_t node_count() const noexcept { return live_nodes_; }
    /// @brief 可用槽位总数（**不含**索引 0 的哨兵槽）。
    [[nodiscard]] std::size_t slot_count() const noexcept {
        return slots_.empty() ? 0 : slots_.size() - 1;
    }

    /// @brief 只读遍历全部槽位。**含**索引 0 的哨兵槽，顺序稳定。
    ///
    /// 因此 `slots().size() == slot_count() + 1`。见 slot_count 的说明。
    [[nodiscard]] const std::vector<NodeSlot>& slots() const noexcept { return slots_; }

    /// @brief 清空整图（版本号递增）。
    void clear() noexcept;

private:
    [[nodiscard]] Node* node_at(NodeId id) noexcept;
    /// @brief 从 to 出发能否到达 from（用于连边前的环检测）。
    [[nodiscard]] bool reaches(NodeId from, NodeId target) const noexcept;

    std::vector<NodeSlot> slots_;
    std::vector<Edge> edges_;
    Generation next_generation_ = 1;
    GraphVersion version_ = 1;
    std::size_t live_nodes_ = 0;
    std::size_t pending_nodes_ = 0;   ///< 已预留但未补全类型名的槽位数
};

}  // namespace qp::graph
