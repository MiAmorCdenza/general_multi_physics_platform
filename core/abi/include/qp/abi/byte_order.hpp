/**
 * @file byte_order.hpp
 * @brief 字节序声明。ABI 只支持小端。
 *
 * 为什么把这件事**显式声明**而不是"假设大家都是小端"：
 *   场数据动辄 19MB，做字节序转换是不可接受的成本；
 *   但"默默假设"会在移植到大端平台时变成静默的数据损坏。
 *
 * 因此：编译期检测，非小端平台直接编译失败。
 * 若将来真要支持大端，正确做法是新增一个显式的转换层并升 kAbiMajor，
 * 而不是在热路径里加条件分支。
 *
 * @frozen 是
 */
#pragma once

namespace qp::abi {

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "qp ABI 只支持小端（little-endian）。大端平台需要显式的转换层并升 kAbiMajor。"
#endif

#if defined(_M_PPC) || defined(__s390x__) || defined(__sparc__)
#error "qp ABI 只支持小端（little-endian）。检测到已知的大端目标平台。"
#endif

/// @brief 本 ABI 假定的小端标记。供外部语言绑定在握手时校验。
inline constexpr bool kLittleEndian = true;

}  // namespace qp::abi
