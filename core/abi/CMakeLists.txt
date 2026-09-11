# core/abi: cross-boundary memory layout. Header-only, **zero core deps**.
#
# Dependency rule: abi is the only module that does not even include units.
# Rationale in include/qp/abi.hpp: the ABI must be consumable on its own by
# external language bindings; duplicate layout (FieldDim vs units::Dim) is asserted in tests/abi/.

add_library(qp_abi INTERFACE)
add_library(qp::abi ALIAS qp_abi)

target_include_directories(qp_abi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(qp_abi INTERFACE cxx_std_20)
target_link_libraries(qp_abi INTERFACE qp_warnings qp_sanitize)

# FieldBuffer uses std::atomic, so it needs the threads library
find_package(Threads REQUIRED)
target_link_libraries(qp_abi INTERFACE Threads::Threads)
