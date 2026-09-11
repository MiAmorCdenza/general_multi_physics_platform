/**
 * @file validate.hpp
 * @brief 加载期校验：打开实验时就报错，而不是算了十分钟曲线荒谬。
 *
 * ## 校验为什么必须在加载期
 *
 * 类型与量纲错误若推迟到求值期，症状是"学生算了十分钟，结果荒谬"。
 * 加载期拒绝的代价是"打开实验时弹一个错误框"。两者对课堂的影响
 * 差一个数量级——这条是 `docs/plan-tree.md` 里 `validate` 模块的全部理由。
 *
 * ## 校验的五个层次
 *
 * 1. **结构**：节点类型已注册、每条边指向的节点与端口真实存在、
 *    类型字段非空。这一层只用图本身。
 * 2. **端口存在性**：连线两端的端口号必须在 `NodeDesc` 里真实存在。
 *    图结构层无法检查它——它不认识 `NodeDesc`。
 * 3. **连接兼容**：把端口层的 `check_connection` 应用到每条边上。
 *    这一步才真正用到 `same_as_input` 的解析结果。
 * 4. **必需参数**：标记为 `required` 且不可连线的端口必须有值。
 * 5. **域许可**：节点是否允许出现在它被放置的域里
 *    （实时域禁止阻塞/分配，因此默认拒绝）。
 *
 * ## 为什么一次报全部问题
 *
 * 在第一个错误处停下，用户要改 N 次才能打开一个实验。
 * 因此校验**收集全部问题**再一起返回，每条都带节点与端口定位。
 *
 * @ownership   pure（不修改图）
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   校验是只读的：调用前后图与版本号完全不变
 * @errors      noexcept（问题以 Report 表达，不抛）
 * @frozen      否
 */
#pragma once

#include <qp/graph/structure.hpp>
#include <qp/graph/validate/dimensions.hpp>
#include <qp/graph/validate/report.hpp>

namespace qp::graph {

/// @brief 图所处的域。不同域对节点能力的要求不同。
enum class Domain : std::uint8_t {
    /// 烘焙（field）：离线、可阻塞、可分配。
    field = 0,
    /// 实时（particle）：每帧执行，禁止阻塞与分配。
    particle = 1,
    /// 渲染（render）：纯声明，不求值。
    render = 2,
};

[[nodiscard]] constexpr const char* to_string(Domain d) noexcept {
    switch (d) {
        case Domain::field: return "field";
        case Domain::particle: return "particle";
        case Domain::render: return "render";
    }
    return "unknown";
}

/**
 * @brief 校验选项。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   默认值即"最严格的合理设置"
 * @errors      noexcept
 * @frozen      否
 */
struct ValidateOptions final {
    /// 图所在的域。决定域许可检查。
    Domain domain = Domain::field;
    /// 是否把"输出端口未连接"视为警告。
    ///
    /// 默认 **false**：一片叶子输出不连是正常用法（用户只关心其中一个结果）。
    /// 打开后适合"实验模板完整性"检查。
    bool warn_unconnected_outputs = false;
    /// 是否要求所有必需参数就位。默认 true。
    bool require_required_params = true;
};

/**
 * @brief 校验整张图。
 *
 * 只读：调用前后图与版本号**完全不变**（有测试断言）。
 *
 * @ownership   borrows（只读 g 与 ctx）
 * @thread      main
 * @pre         ctx.valid()
 * @post        返回的 Report 汇总**全部**发现的问题，而不是第一个
 * @invariant   校验不修改图；同一张图两次校验得到相同结果
 * @errors      noexcept
 * @complexity  O(V + E)
 * @nondet      none
 * @frozen      否
 * @tests       graph.validate.ok_on_consistent_graph,
 *              graph.validate.rejects_unknown_node_type,
 *              graph.validate.rejects_missing_port,
 *              graph.validate.rejects_dimension_mismatch_on_edge,
 *              graph.validate.rejects_type_mismatch_on_edge,
 *              graph.validate.rejects_missing_required_param,
 *              graph.validate.rejects_domain_violation,
 *              graph.validate.collects_all_issues,
 *              graph.validate.is_readonly,
 *              graph.validate.warns_on_any_port,
 *              graph.validate.warns_on_unconnected_output_when_enabled,
 *              graph.validate.deterministic,
 *              graph.validate.allows_unconnected_optional_input,
 *              graph.validate.rejects_dangling_edge_to_deleted_node
 */
[[nodiscard]] Report validate_graph(const Graph& g, const ResolveContext& ctx,
                                    const ValidateOptions& options = {});

/**
 * @brief 校验"能不能把这条边接上去"，用于连线前的预演。
 *
 * 与 `validate_graph` 的区别：它回答的是**单个动作**能否执行，
 * 因此 `core/graph/mutate` 可以在真正改图之前给出理由。
 *
 * @ownership   borrows
 * @thread      main
 * @pre         ctx.valid()
 * @post        返回空 Report 表示可以连；否则给出拒绝原因
 * @invariant   不修改图
 * @errors      noexcept
 * @complexity  O(V + E)
 * @nondet      none
 * @frozen      否
 * @tests       graph.validate.check_edge_ok, graph.validate.check_edge_rejects_mismatch,
 *              graph.validate.check_edge_rejects_unknown_ports
 */
[[nodiscard]] Report check_edge(const Graph& g, const ResolveContext& ctx, PortRef from,
                                PortRef to);

}  // namespace qp::graph
