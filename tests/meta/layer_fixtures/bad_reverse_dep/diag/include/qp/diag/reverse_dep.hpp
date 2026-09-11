/**
 * @file reverse_dep.hpp
 * @brief 反例样本：core 依赖了消费者（views）。必须被 L4 抓到。
 */
#pragma once

#include <views/nodegraph/editor.hpp>   // 非法：core 不得依赖 views

namespace qp::layerfixture::bad_reverse {

[[nodiscard]] inline int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_reverse
