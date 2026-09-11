/**
 * @file allowed_dep.hpp
 * @brief 反例样本（正例）：**合法**依赖。门禁不得对它报错。
 *
 * 目录约定与真实仓库一致：`<mod>/include/qp/<mod>/...`
 * diag 被允许依赖 units（见 docs/plan-tree.md §8 白名单）。
 */
#pragma once

#include <qp/units/dim.hpp>   // 合法：diag → units

namespace qp::layerfixture::ok {

/// 合法：只是使用另一个 core 模块的公开类型。
[[nodiscard]] inline constexpr int use_units_dim() noexcept {
    return qp::units::Dim{}.sum();
}

}  // namespace qp::layerfixture::ok
