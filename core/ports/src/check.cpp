/**
 * @file check.cpp
 * @brief 端口类型检查的实现。
 */
#include <qp/ports/check.hpp>

namespace qp::ports {
namespace {

using qp::diag::ErrorCode;

/// @brief 两个数值类型之间是否需要精度转换，以及方向。
enum class NumericBridge : std::uint8_t {
    identical = 0,   ///< 完全相同
    widen = 1,       ///< a 精度更低，接到 b 需拓宽（无损）
    narrow = 2,      ///< a 精度更高，接到 b 需收窄（有损）
    incompatible = 3 ///< 数值类别不同（如 f64 → bool）
};

/// @brief 数值类型的秩。用于判断拓宽/收窄方向。
[[nodiscard]] constexpr int numeric_rank(NumericKind k) noexcept {
    switch (k) {
        case NumericKind::none: return -1;
        case NumericKind::boolean: return 0;
        case NumericKind::i32: return 1;
        case NumericKind::i64: return 2;
        case NumericKind::f32: return 3;
        case NumericKind::f64: return 4;
    }
    return -1;
}

/// @brief 判定两个数值类型的桥接方式。
///
/// 规则（刻意保守）：
///   - 完全相同 → identical
///   - f32 ↔ f64 → widen / narrow（这是 ADR-0005 明确允许的唯一精度互通）
///   - 其它组合（如 f64 → bool、i64 → f64）→ incompatible
///
/// 为什么不允许 i64 → f64：大整数转 double 会丢位，而静默丢位正是
/// 我们要防的那类错误。需要时由节点显式声明一个转换端口。
[[nodiscard]] constexpr NumericBridge bridge_of(NumericKind a, NumericKind b) noexcept {
    if (a == b) return NumericBridge::identical;

    const auto is_float = [](NumericKind k) {
        return k == NumericKind::f32 || k == NumericKind::f64;
    };
    if (is_float(a) && is_float(b)) {
        return numeric_rank(a) < numeric_rank(b) ? NumericBridge::widen : NumericBridge::narrow;
    }
    return NumericBridge::incompatible;
}

/// @brief 场类端口的分量数与元素类型是否一致。
[[nodiscard]] constexpr bool fields_shape_equal(const PortTypeDesc& a,
                                                const PortTypeDesc& b) noexcept {
    return a.field_components == b.field_components && a.field_element == b.field_element;
}

}  // namespace

bool check_dimensions(const PortTypeDesc& a, const PortTypeDesc& b) noexcept {
    // 任一端不约束量纲 → 兼容
    if (a.constraint == DimensionConstraint::any || b.constraint == DimensionConstraint::any) {
        return true;
    }
    // same_as_input 需要图的上下文才能解析，此处不判定（视为兼容，留给层间校验）
    if (a.constraint == DimensionConstraint::same_as_input ||
        b.constraint == DimensionConstraint::same_as_input) {
        return true;
    }
    return a.dimension == b.dimension;
}

ConnectionCheck check_connection(const PortTypeDesc& from, PortDirection from_dir,
                                 const PortTypeDesc& to, PortDirection to_dir) noexcept {
    ConnectionCheck r{};

    // 方向：只能 output → input
    if (from_dir == to_dir) {
        r.verdict = ConnectionVerdict::direction_mismatch;
        return r;
    }
    // 内部统一按 output → input 处理
    const PortTypeDesc& src = (from_dir == PortDirection::output) ? from : to;
    const PortTypeDesc& dst = (from_dir == PortDirection::output) ? to : from;

    if (!src.valid() || !dst.valid()) {
        r.verdict = ConnectionVerdict::unknown_type;
        return r;
    }

    // any 逃生舱：允许但显式标注
    if (src.is_any || dst.is_any) {
        r.verdict = ConnectionVerdict::ok_with_any;
        r.has_any = true;
        return r;
    }

    // 场类：形状必须完全一致
    const bool src_field = src.is_field();
    const bool dst_field = dst.is_field();
    if (src_field != dst_field) {
        r.verdict = ConnectionVerdict::type_mismatch;
        return r;
    }
    if (src_field && dst_field) {
        if (!fields_shape_equal(src, dst)) {
            r.verdict = ConnectionVerdict::type_mismatch;
            return r;
        }
        // 场的量纲约束通常为 any；若都约束了则要比
        if (!check_dimensions(src, dst)) {
            r.verdict = ConnectionVerdict::dimension_mismatch;
            return r;
        }
        r.verdict = ConnectionVerdict::ok;
        return r;
    }

    // 数值标量：按数值类别判定
    if (src.is_numeric() != dst.is_numeric()) {
        r.verdict = ConnectionVerdict::type_mismatch;
        return r;
    }
    if (src.is_numeric() && dst.is_numeric()) {
        switch (bridge_of(src.numeric, dst.numeric)) {
            case NumericBridge::identical:
                break;
            case NumericBridge::widen:
                r.needs_numeric_conversion = true;
                break;
            case NumericBridge::narrow:
                // 收窄允许但需标注：精度损失是调用方的决定，不能悄悄发生
                r.needs_numeric_conversion = true;
                break;
            case NumericBridge::incompatible:
                r.verdict = ConnectionVerdict::type_mismatch;
                return r;
        }
        if (!check_dimensions(src, dst)) {
            r.verdict = ConnectionVerdict::dimension_mismatch;
            return r;
        }
        r.verdict = r.needs_numeric_conversion ? ConnectionVerdict::ok_with_numeric_widening
                                               : ConnectionVerdict::ok;
        return r;
    }

    // 非数值、非同形状（文本、句柄、数据集等）：必须完全同类
    if (src.id != dst.id) {
        r.verdict = ConnectionVerdict::type_mismatch;
        return r;
    }
    r.verdict = ConnectionVerdict::ok;
    return r;
}

Result<void> check_value(const PortTypeDesc& port, const Value& v, SourceId source) {
    if (!port.valid()) {
        return Result<void>{ErrorCode::unknown_port_type};
    }

    // any 端口接受一切（含 invalid——"尚未计算"对 any 是合法的）
    if (port.is_any) {
        return Result<void>{};
    }

    if (!v.valid()) {
        return Result<void>{ErrorCode::missing_field};
    }

    // ── 场类 ────────────────────────────────────────────────────────────────
    if (port.is_field()) {
        if (v.kind() != ValueKind::field_handle) {
            return Result<void>{ErrorCode::type_mismatch};
        }
        const auto lattice = v.as_field();
        const std::uint32_t expected = port.field_components;
        const std::uint32_t actual = qp::abi::component_count(lattice.component);
        if (expected != actual) {
            return Result<void>{ErrorCode::type_mismatch};
        }
        return Result<void>{};
    }

    // ── 标量类 ──────────────────────────────────────────────────────────────
    switch (port.numeric) {
        case NumericKind::f64:
            // f32 值可以接到 f64 端口（无损拓宽）
            if (v.kind() == ValueKind::f64 || v.kind() == ValueKind::f32) {
                return Result<void>{};
            }
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::f32:
            // f64 → f32 有损，但**允许**：收窄是调用方的显式选择（ADR-0005）
            if (v.kind() == ValueKind::f32 || v.kind() == ValueKind::f64) {
                return Result<void>{};
            }
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::boolean:
            if (v.kind() == ValueKind::boolean) return Result<void>{};
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::i64:
        case NumericKind::i32:
            // 整数端口**只**接整数：避免静默丢位
            if (v.kind() == ValueKind::i64) return Result<void>{};
            return Result<void>{ErrorCode::type_mismatch};

        case NumericKind::none:
            break;
    }

    // ── 非数值：按 ID 对应的期望 kind ───────────────────────────────────────
    switch (port.id) {
        case kString:
        case kEnum:
            return v.kind() == ValueKind::text ? Result<void>{}
                                               : Result<void>{ErrorCode::type_mismatch};
        case kDimension:
            return v.kind() == ValueKind::dimension ? Result<void>{}
                                                    : Result<void>{ErrorCode::type_mismatch};
        case kParticleBuffer:
        case kGeometry:
        case kFieldTable:
        case kDataset:
        case kFitResult:
            // 这些类型的载荷定义在各自模块（abi / runtime/store）。
            // 端口层只保证"类型不混用"：具体载荷校验由使用方完成。
            return Result<void>{};
        default:
            break;
    }

    (void)source;
    return Result<void>{ErrorCode::type_mismatch};
}

}  // namespace qp::ports
