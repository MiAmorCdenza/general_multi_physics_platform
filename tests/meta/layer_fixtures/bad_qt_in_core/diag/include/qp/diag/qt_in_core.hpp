/**
 * @file qt_in_core.hpp
 * @brief 反例样本：core 里出现了 Qt。必须被 L2 抓到。
 *
 * 这条铁律是"core 必须能脱离 Qt 独立构建"（enforcement.md §6）的机械保证。
 */
#pragma once

#include <QObject>          // 非法：core 内不得包含 Qt
#include <QString>

namespace qp::layerfixture::bad_qt {

[[nodiscard]] inline int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_qt
