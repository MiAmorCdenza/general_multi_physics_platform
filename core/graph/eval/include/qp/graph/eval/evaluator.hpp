/**
 * @file evaluator.hpp
 * @brief 求值：拉取式调度 + 内容寻址缓存。
 *
 * ## 为什么是"拉取式"而不是"推送式"
 *
 * 推送式（从源节点沿边往下算）会算出**整张图**，包括当前视图根本不看的
 * 分支。一张图里通常只有少数几个节点是"被声明的输出"，
 * 拉取式只算到达它们的子图。
 *
 * 课堂现场的意义：老师调一个参数，只有真正影响当前画面的分支被重算。
 *
 * ## 求值必须确定性
 *
 * 同一张图、同一批参数，两次求值给出**逐位相同**的结果（章程 R2 的精神）。
 * 因此：
 *   - 拓扑序在有多个可选顺序时按槽位索引打破平局（顺序固定）；
 *   - 缓存键不含地址、时间、随机数；
 *   - 节点实现被要求是纯函数（`INodeEvaluator` 的契约）。
 *
 * ## 单线程
 *
 * 求值在**主线程**进行。多线程求值是后续的事（需要每节点独立的上下文），
 * 现在不做——把并发问题留到有真实性能需求时再解决，
 * 而不是提前引入无法验证的复杂度。
 *
 * @ownership   owns（缓存与统计）
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   求值不修改图与版本号
 * @errors      失败走 Result，不抛
 * @frozen      否
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/eval/cache.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>
#include <qp/ports.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace qp::graph {

using qp::diag::Result;

/**
 * @brief 节点实现的接口。
 *
 * 契约（与 `INodeEvaluator` 的实现者之间的约定）：
 *   - **纯函数**：同输入同输出；不依赖时间、随机数、全局状态。
 *   - **不得修改输入**。
 *   - **不得访问图**：它只看到自己的输入值与参数。
 *   - **不得抛异常**（热路径禁异常；失败用 `Result` 表达）。
 *
 * 这些约束不是"建议"：缓存的存在**要求**纯函数性——
 * 一个依赖隐藏状态的节点会命中错误的缓存条目。
 */
class INodeEvaluator {
public:
    INodeEvaluator() = default;
    virtual ~INodeEvaluator() = default;
    INodeEvaluator(const INodeEvaluator&) = delete;
    INodeEvaluator& operator=(const INodeEvaluator&) = delete;

    /**
     * @brief 计算一个节点的输出。
     *
     * @ownership   pure（不得保留对 inputs 的引用）
     * @thread      main
     * @pre         节点类型与 desc 一致
     * @post        返回该节点全部输出端口的值
     * @invariant   同 inputs 必得同 outputs
     * @errors      Result；失败时返回错误码，不抛
     * @complexity  由实现决定
     * @nondet      **必须 none**——否则缓存会给出错误结果
     * @frozen      是
     */
    [[nodiscard]] virtual Result<std::vector<std::pair<PortNumber, qp::ports::Value>>> evaluate(
        NodeId id, const NodeDesc& desc,
        const std::vector<std::pair<PortNumber, qp::ports::Value>>& inputs) = 0;
};

/// @brief 求值所需的外部依赖。
struct EvalContext final {
    const INodeCatalog* catalog = nullptr;
    const qp::ports::PortTypeRegistry* types = nullptr;
    INodeEvaluator* evaluator = nullptr;
    EvalCache* cache = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return catalog != nullptr && types != nullptr && evaluator != nullptr;
    }
};

/// @brief 一次求值的统计。用于性能观察与"为什么这么慢"的回答。
struct EvalStats final {
    std::size_t nodes_visited = 0;    ///< 拓扑序里被访问的节点数
    std::size_t nodes_computed = 0;   ///< 真正调用实现的次数
    std::size_t cache_hits = 0;       ///< 命中缓存的次数
    std::size_t nodes_skipped = 0;    ///< 被绕过（bypass）的节点数
};

/**
 * @brief 一次求值的结果：按 (节点, 端口) 索引的全部输出。
 */
class EvalResult final {
public:
    struct Output final {
        NodeId node{};
        PortNumber port = 0;
        qp::ports::Value value{};
    };

    void set(NodeId node, PortNumber port, qp::ports::Value v) {
        outputs_.push_back(Output{node, port, std::move(v)});
    }

    /// @brief 查询某个输出端口的值。未求值返回无效值。
    [[nodiscard]] qp::ports::Value get(NodeId node, PortNumber port) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return outputs_.size(); }
    [[nodiscard]] const std::vector<Output>& all() const noexcept { return outputs_; }

private:
    std::vector<Output> outputs_;
};

/**
 * @brief 求值整张图。
 *
 * 关键行为：
 *   - 只算"有下游或自身为终点的子图"？**不**——当前实现算全图。
 *     拉取式剪枝是后续优化（需要"声明输出"信息，见 `core/graph/domain`）。
 *     现在算全图，但**缓存**确保无关分支只在第一次付出代价。
 *   - 节点的参数与输入共同构成缓存键；任何一项变化都会导致重算。
 *   - `bypassed` 的节点不调用实现：把第 1 个输入直接透传到第 1 个输出。
 *
 * @ownership   borrows（只读 g 与 ctx）
 * @thread      main
 * @pre         ctx.valid()
 * @post        成功时返回全部节点的输出；图与版本号不变
 * @post        失败时返回值未定义，但图未被修改
 * @invariant   确定性：同一图同一参数两次求值结果逐位相同
 * @errors      Result；节点实现失败或类型未注册时返回错误码
 * @complexity  O(V + E) 加各节点实现的开销
 * @nondet      none
 * @frozen      否
 * @tests       graph.eval.single_node, graph.eval.chain_propagates,
 *              graph.eval.deterministic_across_runs,
 *              graph.eval.cache_hit_on_second_run,
 *              graph.eval.param_change_invalidates_only_downstream,
 *              graph.eval.unknown_type_fails, graph.eval.missing_param_fails,
 *              graph.eval.bypass_passthrough, graph.eval.diamond_evaluates_once,
 *              graph.eval.is_readonly, graph.eval.result_lookup
 */
[[nodiscard]] Result<EvalStats> evaluate_graph(const Graph& g, const EvalContext& ctx,
                                               EvalResult& out);

}  // namespace qp::graph
