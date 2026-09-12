# core/graph/ir: the intermediate representation. This is the plugin API itself.
#
# Dependency rule: ir depends only on L0 and ports, never on another core/graph submodule.

add_library(qp_graph_ir STATIC
    src/descriptor.cpp
    src/node_type_registry.cpp)

add_library(qp::graph_ir ALIAS qp_graph_ir)

target_include_directories(qp_graph_ir
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_ir PUBLIC cxx_std_20)
target_link_libraries(qp_graph_ir
    PUBLIC qp::units qp::diag qp::ports
    PRIVATE qp_warnings qp_sanitize)
