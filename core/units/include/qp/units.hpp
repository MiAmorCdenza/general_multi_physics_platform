/**
 * @file qp/units.hpp
 * @brief units 模块的唯一入口。
 */
#pragma once

#include <qp/units/dim.hpp>
#include <qp/units/dimensions.hpp>
#include <qp/units/literals.hpp>
#include <qp/units/quantity.hpp>
#include <qp/units/unit_symbol.hpp>

namespace qp::units {

/// @brief 量纲系统的 ABI 版本。`Dim` 的布局或指数含义变更时必须递增。
inline constexpr int kUnitsAbiVersion = 1;

}  // namespace qp::units
