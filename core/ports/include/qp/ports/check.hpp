/**
 * @file check.hpp
 * @brief 端口类型检查：**加载期**判定，而不是求值期。
 *
 * ## 为什么必须在加载期
 *
 * 若把类型/单位校验推到求值期，症状是"学生算了十分钟，曲线荒谬"。
 * 加载期拒绝的代价是"打开实验时弹一个错误框"。
 * 两者对课堂的影响差了一个数量级——这条是 `core/graph/validate` 的设计前提。
 *
 * ## 三层判定，缺一不可
 *
 * 1. **形状兼容**：类型本身能否相接（`check_connection`）
 * 2. **量纲一致**：两端的物理量纲是否相同（`check_dimensions`）
 * 3. **值可用**：具体值是否配得上端口声明（`check_value`）
 *
 * 分开的理由：它们发生在不同的时刻。1、2 在建图时判定，
 * 3 在每次求值后判定（用于 catch 插件返回了错类型的值）。
 *
 * ## `kAny` 是刻意的逃生舱，但要留痕
 *
 * `any` 会让类型检查失效。它只应出现在纯转发的渲染链上。
 * 因此判定结果里**显式**带一个 `has_any` 标志，让层间校验能统计并告警，
 * 而不是让类型安全的洞悄悄存在。
 *
 * @frozen 否（判定规则可扩；已发布类型的判定语义冻结）
 */
#pragma once

#include <qp/diag/diagnostic.hpp>
#include <qp/diag/result.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>

#include <cstdint>

namespace qp::ports {

using qp::diag::Diagnostic;
using qp::diag::Result;
using qp::diag::SourceId;

/// @brief 端口连接的判定结论。
enum class ConnectionVerdict : std::uint8_t {
    /// 可以直接连。
    ok = 0,
    /// `any` 端口：允许连接，但**类型检查失效**。调用方应记录并告警。
    ok_with_any = 1,
    /// 形状相同但精度不同（f32 ↔ f64）：允许，且**必须在端口边界转换一次**。
    ok_with_numeric_widening = 2,
    /// 形状不同：拒绝。
    type_mismatch = 3,
    /// 形状相同但量纲不同：拒绝。
    dimension_mismatch = 4,
    /// 任一端类型未注册：拒绝。
    unknown_type = 5,
    /// 输出接输出 / 输入接输入：拒绝（连线方向错误）。
    direction_mismatch = 6,
};

/// @brief 端口方向。连线只能 output → input。
enum class PortDirection : std::uint8_t {
    input = 0,
    output = 1,
};

/**
 * @brief 连接的完整判定结果。
 *
 * 不只给布尔值：调用方需要知道**为什么**和**要不要警告**。
 */
struct ConnectionCheck final {
    ConnectionVerdict verdict = ConnectionVerdict::type_mismatch;
    /// 是否涉及 `any` 端口（类型检查已失效，应告警）。
    bool has_any = false;
    /// 是否需要数值精度转换（f32 ↔ f64）。
    bool needs_numeric_conversion = false;

    [[nodiscard]] constexpr bool acceptable() const noexcept {
        return verdict == ConnectionVerdict::ok || verdict == ConnectionVerdict::ok_with_any ||
               verdict == ConnectionVerdict::ok_with_numeric_widening;
    }
};

/// @brief 判定结论的稳定短名。
[[nodiscard]] constexpr const char* to_string(ConnectionVerdict v) noexcept {
    switch (v) {
        case ConnectionVerdict::ok: return "ok";
        case ConnectionVerdict::ok_with_any: return "ok_with_any";
        case ConnectionVerdict::ok_with_numeric_widening: return "ok_with_numeric_widening";
        case ConnectionVerdict::type_mismatch: return "type_mismatch";
        case ConnectionVerdict::dimension_mismatch: return "dimension_mismatch";
        case ConnectionVerdict::unknown_type: return "unknown_type";
        case ConnectionVerdict::direction_mismatch: return "direction_mismatch";
    }
    return "unknown";
}

/**
 * @brief 判定两个端口能否连接。
 *
 * @ownership   pure
 * @thread      any
 * @pre         from/to 的 id 必须在 registry 中（否则返回 unknown_type）
 * @post        返回确定的判定；绝不抛
 * @invariant   交换 from/to 时 type_mismatch / dimension_mismatch 判定不变
 * @errors      noexcept
 * @complexity  O(n)（注册表查找）
 * @nondet      none
 * @frozen      否
 * @tests       ports.check.connect_same_type, ports.check.connect_rejects_direction,
 *              ports.check.connect_rejects_type_mismatch,
 *              ports.check.connect_rejects_dimension_mismatch,
 *              ports.check.connect_allows_numeric_widening,
 *              ports.check.connect_any_is_flagged, ports.check.connect_unknown_type,
 *              ports.check.connect_is_symmetric_for_mismatch
 */
[[nodiscard]] ConnectionCheck check_connection(const PortTypeDesc& from, PortDirection from_dir,
                                               const PortTypeDesc& to, PortDirection to_dir) noexcept;

/**
 * @brief 判定两个类型的量纲是否兼容。
 *
 * 规则：
 *   - 任一端 constraint 为 `any` → 兼容（不约束）
 *   - 任一端为 `same_as_input` → **此处不判定**（需要图的上下文，由
 *     `core/graph/validate` 解析后再调用本函数）
 *   - 两端均为 `exact` → 量纲必须逐轴相等
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   同量纲与自己兼容
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       ports.check.dimension_compatible, ports.check.dimension_any_accepts_all,
 *              ports.check.dimension_same_as_input_is_deferred
 */
[[nodiscard]] bool check_dimensions(const PortTypeDesc& a, const PortTypeDesc& b) noexcept;

/**
 * @brief 判定一个值是否配得上端口声明。
 *
 * 这是"catch 插件返回了错类型的值"的那道闸——发生在**每次求值之后**。
 *
 * 判定内容：
 *   - 值的 kind 是否与端口的 numeric / field_components 匹配
 *   - `any` 端口接受任何值
 *   - f32 值接到 f64 端口：接受（无损拓宽）
 *   - f64 值接到 f32 端口：接受但标记需要收窄（有精度损失，调用方决定）
 *   - vector 场接到 scalar 场端口：拒绝
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        失败时返回带具体原因的诊断，而不是裸错误码
 * @invariant   成功的判定意味着 as_* 访问器会取到预期类型
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       ports.check.value_matches_port, ports.check.value_rejects_wrong_kind,
 *              ports.check.value_accepts_widening, ports.check.value_any_accepts_all,
 *              ports.check.value_rejects_invalid, ports.check.value_field_components_matter
 */
[[nodiscard]] Result<void> check_value(const PortTypeDesc& port, const Value& v,
                                       SourceId source = SourceId{"ports"});

}  // namespace qp::ports
