# core/graph/eval: evaluation scheduling + content-addressed cache.
#
# Dependency rule: eval depends on ir / structure / ports.
# It does not know validate (validation happens at load time; evaluation must not repeat it).

add_library(qp_graph_eval STATIC
    src/value_key.cpp
    src/cache.cpp
    src/evaluator.cpp)

add_library(qp::graph_eval ALIAS qp_graph_eval)

target_include_directories(qp_graph_eval
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_eval PUBLIC cxx_std_20)
target_link_libraries(qp_graph_eval
    PUBLIC qp::graph_ir qp::graph_structure qp::ports qp::plugin
    PRIVATE qp_warnings qp_sanitize)
