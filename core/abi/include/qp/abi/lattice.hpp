/**
 * @file lattice.hpp
 * @brief 采样格子的 ABI 描述：场数据长什么样。
 *
 * 这是"一张场"的**元数据**，不是数据本身。数据在 `FieldBuffer` 里。
 * 两者分离的理由：元数据极小（可随意复制），数据极大（必须零拷贝）。
 *
 * @frozen 是——布局是 ABI 契约。
 */
#pragma once

#include <qp/abi/field_dim.hpp>

#include <cstdint>

namespace qp::abi {

/// @brief 格子类型。决定 `data_bytes` 怎么算。
enum class LatticeKind : std::uint8_t {
    /// 不在格子上：单个值（点测量、标量参数）。
    point = 0,
    /// 1 维数组：`count[0]` 个采样点。
    line = 1,
    /// 2 维网格：`count[0] × count[1]`。
    plane = 2,
    /// 3 维网格：`count[0] × count[1] × count[2]`。
    volume = 3,
};

/// @brief 场的分量类型。
enum class ComponentKind : std::uint8_t {
    scalar = 0,   ///< 每点 1 个 float32
    vector = 1,   ///< 每点 3 个 float32（x, y, z）
};

/// @brief 数据元素的标量类型。
enum class ElementType : std::uint8_t {
    f32 = 0,   ///< 4 字节浮点。场数据的默认精度（见 ADR-0005）。
    f64 = 1,   ///< 8 字节浮点。标量测量链用。
};

/**
 * @brief 采样格子的描述。
 *
 * 布局（小端，所有整数定长）：
 * ```
 * 偏移  长度  字段
 *    0     7  dimension   （7 个 int8）
 *    7     1  component
 *    8     1  element
 *    9     1  kind
 *   10     1  padding
 *   12     4  count[0]
 *   16     4  count[1]
 *   20     4  count[2]
 *   24     4  spacing_bytes   （每个采样点的间距 = 分量数 × 元素大小）
 *   28     4  _reserved
 * 共 32 字节，alignof == 4
 * ```
 *
 * @ownership   pure（POD，可随意复制）
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   同一格子描述每次序列化得到同一字节串
 * @errors      noexcept
 * @frozen      是
 * @tests       abi.lattice.size_and_alignment, abi.lattice.field_offsets,
 *              abi.lattice.point_count, abi.lattice.data_bytes,
 *              abi.lattice.trivially_copyable, abi.lattice.default_is_point_scalar
 */
struct LatticeDesc final {
    FieldDim dimension{};                                  ///< 物理量的量纲
    ComponentKind component = ComponentKind::scalar;       ///< 标量还是矢量
    ElementType element = ElementType::f32;                ///< 元素精度
    LatticeKind kind = LatticeKind::point;                 ///< 几何形状
    std::uint8_t padding = 0;                              ///< 显式填充，保持后续字段 4 字节对齐

    std::uint32_t count[3] = {0, 0, 0};                    ///< 各维采样点数
    std::uint32_t spacing_bytes = 0;                       ///< 每个采样点占多少字节
    std::uint32_t reserved = 0;                            ///< 保留，必须为 0
};

/**
 * @brief 采样点总数。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        point / line / plane / volume 分别返回 1 / c0 / c0*c1 / c0*c1*c2
 * @invariant   point 类型恒返回 1（即使 count 全为 0）
 * @errors      noexcept；溢出时按 64 位计算后截断——调用方须先检查 data_bytes 的合理性
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.lattice.point_count
 */
[[nodiscard]] constexpr std::uint64_t point_count(const LatticeDesc& d) noexcept {
    switch (d.kind) {
        case LatticeKind::point:  return 1;
        case LatticeKind::line:   return d.count[0];
        case LatticeKind::plane:  return static_cast<std::uint64_t>(d.count[0]) * d.count[1];
        case LatticeKind::volume:
            return static_cast<std::uint64_t>(d.count[0]) * d.count[1] * d.count[2];
    }
    return 0;
}

/// @brief 每个采样点包含几个 float（标量 1、矢量 3）。
[[nodiscard]] constexpr std::uint32_t component_count(ComponentKind c) noexcept {
    return c == ComponentKind::vector ? 3U : 1U;
}

/// @brief 单个元素的字节数。
[[nodiscard]] constexpr std::uint32_t element_size(ElementType e) noexcept {
    return e == ElementType::f64 ? 8U : 4U;
}

/**
 * @brief 该格子所需的数据字节数。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        等于 point_count × component_count × element_size
 * @invariant   与 LatticeDesc::spacing_bytes × point_count 一致（当 spacing 已正确填写）
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.lattice.data_bytes
 */
[[nodiscard]] constexpr std::uint64_t data_bytes(const LatticeDesc& d) noexcept {
    return point_count(d) * component_count(d.component) * element_size(d.element);
}

/// @brief 按本描述应有的每点间距。
[[nodiscard]] constexpr std::uint32_t expected_spacing(const LatticeDesc& d) noexcept {
    return component_count(d.component) * element_size(d.element);
}

/**
 * @brief 描述是否自洽。宿主在采用外部传入的格子前必须调用。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        检查 reserved 为 0、spacing 与分量/精度一致、维数计数合法
 * @invariant   自洽的描述不会导致越界读取
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      否
 * @tests       abi.lattice.consistency_check
 */
[[nodiscard]] constexpr bool is_consistent(const LatticeDesc& d) noexcept {
    if (d.reserved != 0) return false;
    if (d.padding != 0) return false;
    if (d.spacing_bytes != expected_spacing(d)) return false;
    switch (d.kind) {
        case LatticeKind::point:
            break;
        case LatticeKind::line:
            if (d.count[0] == 0) return false;
            break;
        case LatticeKind::plane:
            if (d.count[0] == 0 || d.count[1] == 0) return false;
            break;
        case LatticeKind::volume:
            if (d.count[0] == 0 || d.count[1] == 0 || d.count[2] == 0) return false;
            break;
        default:
            return false;
    }
    return true;
}

/// @brief 构造一个自洽的格子描述（自动填 spacing）。
[[nodiscard]] constexpr LatticeDesc make_lattice(LatticeKind kind, ComponentKind component,
                                                 ElementType element, FieldDim dim,
                                                 std::uint32_t n0 = 0, std::uint32_t n1 = 0,
                                                 std::uint32_t n2 = 0) noexcept {
    LatticeDesc d{};
    d.dimension = dim;
    d.component = component;
    d.element = element;
    d.kind = kind;
    d.padding = 0;
    d.count[0] = n0;
    d.count[1] = n1;
    d.count[2] = n2;
    d.spacing_bytes = component_count(component) * element_size(element);
    d.reserved = 0;
    return d;
}

}  // namespace qp::abi
