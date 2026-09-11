/**
 * @file allowed_dep.hpp
 * @brief Negative sample (positive case): a **legal** dependency. The gate must not report it.
 *
 * The directory convention matches the real repository: `<mod>/include/qp/<mod>/...`
 * diag is allowed to depend on units (see the whitelist in docs/plan-tree.md section 8).
 */
#pragma once

#include <qp/units/dim.hpp>   // legal: diag -> units

namespace qp::layerfixture::ok {

/// Legal: it merely uses a public type of another core module.
[[nodiscard]] inline constexpr int use_units_dim() noexcept {
    return qp::units::Dim{}.sum();
}

}  // namespace qp::layerfixture::ok
