# core/graph/domain: three-domain semantics and the execution plan.
#
# Dependency rule: domain depends on ir / structure / validate (for its Report) and on field.
# validate in turn uses domain's Domain enum -- the two are two sides of the same
# concept, so validate only provides Report and does not redefine the domains.
#
# field is here because the render domain's **other end** has to read what a declaration names: a render node
# says "draw the field on this wire", and `ViewRequest` carries the run's published tables so that a view item
# can follow that wire to samples. The edge points down -- field depends on abi alone and holds no algorithm --
# and the reasoning is in the ALLOWED table in scripts/check_layers.py.

add_library(qp_graph_domain STATIC
    src/declaration.cpp
    src/build.cpp
    src/view_items.cpp)

add_library(qp::graph_domain ALIAS qp_graph_domain)

target_include_directories(qp_graph_domain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_domain PUBLIC cxx_std_20)
target_link_libraries(qp_graph_domain
    PUBLIC qp::graph_ir qp::graph_structure qp::graph_validate qp::field
    PRIVATE qp_warnings qp_sanitize)
