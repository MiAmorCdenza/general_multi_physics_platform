/**
 * @file edge.hpp
 * @brief 边：一条从输出端口到输入端口的连接。
 *
 * ## 为什么边是不可变的
 *
 * 改变一条边的端点等于"删一条边 + 加一条边"。若允许原地修改：
 *   - 缓存失效判定要区分"端点改了"与"边被换掉"；
 *   - 撤销栈要记录旧值，而旧值指向的节点可能已被删除（悬垂）；
 *   - 多视图同步时，"这条边被改过"与"这条边是新边"语义不同。
 *
 * 不可变让这三件事都退化成同一件事：**旧边消失、新边出现**。
 *
 * ## 一个输入端口至多一条入边
 *
 * 这是**结构性不变量**，不是约定。多条入边会让"这个输入取哪个值"
 * 变成未定义；有些节点图系统用隐式的"最后连的赢"来掩盖它，
 * 那会在学生改连线顺序时产生难以复现的结果差异。
 *
 * 由 `core/graph/structure` 强制，并在连接时返回
 * `ErrorCode::duplicate_connection`。
 *
 * @frozen 是
 */
#pragma once

#include <qp/graph/ir/ids.hpp>

namespace qp::graph {

/**
 * @brief 一条连接。
 *
 * @ownership   pure（值类型）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `from` 必为输出端口，`to` 必为输入端口
 * @errors      noexcept
 * @frozen      是
 * @tests       graph.edge.construction, graph.edge_equality,
 *              graph.edge_direction_invariant
 */
struct Edge final {
    /// 源：必须是**输出**端口。
    PortRef from{};
    /// 目标：必须是**输入**端口。
    PortRef to{};

    [[nodiscard]] bool valid() const noexcept {
        return from.valid() && to.valid() &&
               from.direction == PortDirection::output &&
               to.direction == PortDirection::input;
    }

    [[nodiscard]] friend constexpr bool operator==(const Edge& a, const Edge& b) noexcept {
        return a.from == b.from && a.to == b.to;
    }
    [[nodiscard]] friend constexpr bool operator!=(const Edge& a, const Edge& b) noexcept {
        return !(a == b);
    }
};

}  // namespace qp::graph
