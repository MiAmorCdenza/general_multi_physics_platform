# core/graph/mutate: command bus + undo stack.
#
# Dependency rule: mutate depends on ir and structure.
# It is "the only entry point for every mutation", so editing a Graph directly from
# another module is an architecture violation; core/authoring/commands exposes it to views.

add_library(qp_graph_mutate STATIC
    src/bus.cpp)

add_library(qp::graph_mutate ALIAS qp_graph_mutate)

target_include_directories(qp_graph_mutate
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_mutate PUBLIC cxx_std_20)
target_link_libraries(qp_graph_mutate
    PUBLIC qp::graph_ir qp::graph_structure
    PRIVATE qp_warnings qp_sanitize)
