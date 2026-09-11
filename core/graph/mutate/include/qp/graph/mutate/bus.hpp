/**
 * @file bus.hpp
 * @brief 命令总线：**全部**图变异的唯一入口，并持有唯一的撤销栈。
 *
 * ## 为什么撤销栈在地基里，而不是在编辑器插件里
 *
 * 这是 `docs/plan-tree.md` §5.3 的核心结论，值得复述一遍：
 *
 * 用户可以在节点编辑器、YAML 文本视图、积木视图里改同一张图。
 * 如果撤销栈住在编辑器里，就会出现三种故障：
 *   - YAML 视图里的修改**无法被撤销**（它的命令没进那个栈）；
 *   - 两个视图各撤各的，文档在两者之间来回跳；
 *   - 切换视图后撤销历史丢失。
 *
 * 因此：**撤销栈只有一个，它属于图的拥有者**。视图只负责把手势翻译成命令。
 *
 * ## 撤销靠"逆命令"，不靠快照
 *
 * 每次成功应用命令时，总线同时在**同一时刻**捕获逆向操作所需的数据。
 * 捕获必须在应用前完成（例如 `SetParam` 要记住旧值），
 * 因此实现里"先读旧值，再应用"这个顺序是契约的一部分。
 *
 * 相比整图快照：内存从 O(图大小) 降到 O(单次操作)，
 * 而且"撤销了什么"是可读的（`command_name()`）。
 *
 * ## 同节点合并
 *
 * 拖动滑块会连续产生几十条 `SetParam`。逐条撤销意味着用户要按几十次
 * Ctrl+Z 才能回到拖之前的状态——这不是撤销，是惩罚。
 * 因此 `UndoStack` 支持把同一节点的连续同类命令合并成一条。
 *
 * @ownership   owns（拥有图与撤销栈）
 * @thread      main（全部方法）
 * @pre         none
 * @post        none
 * @invariant   图的任何结构变更都必须经过本类；撤销栈与图状态严格对应
 * @errors      不抛；失败走 Result
 * @frozen      否
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
 * @brief 一条可撤销的编辑记录。
 *
 * 内部持有一对"应用/回退"操作。对是**闭包**，但只捕获命令与捕获到的
 * 旧状态，不捕获图指针——图由总线持有，记录本身可在总线之外传递。
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   revert 与 apply 对同一个图互为逆操作
 * @errors      不抛
 * @frozen      否
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
    /// @brief 本条记录针对的节点。用于同节点合并判定。
    [[nodiscard]] NodeId target() const noexcept { return target_; }

    /// @brief 是否可以与紧随其后的一条合并。
    [[nodiscard]] bool mergeable_with(const UndoEntry& next) const noexcept {
        return target_.valid() && target_ == next.target_ && label_ == next.label_;
    }

    /// @brief 与下一条合并（下一条成为新的 apply，本条保留 revert）。
    void merge_with(const UndoEntry& next) {
        apply_ = next.apply_;
    }

    /// @brief 应用（重做）。
    Result<void> apply(Graph& g) const { return apply_(g); }
    /// @brief 回退（撤销）。
    Result<void> revert(Graph& g) const { return revert_(g); }

private:
    std::string label_;
    NodeId target_{};
    std::function<Result<void>(Graph&)> apply_;
    std::function<Result<void>(Graph&)> revert_;
};

/**
 * @brief 撤销/重做栈。只能通过 `CommandBus` 使用。
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `undo_size() + redo_size()` 单调反映已记录的编辑数
 * @errors      不抛
 * @frozen      否
 * @tests       graph.mutate.undo_redo_roundtrip, graph.mutate.new_edit_clears_redo,
 *              graph.mutate.merge_consecutive_set_param
 */
class UndoStack final {
public:
    /// @brief 记录一条编辑。会清空重做栈。
    void push(UndoEntry entry, bool allow_merge);

    /// @brief 撤销一步。无历史返回 false。
    Result<bool> undo(Graph& g);
    /// @brief 重做一步。无可重做返回 false。
    Result<bool> redo(Graph& g);

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undo_size() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_size() const noexcept { return redo_.size(); }

    /// @brief 下一条将被撤销的编辑的标签。空栈返回空串。
    [[nodiscard]] const std::string& next_undo_label() const noexcept;
    /// @brief 下一条将被重做的编辑的标签。空栈返回空串。
    [[nodiscard]] const std::string& next_redo_label() const noexcept;

    /// @brief 清空两侧历史。
    void clear() noexcept;

private:
    std::vector<UndoEntry> undo_;
    std::vector<UndoEntry> redo_;
};

/**
 * @brief 命令总线的应用结果。
 */
struct ApplyOutcome final {
    /// 命令影响的节点（若有）。
    std::optional<NodeId> node{};
    /// 图的新版本号。
    GraphVersion version = 0;
    /// 是否真的改变了图（幂等命令可能不改变，例如把参数设成同一个值）。
    bool changed = false;
};

/**
 * @brief 命令总线：图的唯一编辑入口 + 唯一撤销栈。
 *
 * @ownership   owns（图与撤销栈）
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   成功应用后版本号严格递增；失败时图与版本号均不变
 * @errors      不抛
 * @frozen      否
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
     * @brief 预留一个新节点的 id（供 `AddNode` 使用）。
     *
     * @ownership   owns（占用图中的一个槽位）
     * @thread      main
     * @pre         图处于稳定状态（`is_stable()`）
     * @post        成功时返回的句柄在图里立即有效但处于 pending 状态
     * @post        失败时（图不稳定）图与版本号均不变
     * @invariant   同一 id 不会被分配给两次
     * @errors      Result<NodeId>；图不稳定 → graph_busy
     * @complexity  O(1) 摊销
     * @nondet      none
     * @frozen      否
     * @tests       graph.mutate.reserve_id_is_stable_across_undo,
     *              graph.mutate.rejects_apply_while_unstable
     */
    Result<NodeId> reserve_node();

    /**
     * @brief 应用一条命令。
     *
     * @ownership   borrows（不保留命令或图以外的引用）
     * @thread      main
     * @pre         图处于稳定状态
     * @post        成功时图已变更、版本号递增、撤销栈压入一条记录
     * @post        **失败时图、版本号与撤销栈均不变**
     * @invariant   成功返回的 outcome.node 与 command_target 一致
     * @errors      Result<ApplyOutcome>；见各命令的校验规则
     * @complexity  取决于命令；Connect 为 O(V+E)
     * @nondet      none
     * @frozen      否
     * @tests       graph.mutate.apply_add_node, graph.mutate.apply_remove_node,
     *              graph.mutate.apply_set_param, graph.mutate.apply_connect,
     *              graph.mutate.apply_rejects_invalid
     */
    Result<ApplyOutcome> apply(const Command& c, bool allow_merge = true);

    /// @brief 撤销一步。无历史时返回 false（不是错误）。
    Result<bool> undo() { return undo_.undo(*graph_); }
    /// @brief 重做一步。无历史时返回 false（不是错误）。
    Result<bool> redo() { return undo_.redo(*graph_); }

    [[nodiscard]] bool can_undo() const noexcept { return undo_.can_undo(); }
    [[nodiscard]] bool can_redo() const noexcept { return undo_.can_redo(); }
    [[nodiscard]] const UndoStack& history() const noexcept { return undo_; }

    /// @brief 只读访问被拥有的图。**变异必须走 apply()。**
    ///
    /// 刻意不提供非 const 访问器：那样"全部变异走总线"这条不变量
    /// 就退化成了口头约定。
    [[nodiscard]] const Graph& graph() const noexcept { return *graph_; }

private:
    Graph* graph_;
    UndoStack undo_;
};

}  // namespace qp::graph
