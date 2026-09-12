# core/graph/execution: the run loop's contracts -- an operator, a state, a binder.
#
# Dependency rule: execution depends on diag / graph_ir / graph_structure / runtime_run /
# runtime_trace / runtime_store (see the ALLOWED table in scripts/check_layers.py). Runtime
# modules are the "third group" in that table and are visible to everything, so the trace and
# the run identity are legitimate here.
#
# ## Why this is a core module and not part of the view layer
#
# It was written there first, and it did not compile -- not because of a typo but because of a
# **dependency cycle**: the binder that maps a node's parameters to a plugin's operator has to
# live in the plugin, and the view layer already links the plugin. `plugins -> views -> plugins`
# is not a missing include; it is the layering saying the interfaces are in the wrong place.
#
# And they were. `IStateOperator` and the run loop are **mechanism**: "how the host drives an
# operator over time" is exactly what `graph/kernels` refuses to own ("it does NOT contain a
# single integrator"), and the loop that steps one belongs beside that contract rather than in a
# view. What remains in `views` is orchestration -- reading a session's graph and handing a node
# to a binder -- which is where the graph, the session and the panels actually meet.
#
# It deliberately does **not** depend on `graph/kernels`: an operator here is two methods, and
# binding this to `IBatchAdvancer` would mean a run loop that can only drive batch advancers. The
# adapter that presents one as the other belongs to the plugin, next to the code it adapts.

add_library(qp_graph_execution STATIC
    src/execution.cpp)

add_library(qp::graph_execution ALIAS qp_graph_execution)

target_include_directories(qp_graph_execution
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_graph_execution PUBLIC cxx_std_20)
target_link_libraries(qp_graph_execution
    PUBLIC qp::graph_ir qp::graph_structure qp::graph_validate qp::diag qp::plugin
           qp::runtime_run qp::runtime_store qp::runtime_trace
    PRIVATE qp_warnings qp_sanitize)
