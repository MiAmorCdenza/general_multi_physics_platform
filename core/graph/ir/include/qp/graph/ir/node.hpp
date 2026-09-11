/**
 * @file node.hpp
 * @brief 图中的节点实例：类型 + 参数值 + 位置无关的身份。
 *
 * ## 关键区分：`NodeDesc` 是类型，`Node` 是实例
 *
 * `NodeDesc`（见 descriptor.hpp）描述"偶极场节点长什么样"，
 * 全图只有一份。`Node` 描述"图里第 3 个偶极场节点，参数是这些值"，
 * 每个实例一份。
 *
 * 把两者混为一谈是常见错误：它会导致"改一个节点的端口定义，全图所有
 * 同类型节点一起变"，而用户完全无法理解发生了什么。
 *
 * ## 节点不持有状态
 *
 * `Node` 里只有**声明**：类型、参数、是否绕过。
 * 求值中间态属于求值上下文，时间步进属于运行，缓存属于缓存存储。
 * 一个持有可变状态的节点会让缓存失效判定变成不可解的问题。
 *
 * @frozen 否（可扩；已有字段语义冻结）
 */
#pragma once

#include <qp/graph/ir/ids.hpp>
#include <qp/ports/value.hpp>

#include <string>
#include <vector>

namespace qp::graph {

/// @brief 节点的一个参数值（按端口号索引）。
struct ParamValue final {
    PortNumber number = 0;
    qp::ports::Value value{};
};

/**
 * @brief 图中的一个节点实例。
 *
 * @ownership   owns
 * @thread      main（图的变异与读取都在主线程；求值线程读快照）
 * @pre         none
 * @post        none
 * @invariant   `type_name` 在实例存续期内不变（改类型 = 删旧建新）
 * @errors      noexcept
 * @frozen      否
 * @tests       graph.node.construction, graph.node.param_lookup,
 *              graph.node.set_param_replaces, graph.node.bypass_flag,
 *              graph.node.user_name_is_separate_from_id
 */
struct Node final {
    NodeId id{};

    /// 类型名。指向 `NodeDesc::type_name` 的**副本**——描述可能被热重载，
    /// 而实例应当记住自己当初是哪种类型。
    std::string type_name;

    /// 面向用户的稳定名字（YAML 里的键、报告里的引用）。
    ///
    /// 与 `id` 分开：`id` 是内部寻址句柄（含世代，会因删除而失效），
    /// `name` 是给人看的标签。两者都可为空/默认，但只有 `id` 参与寻址。
    std::string name;

    std::vector<ParamValue> params;

    /// 绕过该节点：求值时直接把匹配的输入透传到输出。
    /// 用于"临时禁用某个滤波节点"而不必断开连线。
    bool bypassed = false;

    /// 求值顺序提示。仅在拓扑排序有多解时作为次要判据，
    /// **不改变拓扑约束**（图永远无环）。
    std::int32_t order_hint = 0;

    /// @brief 取参数值。未设置返回无效 Value。
    [[nodiscard]] qp::ports::Value param(PortNumber number) const noexcept {
        for (const auto& p : params) {
            if (p.number == number) return p.value;
        }
        return qp::ports::Value{};
    }

    /// @brief 设置参数值。已存在则替换，否则追加。
    ///
    /// @ownership   owns（复制值）
    /// @thread      main
    /// @pre         number != 0
    /// @post        `param(number) == value`
    /// @invariant   同一 number 至多出现一次
    /// @errors      noexcept
    /// @complexity  O(n)
    /// @nondet      none
    /// @frozen      否
    /// @tests       graph.node.set_param_replaces
    void set_param(PortNumber number, qp::ports::Value value) {
        for (auto& p : params) {
            if (p.number == number) {
                p.value = std::move(value);
                return;
            }
        }
        params.push_back(ParamValue{number, std::move(value)});
    }

    /// @brief 删除参数值。返回是否确实删除。
    bool erase_param(PortNumber number) noexcept {
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (it->number == number) {
                params.erase(it);
                return true;
            }
        }
        return false;
    }
};

}  // namespace qp::graph
