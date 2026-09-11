# core/graph/structure: graph structure. Add/remove connections, version, acyclicity.
#
# Dependency rule: structure depends on ir and on no other submodule of core/graph.

add_library(qp_graph_structure STATIC
    src/graph.cpp)

add_library(qp::graph_structure ALIAS qp_graph_structure)

target_include_directories(qp_graph_structure
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_structure PUBLIC cxx_std_20)
target_link_libraries(qp_graph_structure
    PUBLIC qp::graph_ir
    PRIVATE qp_warnings qp_sanitize)
