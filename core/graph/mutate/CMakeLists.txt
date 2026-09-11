# core/graph/mutate：命令总线 + 撤销栈。
#
# 依赖规则：mutate 依赖 ir 与 structure。
# 它是"全部变异的唯一入口"——因此在别的模块里直接改 Graph 是架构违规，
# 由 core/authoring/commands 负责把这条规则暴露给视图层。

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
