# core/graph/ir：中间表示。插件 API 本身。
#
# 依赖规则：ir 只依赖 L0 与 ports，不依赖 core/graph 内其他子模块。

add_library(qp_graph_ir STATIC
    src/descriptor.cpp)

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
