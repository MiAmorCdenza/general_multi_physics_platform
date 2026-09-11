/**
 * @file forbidden_direction.hpp
 * @brief 反例样本：**非法**方向。必须被 L1 抓到。
 *
 * units 是 L0 最底层，白名单里"谁都不依赖"。
 * 让它去 include abi 属于同层越界。
 */
#pragma once

#include <qp/abi/field_buffer.hpp>   // 非法：units 不得依赖 abi

namespace qp::layerfixture::bad_direction {

[[nodiscard]] inline constexpr int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_direction
