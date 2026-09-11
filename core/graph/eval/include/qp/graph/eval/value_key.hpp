/**
 * @file value_key.hpp
 * @brief 内容寻址：可复现的值哈希与键构造。
 *
 * ## 为什么不用 `std::hash`
 *
 * `std::hash<std::string>`、`std::hash<double>` 的具体算法是**实现定义**的：
 * 同一个二进制在不同标准库版本上给出不同结果。这带来两个问题：
 *
 *   1. **跨运行不可复现**。缓存落盘后再读回，键对不上。
 *   2. **缓存命中率无法解释**。"为什么这次没命中" 变成无法回答的问题。
 *
 * 因此本项目自带一个确定性的哈希（FNV-1a 64），并把它写进契约。
 * 它不是密码学哈希——只用于缓存键，且**必须**配合精确比较使用
 * （见 `CacheKey::equals`）：哈希相同不等于键相同。
 *
 * ## 大对象一律走标识，不走内容
 *
 * 19MB 的场不允许被逐字节哈希。因此 `field_handle` 类型的值按其
 * **格子描述符**参与键构造 —— 这是"同一格子 = 同一份数据"这一约定的落点，
 * 由 `core/abi` 的发布方保证。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   同一值序列永远给出同一哈希（跨运行、跨编译器）
 * @errors      noexcept
 * @frozen      是（哈希算法冻结；改变它会使所有缓存失效）
 */
#pragma once

#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/ir/ids.hpp>
#include <qp/ports/value.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace qp::graph {

/// @brief 哈希值的类型别名。
using ValueHash = std::uint64_t;

/// @brief FNV-1a 64 的初始值（与 offset basis 同名，便于对照规范）。
inline constexpr ValueHash kFnvOffsetBasis = 0xcbf29ce484222325ULL;
/// @brief FNV-1a 64 的质数。
inline constexpr ValueHash kFnvPrime = 0x100000001b3ULL;

/**
 * @brief 把一个字节串混入哈希。FNV-1a。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回混入后的新哈希，不修改输入
 * @invariant   同一 (seed, bytes) 永远给出同一结果
 * @errors      noexcept
 * @complexity  O(len)
 * @nondet      none
 * @frozen      是
 * @tests       graph.eval.hash_is_fnv1a, graph.eval.hash_of_empty_is_seed,
 *              graph.eval.hash_is_deterministic
 */
[[nodiscard]] ValueHash mix_bytes(ValueHash seed, const void* data, std::size_t len) noexcept;

/// @brief 混入一个 64 位整数（按小端序逐字节，保证跨平台一致）。
[[nodiscard]] ValueHash mix_u64(ValueHash seed, std::uint64_t v) noexcept;

/**
 * @brief 计算一个值的确定性哈希。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        同一值永远给出同一哈希；不同种类的值不会给出同一哈希
 *              （种类标签先行混入）
 * @invariant   确定性：不依赖地址、时间、随机数
 * @errors      noexcept
 * @complexity  O(1)（文本除外）
 * @nondet      none
 * @frozen      是
 * @tests       graph.eval.hash_value_kind_discriminates,
 *              graph.eval.hash_value_f32_f64_differ,
 *              graph.eval.hash_field_uses_lattice
 */
[[nodiscard]] ValueHash hash_value(ValueHash seed, const qp::ports::Value& v) noexcept;

/**
 * @brief 计算一串值（按端口号配对）的确定性哈希。
 *
 * 端口号参与哈希：`{1: 2.0}` 与 `{2: 2.0}` 必须给出不同结果，
 * 否则"把线从端口 1 挪到端口 2"会命中同一个缓存条目。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        顺序敏感（按传入顺序混入）
 * @invariant   确定性
 * @errors      noexcept
 * @complexity  O(n)
 * @nondet      none
 * @frozen      是
 * @tests       graph.eval.hash_port_number_matters, graph.eval.hash_order_matters
 */
[[nodiscard]] ValueHash hash_port_values(ValueHash seed,
                                         const std::vector<std::pair<PortNumber,
                                                                     qp::ports::Value>>& values)
    noexcept;

/**
 * @brief 把值序列化成可读文本。用于**缓存键的精确比较**与调试。
 *
 * 哈希负责快速筛选，文本负责最终判定：哈希碰撞时不会产生错误命中。
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        返回非空字符串；同一值永远给出同一文本
 * @invariant   若两个值的文本不同，则它们不是同一个值
 * @errors      noexcept（分配失败即 terminate）
 * @complexity  O(1)（文本除外）
 * @nondet      none
 * @frozen      否（展示格式可变，不参与缓存键相等性）
 * @tests       graph.eval.canonical_text_is_stable, graph.eval.canonical_text_discriminates
 */
[[nodiscard]] std::string canonical_text(const qp::ports::Value& v) noexcept;

}  // namespace qp::graph
