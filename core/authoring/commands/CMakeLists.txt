# core/authoring/commands: the command bus exposed to views.
#
# Dependency rule: commands depends on graph/mutate + graph/structure + graph/ir
# (see plan-tree.md section 5.3 and the ALLOWED table in scripts/check_layers.py).
#
# The rule this module enforces structurally: **all views go through this bus,
# and no view holds its own copy of the graph.** Two copies diverge, and the
# divergence is not a crash -- the node editor moves a slider, the property panel
# still shows the old value, the user believes the panel, and the recorded
# experiment is not the one they configured.
#
# Session therefore owns the single Graph and the single CommandBus, and exposes
# the graph only as a const reference. There is deliberately no non-const
# accessor: that would reduce "all mutation goes through the bus" to a verbal
# agreement, which is the thing this module exists to avoid.

add_library(qp_authoring_commands STATIC
    src/session.cpp)

add_library(qp::authoring_commands ALIAS qp_authoring_commands)

target_include_directories(qp_authoring_commands
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_authoring_commands PUBLIC cxx_std_20)
target_link_libraries(qp_authoring_commands
    PUBLIC qp::graph_mutate qp::graph_structure qp::graph_ir
    PRIVATE qp_warnings qp_sanitize)
