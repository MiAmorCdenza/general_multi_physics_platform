/**
 * @file field_dim.hpp
 * @brief ABI 自带的量纲编码：**刻意与 `qp::units::Dim` 重复定义**。
 *
 * 这是一个有意为之的取舍，必须写清楚，否则后人会"顺手去重"而破坏分层。
 *
 * ## 为什么不直接 include `qp/units/dim.hpp`
 *
 * `core/abi` 是给**外部语言绑定**（Python / MATLAB / 未来 Web）看的边界契约。
 * 那些绑定不编译 C++，只读一份布局说明。若 abi 头文件依赖 units 头文件，
 * "ABI 是什么"这件事就被埋进了一条 C++ 模板库里，外部无法独立消费。
 *
 * 因此 `abi` 的规则是：**只用 C 语言子集**（POD、定长整数、无模板、无 STL）。
 * 这让 ABI 可以被逐字节复制到一份语言中立的 .h 里。
 *
 * ## 重复定义会不会漂移
 *
 * 不会——`tests/abi/test_abi_layout.cpp` 里有一条断言：
 * `qp::abi::FieldDim` 与 `qp::units::Dim` 的 sizeof 与七个指数语义必须一致。
 * 漂移会在编译期立刻暴露。**用断言守住重复，而不是用依赖消除重复。**
 *
 * @frozen 是——本结构的布局是 ABI 契约。
 */
#pragma once

#include <cstdint>

namespace qp::abi {

/// @brief 量纲指数。与 `qp::units::DimExp` 相同（int8）。
using DimExp = std::int8_t;

/// @brief 量纲的 ABI 表示：七个 SI 基本量纲的整数指数。
///
/// 成员顺序与语义必须与 `qp::units::Dim` 一致：L, M, T, I, Th, N, J
/// （对应 m, kg, s, A, K, mol, cd）。
///
/// 用 struct 而不是 `int8_t[7]`：数组无法带契约，也不能直接按成员名访问，
/// 而外部语言绑定读的是**成员名与偏移**。
struct FieldDim final {
    DimExp L = 0;
    DimExp M = 0;
    DimExp T = 0;
    DimExp I = 0;
    DimExp Th = 0;
    DimExp N = 0;
    DimExp J = 0;
};

/// @brief 七个指数的含义（供外部语言绑定阅读，不参与布局）。
enum class DimAxis : std::uint8_t {
    length = 0,        ///< 米
    mass = 1,          ///< 千克
    time = 2,          ///< 秒
    current = 3,       ///< 安培
    temperature = 4,   ///< 开尔文
    amount = 5,        ///< 摩尔
    luminous = 6,      ///< 坎德拉
};

/// @brief 无量纲（全零）。
inline constexpr FieldDim kDimensionless{};

}  // namespace qp::abi
