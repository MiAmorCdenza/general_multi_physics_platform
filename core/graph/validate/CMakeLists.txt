# core/graph/validate：加载期校验。
#
# 依赖规则：validate 依赖 ir / structure / ports。
# 它把端口层的单点类型判定，扩展成"看到整张图才能完成"的量纲解析与全图校验。

add_library(qp_graph_validate STATIC
    src/report.cpp
    src/dimensions.cpp
    src/validate.cpp)

add_library(qp::graph_validate ALIAS qp_graph_validate)

target_include_directories(qp_graph_validate
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_validate PUBLIC cxx_std_20)
target_link_libraries(qp_graph_validate
    PUBLIC qp::graph_ir qp::graph_structure qp::ports
    PRIVATE qp_warnings qp_sanitize)
