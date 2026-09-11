# core/graph/eval：求值调度 + 内容寻址缓存。
#
# 依赖规则：eval 依赖 ir / structure / ports。
# 它不认识 validate（校验是加载期的事，求值不该重复校验）。

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
    PUBLIC qp::graph_ir qp::graph_structure qp::ports
    PRIVATE qp_warnings qp_sanitize)
