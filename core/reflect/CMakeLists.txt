# core/reflect: type names, from one source.
#
# Dependency rule: reflect depends on units (see plan-tree.md section 2.6 and the
# ALLOWED table in scripts/check_layers.py).
#
# Why this is foundation: C++, YAML and a plugin's script must share **one**
# spelling of a type's name, or the three drift. The spec puts it exactly that way,
# and the drift is not a compile error -- it shows up as a document that loads on
# one build and not another, or a plugin that is present but "not found", with a
# message naming a type that plainly exists.
#
# Header-only by design: everything here is a constexpr query over static members,
# so there is no state to hide and no implementation to compile. The one function
# with real work (`unit_symbol_of`) forwards to the units module rather than
# reimplementing it, because a second formatter would be the second source of a
# name -- the thing this module exists to remove.

add_library(qp_reflect INTERFACE)
add_library(qp::reflect ALIAS qp_reflect)

target_include_directories(qp_reflect INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(qp_reflect INTERFACE cxx_std_20)
target_link_libraries(qp_reflect INTERFACE qp::units qp_warnings qp_sanitize)
