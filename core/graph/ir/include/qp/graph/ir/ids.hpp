/**
 * @file ids.hpp
 * @brief 图元素的标识：**句柄 + 世代**，不是裸整数。
 *
 * ## 为什么不用裸索引，也不用字符串 ID
 *
 * 裸索引：删掉节点 n3 后再加一个新节点会复用索引 3，于是所有指向老 n3 的
 * 引用（撤销栈、缓存键、UI 选中状态、诊断记录）会**静默指向新节点**。
 * 这是最难查的一类 bug——它不崩溃，只是改错了对象。
 *
 * 字符串 ID：学生手册里的 `n3` 应当稳定且可读，但字符串做键会让每次
 * 求值都付出哈希与比较成本，而且容易在重命名时破坏引用。
 *
 * 因此：内部用 `(index, generation)`，世代在槽位复用时递增。
 * 老引用因而变成"可检测的失效"而不是"静默指向别人"。
 *
 * 面向用户与 YAML 的稳定名字是**另一层**（`NodeDesc::name`），
 * 不参与内部寻址。
 *
 * @frozen 是（`index`/`generation` 的语义与零值含义冻结）
 */
#pragma once

#include <cstdint>

namespace qp::graph {

/// @brief 槽位索引。0 表示"无"。
using SlotIndex = std::uint32_t;

/// @brief 世代号。槽位每次复用时递增，用于识别失效句柄。
using Generation = std::uint32_t;

/// @brief 无效索引。
inline constexpr SlotIndex kNoSlot = 0;

/// @brief 无效世代。
inline constexpr Generation kNoGeneration = 0;

/**
 * @brief 图节点句柄。
 *
 * @ownership   pure（值类型，可随意复制）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   默认构造的句柄无效
 * @errors      noexcept
 * @frozen      是
 * @tests       graph.ids.node_default_is_invalid, graph.ids.node_equality,
 *              graph.ids.node_generation_matters
 */
struct NodeId final {
    SlotIndex index = kNoSlot;
    Generation generation = kNoGeneration;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != kNoSlot && generation != kNoGeneration;
    }

    [[nodiscard]] friend constexpr bool operator==(NodeId a, NodeId b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    [[nodiscard]] friend constexpr bool operator!=(NodeId a, NodeId b) noexcept {
        return !(a == b);
    }
    /// @brief 供有序容器使用。仅比较数值，**不代表语义顺序**。
    [[nodiscard]] friend constexpr bool operator<(NodeId a, NodeId b) noexcept {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    }
};

/// @brief 节点内端口的序号。0 表示"无"。
///
/// 端口序号在节点类型内**稳定**：`NodeDesc::inputs[i]` 的序号就是 `i + 1`。
/// 这样端口名可以改（面向用户），而内部引用不变。
using PortIndex = std::uint32_t;

inline constexpr PortIndex kNoPort = 0;

/// @brief 端口方向。
enum class PortDirection : std::uint8_t {
    input = 0,
    output = 1,
};

/**
 * @brief 指向某个节点某个端口的位置。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   默认构造的位置无效
 * @errors      noexcept
 * @frozen      是
 * @tests       graph.ids.port_ref_default_is_invalid, graph.ids.port_ref_equality
 */
struct PortRef final {
    NodeId node{};
    PortIndex port = kNoPort;
    PortDirection direction = PortDirection::input;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return node.valid() && port != kNoPort;
    }

    [[nodiscard]] friend constexpr bool operator==(PortRef a, PortRef b) noexcept {
        return a.node == b.node && a.port == b.port && a.direction == b.direction;
    }
    [[nodiscard]] friend constexpr bool operator!=(PortRef a, PortRef b) noexcept {
        return !(a == b);
    }
};

/// @brief 图的版本号。任何结构性变异都递增。
///
/// 用途：缓存失效判定、撤销栈、UI 增量刷新、"这份快照对应哪一版图"。
using GraphVersion = std::uint64_t;

}  // namespace qp::graph
