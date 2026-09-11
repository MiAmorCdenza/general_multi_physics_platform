/**
 * @file qp/units.hpp
 * @brief The single entry point of the units module.
 */
#pragma once

#include <qp/units/dim.hpp>
#include <qp/units/dimensions.hpp>
#include <qp/units/literals.hpp>
#include <qp/units/quantity.hpp>
#include <qp/units/unit_symbol.hpp>

namespace qp::units {

/// @brief ABI version of the dimension system. Bump when `Dim` layout or exponents change.
inline constexpr int kUnitsAbiVersion = 1;

}  // namespace qp::units
