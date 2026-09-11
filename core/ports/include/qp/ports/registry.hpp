/**
 * @file registry.hpp
 * @brief 端口类型注册表：类型的**唯一真相源**。
 *
 * ## 为什么是实例而不是全局单例
 *
 * "可变全局状态"是归属不清最常见的物理形态，并且会让测试互相污染
 * （见 `standards/enforcement.md` §3 的 `qp-no-mutable-global`）。
 * 注册表因此是显式传递的对象：宿主持有一个，测试各持一个。
 *
 * 代价是每个需要查类型的函数都要拿到注册表引用。这个代价是值得的：
 * 它让"谁注册了什么"变成可追踪的事实，而不是进程级的隐式状态。
 *
 * ## 三态注册语义
 *
 * 插件可能重复注册同一个 ID。三种情形必须区别对待：
 *   - **完全相同**（幂等）：允许。插件被加载两次是正常现象。
 *   - **冲突**（同 ID 不同描述）：拒绝，并报告冲突的具体字段。
 *   - **低于 kUserTypeBase 的 ID**：拒绝——那是宿主保留区间。
 *
 * "静默覆盖"是最危险的默认行为：它让两个插件的类型定义互相踩踏，
 * 而症状要到很远的地方才出现。
 *
 * @ownership   owns（持有全部注册项的值副本）
 * @thread      注册必须在单线程阶段完成（插件加载期）；查询可任意线程
 * @pre         none
 * @post        none
 * @invariant   已注册的 ID 不会被静默覆盖
 * @errors      register_type 返回 Result<void>，不抛
 * @frozen      否（可扩；已注册类型的语义冻结）
 * @tests       ports.registry.builtins_present, ports.registry.add_custom_type,
 *              ports.registry.rejects_duplicate_id, ports.registry.idempotent_reregister,
 *              ports.registry.rejects_reserved_range, ports.registry.rejects_invalid_id,
 *              ports.registry.count_and_lookup, ports.registry.deterministic_order
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace qp::ports {

using qp::diag::Result;

/**
 * @brief 端口类型注册表。
 *
 * @ownership   owns
 * @thread      注册：单线程；查询：任意线程（注册完成后只读）
 * @pre         none
 * @post        none
 * @invariant   任一 ID 至多对应一个 PortTypeDesc
 * @errors      不抛
 * @frozen      否
 */
class PortTypeRegistry final {
public:
    /// @brief 构造并注册全部宿主内置类型。
    PortTypeRegistry();

    PortTypeRegistry(const PortTypeRegistry&) = delete;
    PortTypeRegistry& operator=(const PortTypeRegistry&) = delete;

    /**
     * @brief 注册一个端口类型。
     *
     * @ownership   owns（复制描述）
     * @thread      main（插件加载期）
     * @pre         desc.id != kInvalidType；desc.name 非空
     * @post        成功时 find(desc.id) 返回 desc 的副本
     * @invariant   成功后 count() 增加 1；失败时注册表不变（强保证）
     * @errors      Result<void>；见下方错误码
     *   - invalid_argument        id 非法 / name 为空
     *   - out_of_range            id 落在宿主保留区间（< kUserTypeBase）
     *   - duplicate_connection    id 已被**不同**的描述占用
     * @complexity  O(n)
     * @nondet      none
     * @frozen      否
     * @tests       ports.registry.add_custom_type, ports.registry.rejects_duplicate_id,
     *              ports.registry.idempotent_reregister, ports.registry.rejects_reserved_range,
     *              ports.registry.rejects_invalid_id
     */
    Result<void> register_type(const PortTypeDesc& desc);

    /**
     * @brief 按 ID 查询。未注册返回 nullptr。
     *
     * @ownership   observes（返回指针指向注册表内部，注册表销毁即失效）
     * @thread      any
     * @pre         none
     * @post        返回 nullptr 或指向已注册描述
     * @invariant   同一 ID 在无注册发生的两次查询返回相同内容
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      否
     * @tests       ports.registry.count_and_lookup
     */
    [[nodiscard]] const PortTypeDesc* find(PortTypeId id) const noexcept;

    /**
     * @brief 按稳定短名查询。未注册返回 nullptr。
     *
     * 名称查询是给 YAML 加载器与脚本绑定用的：学生写 `type: scalar_f64`，
     * 而不是记数字 ID。
     *
     * @ownership   observes
     * @thread      any
     * @pre         none
     * @post        返回 nullptr 或指向已注册描述
     * @invariant   名称到 ID 的映射稳定
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      否
     * @tests       ports.registry.lookup_by_name
     */
    [[nodiscard]] const PortTypeDesc* find_by_name(std::string_view name) const noexcept;

    /// @brief 已注册类型数量。
    [[nodiscard]] std::size_t count() const noexcept { return types_.size(); }

    /// @brief 按注册顺序只读枚举（UI 与文档生成用）。
    [[nodiscard]] const std::vector<PortTypeDesc>& all() const noexcept { return types_; }

    /// @brief 是否存在给定的内置类型（用于自检）。
    [[nodiscard]] bool has_builtin(PortTypeId id) const noexcept;

private:
    std::vector<PortTypeDesc> types_;
};

/**
 * @brief 取得进程级共享的内置注册表。
 *
 * **刻意只读**：返回 const 引用。任何需要注册自定义类型的调用方
 * 必须自己构造一个 `PortTypeRegistry` 并显式持有它——
 * 这样"谁注册了什么"永远是局部的、可追踪的。
 *
 * 存在的理由：查"内置 scalar_f64 是什么"不需要每个调用点各自构造一份
 * 8 个元素的表。这是一份**不可变的**共享数据，不违反"无可变全局状态"。
 *
 * @ownership   observes（返回静态存储的 const 引用，全程有效）
 * @thread      any（首次调用由 C++11 静态初始化保证线程安全）
 * @pre         none
 * @post        返回的对象至少含全部内置类型
 * @invariant   跨调用返回同一对象
 * @errors      noexcept
 * @complexity  O(1)（首次 O(n)）
 * @nondet      none
 * @frozen      否
 * @tests       ports.registry.shared_builtins_is_readonly
 */
[[nodiscard]] const PortTypeRegistry& builtin_registry() noexcept;

/// @brief 内置类型描述的构造。供注册表与测试共用，避免两处漂移。
[[nodiscard]] std::vector<PortTypeDesc> make_builtin_types();

}  // namespace qp::ports
