# core/graph/validate: load-time validation.
#
# Dependency rule: validate depends on ir / structure / ports.
# It extends the ports layer's pointwise type check into whole-graph dimension

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
