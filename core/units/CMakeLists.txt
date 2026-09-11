# core/units: dimensions and quantities. Header-only, zero dependencies.
#
# Dependency rule: units must not depend on any other module inside core (plan-tree.md §8 rule 4).

add_library(qp_units INTERFACE)
add_library(qp::units ALIAS qp_units)

target_include_directories(qp_units INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(qp_units INTERFACE cxx_std_20)
target_link_libraries(qp_units INTERFACE qp_warnings qp_sanitize)
