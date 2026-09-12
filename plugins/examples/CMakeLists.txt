# plugins/examples: worked experiments, and the predictions they check against.
#
# **Why this is a plugin and not a directory of saved files.** The plan tree lists `examples/` as "example graphs and
# documents", and files would be the obvious shape -- but a `.qpd` in the repository is either hand-written or
# generated, and both are worse here. Hand-written JSON drifts from the node types it names the moment a port
# changes, and nothing notices until a student opens it. Generated files need a generator and a gate to keep them
# in step, which is the icon sheet's arrangement -- and unlike an icon, an example's correctness is not a pixel
# comparison but a **physical agreement**, which only a test can check.
#
# So the examples are code, and the tests run them.
#
# **What makes an example worth having.** Each one is a whole experiment: a node with the parameters a lab would
# set, a run through the real loop, and a comparison against a **closed form written down independently** in
# `expected.hpp`. That last part is the point. A platform that computed both the trajectory and the number it is
# checked against would compare an implementation with itself and pass for any physics at all; the predictions
# here are published series and exact solutions, with no numerical method anywhere in them.
#
# An example is therefore also the strongest test in the repository: it exercises a model, the loop that drives it,
# and a formula from a textbook, and it fails if any of the three is wrong.
#
# Nothing here includes a Qt header: an example has to run in CI without a display.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_examples STATIC
    src/expected.cpp)

add_library(qp::plugins::examples ALIAS qp_plugins_examples)

target_include_directories(qp_plugins_examples
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_examples PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_examples
    PUBLIC qp::diag
    # The graph and the loop are PUBLIC because the examples **are** graphs a caller runs: a consumer that linked
    # only this library could not name the types the functions return. The models are PUBLIC for the same reason
    # in the other direction -- an example names `model.pendulum`, so whoever loads it needs that type registered.
    PUBLIC qp::graph_structure qp::graph_ir qp::graph_execution qp::plugins::models
    PRIVATE qp_warnings qp_sanitize)
