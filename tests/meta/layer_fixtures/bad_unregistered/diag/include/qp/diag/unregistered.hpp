/**
 * @file unregistered.hpp
 * @brief 反例样本：依赖了未在白名单登记的 core 模块。必须被 L5 抓到。
 *
 * 这条规则的意义：新增跨模块依赖必须**显式**修改 check_layers.py 的 ALLOWED，
 * 改文件这个动作本身就是"我知道我在扩大耦合"的确认。这是故意的摩擦。
 */
#pragma once

#include <qp/this_module_is_not_registered/thing.hpp>   // 非法：未登记模块
namespace qp::layerfixture::bad_unregistered {

[[nodiscard]] inline constexpr int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_unregistered
