/**
 * @file port_type.hpp
 * @brief 端口类型描述：端口的**类型契约**。
 *
 * ## 为什么端口类型必须是可注册的数据，而不是 C++ 类型
 *
 * 图形编辑、YAML 加载、脚本绑定、未来的 Web 前端都需要回答同一个问题：
 * "这个端口能不能连到那个端口"。若把类型信息藏在 C++ 类型里，
 * 每种消费者都要自己实现一遍判定，四份实现必然漂移。
 *
 * 因此类型信息是**数据**：一个 `PortTypeDesc`，可查询、可枚举、可序列化。
 * C++ 侧只是它的一层便捷封装。
 *
 * ## 为什么 ID 是稳定整数而不是 `type_index`
 *
 * `std::type_index` 在同一进程内有效，但：
 *   - 无法写进 YAML（学生保存的实验文件要能跨机器打开）；
 *   - 无法跨 DLL 边界（各 DLL 的 RTTI 不共享）；
 *   - 无法跨语言（Python 绑定拿不到它）。
 *
 * 因此 ID 是显式的 `uint16_t`，插件类型从 `kUserTypeBase` 起分配。
 *
 * @frozen 是——已注册类型的 ID 与名称一经发布即不可变。
 */
#pragma once

#include <qp/diag/error.hpp>
#include <qp/units.hpp>

#include <cstdint>
#include <string_view>

namespace qp::ports {

/// @brief 端口类型标识。稳定、可序列化、可跨语言。
using PortTypeId = std::uint16_t;

/// @brief 内置类型的固定 ID。**数值不得重排。**
enum BuiltinTypeId : PortTypeId {
    kInvalidType = 0,

    // ── 1..19 标量 ──────────────────────────────────────────────────────────
    kScalarF64 = 1,   ///< 双精度标量。测量链、参数、不确定度的默认类型
    kScalarF32 = 2,   ///< 单精度标量。**只用于与场数据交汇的端口**（ADR-0005）
    kBool = 3,
    kInt64 = 4,
    kString = 5,
    kEnum = 6,        ///< 具名选项，取值在 PortTypeDesc::choice_names 里
    kDimension = 7,   ///< 量纲本身作为值（用于"单位"参数）

    // ── 20..39 场 ───────────────────────────────────────────────────────────
    kScalarField = 20,   ///< 标量场（烘焙域产物）
    kVectorField = 21,   ///< 矢量场（烘焙域产物）
    kFieldTable = 22,    ///< 时间序列场表

    // ── 40..59 几何与粒子 ───────────────────────────────────────────────────
    kParticleBuffer = 40,
    kGeometry = 41,

    // ── 60..79 测量与数据（测量平台的差异化所在）────────────────────────────
    kDataset = 60,       ///< 测量数据表：值 + 不确定度 + 时间戳 + 有效标志
    kFitResult = 61,     ///< 拟合结果：系数 + 协方差 + 残差 + R²

    // ── 特殊 ────────────────────────────────────────────────────────────────
    /// "任意类型"：**会让类型检查失效**。只应用于纯转发的渲染链。
    /// 层间校验（core/graph/validate）会对它的使用发出告警。
    kAny = 1000,

    /// 插件自定义类型的起始 ID。低于此值的 ID 保留给宿主。
    kUserTypeBase = 1024,
};

/// @brief 核心（宿主内置）类型总数，用于注册表的静态容量。
inline constexpr std::size_t kBuiltinTypeCount = 8;

// ── 值的底层数值表示 ─────────────────────────────────────────────────────────

/**
 * @brief 数值类型。用于判定"形状相同但精度不同"能否连接。
 *
 * 这条信息是 ADR-0005 在类型系统里的落地：
 * `scalar_f32` 与 `scalar_f64` 是不同的端口类型，但在**值层面**兼容，
 * 转换只发生在端口边界一次。
 */
enum class NumericKind : std::uint8_t {
    none = 0,   ///< 非数值（string / field / dataset 等）
    f32 = 1,
    f64 = 2,
    i32 = 3,
    i64 = 4,
    boolean = 5,
};

// ── 量纲约束 ─────────────────────────────────────────────────────────────────

/**
 * @brief 端口的量纲约束。
 *
 * 三态是刻意的：
 *   - `exact`  ：必须等于 dim。绝大多数端口是这一类。
 *   - `any`    ：不约束量纲。只用于真正的泛型端口（如"任意场"）。
 *   - `same_as_input`：输出端口的量纲由某个输入端口的量纲决定
 *     （例：加法节点输出 = 两个输入的共同量纲）。**层间校验**负责解析它。
 */
enum class DimensionConstraint : std::uint8_t {
    exact = 0,
    any = 1,
    same_as_input = 2,
};

/**
 * @brief 端口的类型描述。
 *
 * @ownership   owns（`name` / `choice_names` 指向静态存储，见下）
 * @thread      any（构造后只读）
 * @pre         id != kInvalidType 时 name 非空
 * @post        none
 * @invariant   同一 id 的描述在注册表中唯一
 * @errors      noexcept
 * @frozen      是
 * @tests       ports.type_desc.basic_fields, ports.type_desc.dimension_constraint,
 *              ports.type_desc.numeric_kind_consistency
 */
struct PortTypeDesc final {
    PortTypeId id = kInvalidType;            ///< 稳定 ID
    std::string_view name{};                 ///< 稳定短名，如 "scalar_f64"
    NumericKind numeric = NumericKind::none; ///< 底层数值类型

    DimensionConstraint constraint = DimensionConstraint::exact;

    /// 仅当 constraint == exact 时有意义。
    qp::units::Dim dimension{};

    /// 场类端口的元素类型。非场类型为 f32（无意义）。
    /// 复用 abi 的取值语义：0 = f32，1 = f64。
    std::uint8_t field_element = 0;

    /// 场类端口的分量数：标量场 1、矢量场 3、非场类型 0。
    std::uint8_t field_components = 0;

    /// 是否为"任意类型"。为真时类型检查失效——**刻意显式**，
    /// 便于层间校验统计并告警，而不是让失效悄悄发生。
    bool is_any = false;

    [[nodiscard]] constexpr bool valid() const noexcept { return id != kInvalidType; }

    /// @brief 是否为场类端口。
    [[nodiscard]] constexpr bool is_field() const noexcept { return field_components > 0; }

    /// @brief 是否为数值标量（可与其它数值标量比较）。
    [[nodiscard]] constexpr bool is_numeric() const noexcept {
        switch (numeric) {
            case NumericKind::f32:
            case NumericKind::f64:
            case NumericKind::i32:
            case NumericKind::i64:
            case NumericKind::boolean:
                return true;
            case NumericKind::none:
                return false;
        }
        return false;
    }
};

}  // namespace qp::ports
