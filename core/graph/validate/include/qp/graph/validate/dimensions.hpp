/**
 * @file dimensions.hpp
 * @brief 量纲解析：把端口上的 `same_as_input` 约束**解析成具体量纲**。
 *
 * ## 为什么需要解析
 *
 * 端口层（`core/ports`）对 `DimensionConstraint::same_as_input` 是**不判定**的
 * ——它需要图的上下文。典型例子：加法节点
 * "输出量纲 = 两个输入的共同量纲"，而节点描述是静态的，
 * 它只能声明"我跟输入一样"。
 *
 * 解析必须沿图的**拓扑顺序**推进：一个节点的输出量纲可能取决于
 * 上游节点的输出量纲，而上游又取决于更上游。
 *
 * ## 环怎么办
 *
 * `core/graph/structure` 已经保证图无环，因此拓扑顺序必然存在。
 * 但解析器**不能假设这一点**：它可能被用在别的容器上，
 * 或者被用在"结构校验还没跑"的时刻。
 * 因此遇到无法解析的量纲时**标记为未知并继续**，而不是崩溃或死循环。
 *
 * ## 未知量纲不是错误
 *
 * `unknown` 有两种成因，都是合法的：
 *   - 上游是个尚未定量的源（例如"由用户输入长度"）
 *   - `same_as_input` 的输入端口没有连线（此时应由别的校验报错）
 * 因此解析结果里 unknown 只是信息，是否报错由校验层决定。
 *
 * @ownership   owns
 * @thread      any（构造后只读）
 * @pre         none
 * @post        none
 * @invariant   同一张图两次解析得到相同结果（确定性）
 * @errors      noexcept
 * @frozen      否
 */
#pragma once

#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>
#include <qp/ports.hpp>
#include <qp/units.hpp>

#include <vector>

namespace qp::graph {

/// @brief 一个输出端口的解析结果。
struct ResolvedDimension final {
    NodeId node{};
    PortNumber port = 0;
    /// 解析出的量纲。`known == false` 时无意义。
    qp::units::Dim dimension{};
    /// 是否成功解析。
    bool known = false;

    [[nodiscard]] bool valid() const noexcept { return node.valid() && port != 0; }
};

/**
 * @brief 量纲解析的完整结果。
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   对同一 (node, port) 至多一条记录
 * @errors      noexcept
 * @frozen      否
 * @tests       graph.validate.dimensions_lookup, graph.validate.dimensions_unknown_for_unset
 */
class DimensionMap final {
public:
    void set(NodeId node, PortNumber port, qp::units::Dim dim) {
        entries_.push_back(ResolvedDimension{node, port, dim, true});
    }
    void set_unknown(NodeId node, PortNumber port) {
        entries_.push_back(ResolvedDimension{node, port, {}, false});
    }

    /// @brief 查询某个输出端口的解析结果。查不到返回 nullptr。
    [[nodiscard]] const ResolvedDimension* find(NodeId node, PortNumber port) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::vector<ResolvedDimension>& all() const noexcept { return entries_; }

private:
    std::vector<ResolvedDimension> entries_;
};

/**
 * @brief 量纲解析所需的上下文：节点类型目录 + 端口类型注册表。
 *
 * 两者都是只读的，因此按引用传入而不是拷贝。
 */
struct ResolveContext final {
    const INodeCatalog* catalog = nullptr;
    const qp::ports::PortTypeRegistry* types = nullptr;

    [[nodiscard]] bool valid() const noexcept { return catalog != nullptr && types != nullptr; }
};

/**
 * @brief 解析全图所有输出端口的量纲。
 *
 * @ownership   borrows（只读 ctx，不保留引用到返回之后）
 * @thread      main
 * @pre         ctx.valid()
 * @post        为每个"有输出端口且类型已注册"的节点给出解析结果；
 *              无法解析的端口也会以 `known == false` 出现（而不是缺失）
 * @invariant   确定性：同一张图两次调用得到相同结果
 * @errors      noexcept（解析失败不抛，以 unknown 表达）
 * @complexity  O(V + E)
 * @nondet      none
 * @frozen      否
 * @tests       graph.validate.resolve_dimension_from_port_type,
 *              graph.validate.resolve_same_as_input_chain,
 *              graph.validate.resolve_unknown_when_input_missing,
 *              graph.validate.resolve_is_deterministic,
 *              graph.validate.resolve_ignores_unknown_types
 */
[[nodiscard]] DimensionMap resolve_dimensions(const Graph& g, const ResolveContext& ctx) noexcept;

}  // namespace qp::graph
