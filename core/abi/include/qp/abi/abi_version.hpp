/**
 * @file abi_version.hpp
 * @brief ABI 版本与兼容判定。
 *
 * 为什么版本号必须是一等公民（而不是注释里的一句话）：
 *   插件的 `.dll` / `.so` 是**独立编译**的产物，宿主无法在编译期发现不匹配。
 *   静默的版本错配是插件架构最常见的失败模式——它不崩溃，只是算出错的东西。
 *
 * 因此：
 *   - 每个跨边界的结构体都带自己的版本常量；
 *   - 宿主在**加载期**判定，不兼容即拒绝，不做"尽力兼容"；
 *   - 判定规则是纯函数，可被单独测试（不需要真的加载任何插件）。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   同一 (host, plugin) 组合的判定结果恒定
 * @errors      noexcept
 * @frozen      是（枚举值与常量值冻结）
 * @tests       abi.version.values_are_frozen, abi.version.compatibility_matrix,
 *              abi.version.rejects_newer_major, abi.version.accepts_newer_minor
 */
#pragma once

#include <cstdint>

namespace qp::abi {

/// @brief 主版本。**任何布局变更**都必须递增——包括加字段、改对齐、改语义。
inline constexpr std::uint16_t kAbiMajor = 1;

/// @brief 次版本。只增不改：新增可选能力、新增枚举值属于此类。
inline constexpr std::uint16_t kAbiMinor = 0;

/// @brief `FieldBuffer` 的布局版本。与 kAbiMajor 独立演进。
inline constexpr std::uint16_t kFieldBufferLayout = 1;

/// @brief `LatticeDesc` 的布局版本。
inline constexpr std::uint16_t kLatticeDescLayout = 1;

/// @brief 版本三元组。出现在插件清单、序列化头部、以及跨进程握手包中。
struct Version final {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;

    [[nodiscard]] friend constexpr bool operator==(Version a, Version b) noexcept {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
    }
    [[nodiscard]] friend constexpr bool operator!=(Version a, Version b) noexcept {
        return !(a == b);
    }
};

/// @brief 宿主的版本。
inline constexpr Version kHostVersion{kAbiMajor, kAbiMinor, 0};

/**
 * @brief 版本兼容判定的结论。
 *
 * 刻意**给出原因**而不只是布尔值：插件加载失败时用户需要知道"为什么"，
 * 否则只能看到"加载失败"四个字。
 */
enum class CompatVerdict : std::uint8_t {
    /// 完全兼容。
    compatible = 0,
    /// 插件要求更新的宿主：加载它可能调用到不存在的符号。**拒绝**。
    host_too_old = 1,
    /// 插件主版本落后：语义可能已变。**拒绝**。
    plugin_too_old = 2,
    /// 插件主版本更新：同上，方向相反。**拒绝**。
    plugin_too_new = 3,
    /// 结构体布局版本不匹配（加字段/改对齐）。**拒绝**。
    layout_mismatch = 4,
};

/**
 * @brief 判定插件版本能否在给定宿主版本上加载。
 *
 * 规则（刻意保守）：
 *   - **主版本必须完全相等**。主版本变更意味着布局或语义已变，
 *     任何"尽力兼容"都会变成静默错误。
 *   - 插件次版本 > 宿主次版本 → 拒绝（插件可能用了宿主没有的能力）。
 *   - 插件次版本 <= 宿主次版本 → 允许（宿主向后兼容旧插件）。
 *   - 布局版本必须相等。
 *
 * 注意这条规则**故意不做**语义化版本的"向后兼容"推断：
 * 在 ABI 层面，"我以为它兼容"与"它确实兼容"的差别就是数据损坏。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回确定的判定结果，绝不抛
 * @invariant   自反性：host_version 与自身比较必为 compatible
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      是（判定规则冻结；放宽规则需升 kAbiMajor）
 * @tests       abi.version.compatibility_matrix, abi.version.rejects_newer_major,
 *              abi.version.accepts_newer_minor, abi.version.rejects_older_major,
 *              abi.version.rejects_layout_mismatch, abi.version.reflexive
 */
[[nodiscard]] constexpr CompatVerdict check_compatible(
    Version host, Version plugin,
    std::uint16_t host_layout = kFieldBufferLayout,
    std::uint16_t plugin_layout = kFieldBufferLayout) noexcept {
    if (host_layout != plugin_layout) return CompatVerdict::layout_mismatch;
    if (plugin.major > host.major) return CompatVerdict::host_too_old;
    if (plugin.major < host.major) return CompatVerdict::plugin_too_old;
    if (plugin.minor > host.minor) return CompatVerdict::plugin_too_new;
    return CompatVerdict::compatible;
}

/// @brief 判定是否可加载（等价于 verdict == compatible）。供 `if` 直接使用。
[[nodiscard]] constexpr bool is_compatible(Version host, Version plugin,
                                           std::uint16_t host_layout = kFieldBufferLayout,
                                           std::uint16_t plugin_layout = kFieldBufferLayout) noexcept {
    return check_compatible(host, plugin, host_layout, plugin_layout) ==
           CompatVerdict::compatible;
}

/**
 * @brief 判定结论的稳定短名。用于日志与用户可见提示。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回非空 ASCII 标识符
 * @invariant   同一 verdict 永远返回同一字符串
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      是
 * @tests       abi.version.verdict_names
 */
[[nodiscard]] constexpr const char* to_string(CompatVerdict v) noexcept {
    switch (v) {
        case CompatVerdict::compatible: return "compatible";
        case CompatVerdict::host_too_old: return "host_too_old";
        case CompatVerdict::plugin_too_old: return "plugin_too_old";
        case CompatVerdict::plugin_too_new: return "plugin_too_new";
        case CompatVerdict::layout_mismatch: return "layout_mismatch";
    }
    return "unknown";
}

}  // namespace qp::abi
