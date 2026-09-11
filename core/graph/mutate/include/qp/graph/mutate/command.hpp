/**
 * @file command.hpp
 * @brief 图变异命令：**全部**结构变更的唯一表达形式。
 *
 * ## 为什么用一个显式命令类型，而不是直接暴露 Graph 的变异方法
 *
 * 三个理由，每个都独立成立：
 *
 * 1. **撤销/重做需要可存储的操作**。`Graph::connect()` 调用完之后什么都没留下；
 *    一个 `Command` 值可以被存进撤销栈、被序列化、被发送到另一个视图。
 *
 * 2. **多视图必须共用一条编辑通道**。节点编辑器、YAML 视图、积木视图
 *    如果各自直接改 `Graph`，就没有任何地方能统一校验、统一失效、统一撤销。
 *    命令是那条通道。
 *
 * 3. **命令是可校验、可预演、可记录的数据**。这三点让"这个操作能不能做"
 *    与"做完之后图是什么样"可以在**不真的改图**的前提下回答。
 *
 * ## 命令是值，不是回调
 *
 * 命令里不含函数指针、不含闭包、不含 `this`。它是纯数据，
 * 因此可以比较、可以打印、可以存进文件。回调式命令做不到这些，
 * 而且会把生命周期问题带进撤销栈。
 *
 * @ownership   pure（值类型）
 * @thread      any（构造后只读）
 * @pre         none
 * @post        none
 * @invariant   命令自身不含任何指向图的引用
 * @errors      noexcept
 * @frozen      否（可扩；已有变体的语义冻结）
 */
#pragma once

#include <qp/graph/ir.hpp>
#include <qp/ports/value.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace qp::graph {

/// @brief 节点被创建时的完整初始状态。用于撤销"删除节点"时精确还原。
struct NodeSnapshot final {
    NodeId id{};
    std::string type_name;
    std::string name;
    std::vector<ParamValue> params;
    bool bypassed = false;

    [[nodiscard]] friend bool operator==(const NodeSnapshot& a,
                                         const NodeSnapshot& b) noexcept {
        return a.id == b.id && a.type_name == b.type_name && a.name == b.name &&
               a.bypassed == b.bypassed && a.params.size() == b.params.size() &&
               std::equal(a.params.begin(), a.params.end(), b.params.begin(),
                          [](const ParamValue& x, const ParamValue& y) {
                              return x.number == y.number && x.value == y.value;
                          });
    }
};

/// @brief 一条边的完整记录。用于撤销"断开"时精确还原。
struct EdgeSnapshot final {
    PortRef from{};
    PortRef to{};

    [[nodiscard]] friend constexpr bool operator==(EdgeSnapshot a, EdgeSnapshot b) noexcept {
        return a.from == b.from && a.to == b.to;
    }
};

/// @brief 新增节点。
struct AddNode final {
    /// 由调用方提供（通常来自 `CommandBus::reserve_node()`）。
    /// **不是**命令自己生成的：这样撤销再重做时 id 保持不变。
    NodeId id{};
    std::string type_name;
    std::string name;

    [[nodiscard]] friend bool operator==(const AddNode& a, const AddNode& b) noexcept {
        return a.id == b.id && a.type_name == b.type_name && a.name == b.name;
    }
};

/// @brief 删除节点（连同其全部边）。
struct RemoveNode final {
    NodeId id{};

    [[nodiscard]] friend constexpr bool operator==(RemoveNode a, RemoveNode b) noexcept {
        return a.id == b.id;
    }
};

/// @brief 设置节点参数。
struct SetParam final {
    NodeId id{};
    PortNumber port = 0;
    qp::ports::Value value{};

    [[nodiscard]] friend bool operator==(const SetParam& a, const SetParam& b) noexcept {
        return a.id == b.id && a.port == b.port && a.value == b.value;
    }
};

/// @brief 删除节点参数（恢复为"未设置"）。
struct EraseParam final {
    NodeId id{};
    PortNumber port = 0;

    [[nodiscard]] friend constexpr bool operator==(EraseParam a, EraseParam b) noexcept {
        return a.id == b.id && a.port == b.port;
    }
};

/// @brief 设置节点的用户名字。
struct SetNodeName final {
    NodeId id{};
    std::string name;

    [[nodiscard]] friend bool operator==(const SetNodeName& a,
                                         const SetNodeName& b) noexcept {
        return a.id == b.id && a.name == b.name;
    }
};

/// @brief 设置节点的绕过标志。
struct SetBypass final {
    NodeId id{};
    bool bypassed = false;

    [[nodiscard]] friend constexpr bool operator==(SetBypass a, SetBypass b) noexcept {
        return a.id == b.id && a.bypassed == b.bypassed;
    }
};

/// @brief 建立连接。
struct Connect final {
    PortRef from{};
    PortRef to{};

    [[nodiscard]] friend constexpr bool operator==(Connect a, Connect b) noexcept {
        return a.from == b.from && a.to == b.to;
    }
};

/// @brief 断开某个输入端口的入边。
struct Disconnect final {
    PortRef input{};

    [[nodiscard]] friend constexpr bool operator==(Disconnect a, Disconnect b) noexcept {
        return a.input == b.input;
    }
};

/// @brief 所有命令变体。新增操作必须在此登记——这是唯一入口。
using Command = std::variant<AddNode, RemoveNode, SetParam, EraseParam, SetNodeName, SetBypass,
                             Connect, Disconnect>;

/// @brief 命令的稳定短名。用于日志、撤销栈展示、测试断言。
[[nodiscard]] inline const char* command_name(const Command& c) noexcept {
    return std::visit(
        [](const auto& v) -> const char* {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, AddNode>) return "AddNode";
            else if constexpr (std::is_same_v<T, RemoveNode>) return "RemoveNode";
            else if constexpr (std::is_same_v<T, SetParam>) return "SetParam";
            else if constexpr (std::is_same_v<T, EraseParam>) return "EraseParam";
            else if constexpr (std::is_same_v<T, SetNodeName>) return "SetNodeName";
            else if constexpr (std::is_same_v<T, SetBypass>) return "SetBypass";
            else if constexpr (std::is_same_v<T, Connect>) return "Connect";
            else if constexpr (std::is_same_v<T, Disconnect>) return "Disconnect";
            else return "unknown";
        },
        c);
}

/// @brief 命令影响的节点（若有）。撤销栈用它做"同节点合并"。
[[nodiscard]] inline std::optional<NodeId> command_target(const Command& c) noexcept {
    return std::visit(
        [](const auto& v) -> std::optional<NodeId> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, AddNode>) return v.id;
            else if constexpr (std::is_same_v<T, RemoveNode>) return v.id;
            else if constexpr (std::is_same_v<T, SetParam>) return v.id;
            else if constexpr (std::is_same_v<T, EraseParam>) return v.id;
            else if constexpr (std::is_same_v<T, SetNodeName>) return v.id;
            else if constexpr (std::is_same_v<T, SetBypass>) return v.id;
            else if constexpr (std::is_same_v<T, Connect>) return v.to.node;
            else if constexpr (std::is_same_v<T, Disconnect>) return v.input.node;
            else return std::nullopt;
        },
        c);
}

}  // namespace qp::graph
