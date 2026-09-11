/**
 * @file descriptor.hpp
 * @brief 节点类型的描述：`NodeDesc` / `PortDesc` / `ParamDesc`。
 *
 * ## 核心决策：**Param 与 Port 统一**
 *
 * 旧工程的 `engine/ports.py` 里，`Port` 与 `Param` 是两套机制：
 * `Port` 既是连线插座又是参数，而 `Param` 是不参与连线的参数。
 * 两者都要单独实现校验、单独实现 UI、单独实现序列化——同一件事做了两遍。
 *
 * 本项目统一为 `PortDesc`，用一个 `connectable` 标志区分：
 *   - `connectable == true`  ：可连线（有插座）
 *   - `connectable == false` ：只能填值（"参数"）
 *
 * 好处：校验、UI 渲染、YAML 序列化、类型检查全部只有一条代码路径。
 * 学生把一个参数连上线的行为因此**天然被支持或天然被拒绝**，
 * 而不是"另一套机制里没实现"。
 *
 * ## 节点只有三个可选钩子
 *
 * `compute` / `validate` / `on_param`。这是**上限**，不是起点。
 * 加第四个钩子前必须走 ADR：每加一个钩子，所有插件作者都要理解它。
 *
 * @frozen 是（`NodeDesc` 的形状冻结；新增可选字段需走 ADR）
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>
#include <qp/units.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace qp::graph {

/// @brief 端口/参数的序号（1 起，0 表示无）。
using PortNumber = std::uint32_t;

/**
 * @brief 一个端口（或参数）的描述。
 *
 * `connectable == false` 时它就是旧工程里的 `Param`。
 *
 * @ownership   owns（name / label / description / choice_names 自持）
 * @thread      any（构造后只读）
 * @pre         none
 * @post        none
 * @invariant   同一 PortDesc 的 number 在所属 NodeDesc 内唯一
 * @errors      noexcept
 * @frozen      是
 * @tests       graph.desc.port_basic, graph.desc.port_connectable_flag,
 *              graph.desc.port_numeric_bounds, graph.desc.port_choices
 */
struct PortDesc final {
    PortNumber number = 0;                 ///< 节点内序号（1 起）
    std::string name;                      ///< 程序用标识（YAML 键、脚本名）
    std::string label;                     ///< 面向用户的显示名（可为空 → 用 name）
    std::string description;               ///< 帮助文本

    qp::ports::PortTypeId type = qp::ports::kScalarF64;

    /// 是否可连线。`false` 即"参数"——只能在属性面板填值。
    ///
    /// 输入端口可以是参数；**输出端口必须是可连线的**（否则它没有意义）。
    bool connectable = true;

    /// 是否为必需。缺席时该节点不可求值（校验期即报错）。
    bool required = false;

    // ── UI 提示（不参与求值，但参与属性面板的自动渲染）────────────────────
    /// 数值下界（仅数值类型有意义）。
    double min_value = 0.0;
    /// 数值上界。
    double max_value = 0.0;
    /// 是否使用数值范围（false 时不校验范围）。
    bool has_range = false;
    /// 提示步长（UI 用，0 表示不提示）。
    double step = 0.0;
    /// 单位换算因子：用户在 UI 里填的数 × 该因子 = SI 值。
    /// 例：用户填厘米，factor = 0.01。默认 1.0（即用户直接填 SI 值）。
    double unit_factor = 1.0;
    /// 单位符号（显示用）。空则由端口的量纲自动推导。
    std::string unit_symbol;
    /// 枚举选项的稳定名（type == kEnum 时有意义）。
    std::vector<std::string> choice_names;
    /// 枚举选项的显示名（与 choice_names 一一对应）。
    std::vector<std::string> choice_labels;

    [[nodiscard]] bool valid() const noexcept { return number != 0 && !name.empty(); }
};

/**
 * @brief 计算结果：端口号 → 值。
 *
 * 用 `std::vector` 而不是 map：节点端口数很少（几个到几十个），
 * 而求值在热路径上，线性查找远快于哈希。
 */
using PortValues = std::vector<std::pair<PortNumber, qp::ports::Value>>;

/// @brief 输入集合：只读视图，按端口号线性查找。
class InputView final {
public:
    explicit InputView(const PortValues& values) noexcept : values_(&values) {}

    /**
     * @brief 按键取输入值。缺失返回无效 Value。
     *
     * @ownership   observes
     * @thread      any
     * @pre         none
     * @post        未提供的端口返回 `Value{}`（kind == invalid）
     * @invariant   不修改底层集合
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      否
     * @tests       graph.desc.input_view_lookup
     */
    [[nodiscard]] qp::ports::Value get(PortNumber number) const noexcept {
        for (const auto& [n, v] : *values_) {
            if (n == number) return v;
        }
        return qp::ports::Value{};
    }

    /// @brief 取 f64 输入。缺失返回 0.0。
    [[nodiscard]] double f64(PortNumber number) const noexcept { return get(number).as_f64(); }
    /// @brief 取 i64 输入。缺失返回 0。
    [[nodiscard]] std::int64_t i64(PortNumber number) const noexcept { return get(number).as_i64(); }
    /// @brief 取布尔输入。缺失返回 false。
    [[nodiscard]] bool boolean(PortNumber number) const noexcept { return get(number).as_bool(); }
    /// @brief 取文本输入。缺失或类型不符返回空串。
    ///
    /// 返回引用指向**底层集合里的值**，因此在本视图存活期内有效。
    [[nodiscard]] const std::string& text(PortNumber number) const noexcept {
        for (const auto& [n, v] : *values_) {
            if (n == number) return v.as_text();
        }
        static const std::string kEmpty{};
        return kEmpty;
    }

private:
    const PortValues* values_;
};

/**
 * @brief 节点类型的描述。
 *
 * @ownership   owns
 * @thread      any（注册后只读）
 * @pre         none
 * @post        none
 * @invariant   同一 type_name 在注册表中唯一
 * @errors      noexcept
 * @frozen      是
 * @tests       graph.desc.node_basic, graph.desc.node_port_lookup,
 *              graph.desc.node_output_count, graph.desc.node_has_hooks
 */
struct NodeDesc final {
    /// 稳定类型名（YAML 里的 `type:`，插件注册键）。发布后不可改名。
    std::string type_name;
    /// 面向用户的显示名。
    std::string label;
    /// 帮助文本。
    std::string description;
    /// 分类（用于节点面板分组），如 "力学" / "信号"。
    std::string category;

    /// 插件来源标识。内置节点为空。用于归因与"哪个插件提供的"提示。
    std::string source;

    /// 实现版本。插件升级时用于触发迁移（见 core/plugin）。
    std::uint32_t version = 1;

    std::vector<PortDesc> inputs;
    std::vector<PortDesc> outputs;

    /// 是否允许出现在烘焙（field）域。默认允许。
    bool allow_in_field_domain = true;
    /// 是否允许出现在实时（particle）域。
    ///
    /// 实时域每帧执行，**禁止**任何可能阻塞或分配的实现。
    /// 由插件显式声明，默认拒绝（保守）。
    bool allow_in_particle_domain = false;

    /// 是否有求值实现。没有实现的节点只能作为纯声明（如渲染节点）。
    bool has_compute = false;

    // ── 查找辅助 ────────────────────────────────────────────────────────────

    /// @brief 按序号取端口（含输入与输出）。找不到返回 nullptr。
    [[nodiscard]] const PortDesc* find_port(PortNumber number, bool is_output) const noexcept;
    /// @brief 按名字取端口。找不到返回 nullptr。
    [[nodiscard]] const PortDesc* find_by_name(std::string_view name,
                                               bool is_output) const noexcept;
    /// @brief 输出端口数量（用于快速判断是否需要求值）。
    [[nodiscard]] std::size_t output_count() const noexcept { return outputs.size(); }

    [[nodiscard]] bool valid() const noexcept { return !type_name.empty(); }
};

/// @brief 节点类型注册表接口：从类型名到描述。
///
/// 刻意是**接口**而不是具体容器：宿主提供实现，插件只读消费。
/// 这样测试可以塞入假注册表，而不必构造完整的插件系统。
class INodeCatalog {
public:
    INodeCatalog() = default;
    virtual ~INodeCatalog() = default;
    INodeCatalog(const INodeCatalog&) = delete;
    INodeCatalog& operator=(const INodeCatalog&) = delete;

    /// @brief 按类型名查描述。未注册返回 nullptr。
    [[nodiscard]] virtual const NodeDesc* find(std::string_view type_name) const noexcept = 0;
    /// @brief 已注册类型数量。
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

}  // namespace qp::graph
