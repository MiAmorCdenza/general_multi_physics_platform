/**
 * @file forbidden_direction.hpp
 * @brief Negative sample: an **illegal** direction. L1 must catch it.
 *
 * units is the bottom of L0; in the whitelist it "depends on nobody".
 * Making it include abi is a violation inside the same layer.
 */
#pragma once

#include <qp/abi/field_buffer.hpp>   // illegal: units must not depend on abi

namespace qp::layerfixture::bad_direction {

[[nodiscard]] inline constexpr int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_direction
