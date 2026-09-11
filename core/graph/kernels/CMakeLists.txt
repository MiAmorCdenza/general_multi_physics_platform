# core/graph/kernels: the native operator contract and its registry.
#
# Dependency rule: kernels depends on abi / diag / field / domain
# (see plan-tree.md section 3.7 and the ALLOWED table in scripts/check_layers.py).
#
# It does NOT contain a single integrator. Boris, leapfrog, RK4 and Verlet are
# plugins: the foundation owns "what an operator is and how the host drives it",
# the content owns the physics. A scheme appearing in this directory would mean
# the plugin boundary had been crossed.
#
# Eigen is permitted here by the layer rule (L3), for kernels whose arithmetic is
# naturally vectorised. Nothing in this module uses it yet -- the contract is
# about POD plans and batch views, not about linear algebra.

add_library(qp_graph_kernels STATIC
    src/registry.cpp)

add_library(qp::graph_kernels ALIAS qp_graph_kernels)

target_include_directories(qp_graph_kernels
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_kernels PUBLIC cxx_std_20)
target_link_libraries(qp_graph_kernels
    PUBLIC qp::abi qp::diag qp::field qp::graph_domain
    PRIVATE qp_warnings qp_sanitize)
