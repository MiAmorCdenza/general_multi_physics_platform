/**
 * @file qt_in_core.hpp
 * @brief Negative fixture: Qt appears inside core. Layer L2 must catch it.
 *
 * This iron rule mechanically guarantees "core can build without Qt" (enforcement.md section 6).
 */
#pragma once

// Illegal: core must not include Qt
#include <QString>

namespace qp::layerfixture::bad_qt {

[[nodiscard]] inline int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_qt
