/**
 * @file unregistered.hpp
 * @brief A counter-example: it depends on a core module that is not in the whitelist. L5 must catch it.
 *
 * The point of this rule: a new cross-module dependency must **explicitly** change ALLOWED in
 * check_layers.py; editing that file is itself the confirmation "I know I am widening coupling".
 */
#pragma once

#include <qp/this_module_is_not_registered/thing.hpp>   // illegal: an unregistered module
namespace qp::layerfixture::bad_unregistered {

[[nodiscard]] inline constexpr int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_unregistered
