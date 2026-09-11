/**
 * @file reverse_dep.hpp
 * @brief Negative sample: core depends on a consumer (views). L4 must catch it.
 */
#pragma once

#include <views/nodegraph/editor.hpp>   // illegal: core must not depend on views

namespace qp::layerfixture::bad_reverse {

[[nodiscard]] inline int f() noexcept { return 0; }

}  // namespace qp::layerfixture::bad_reverse
