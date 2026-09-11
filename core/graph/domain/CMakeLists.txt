# core/graph/domain: three-domain semantics and the execution plan.
#
# Dependency rule: domain depends on ir / structure / validate (for its Report).
# validate in turn uses domain's Domain enum -- the two are two sides of the same
# concept, so validate only provides Report and does not redefine the domains.

add_library(qp_graph_domain STATIC
    src/declaration.cpp
    src/build.cpp)

add_library(qp::graph_domain ALIAS qp_graph_domain)

target_include_directories(qp_graph_domain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_domain PUBLIC cxx_std_20)
target_link_libraries(qp_graph_domain
    PUBLIC qp::graph_ir qp::graph_structure qp::graph_validate
    PRIVATE qp_warnings qp_sanitize)
