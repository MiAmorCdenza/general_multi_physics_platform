# core/authoring/layout: the layout algorithm contract.
#
# Dependency rule: layout depends on ir / document / diag
# (see plan-tree.md section 5.4 and the ALLOWED table in scripts/check_layers.py).
#
# The structural rule: the algorithm is a **plugin**. Where to put a node is a
# design question with several defensible answers -- layered for a circuit,
# circular for a state machine, "wherever the author dropped it" for a teaching
# demo -- and a platform that picks one has decided for every user of every
# domain.
#
# The determinism rule: compute() takes a seed and must be a pure function of
# (graph, seed). A layout that read a clock would make a screenshot regression
# test impossible; a layout that used an unseeded RNG would make the saved
# document differ between two runs of the same experiment, so a diff of two
# documents would stop meaning "the physics changed".
#
# Positions are integers, not doubles. A position is a graph coordinate, not a
# screen pixel: a float layout that depends on iteration order or on the host's
# floating-point mode would write 120.00000000000001 on one machine and 120.0 on
# another, and every save would show a spurious diff.

add_library(qp_authoring_layout STATIC
    src/layout.cpp)

add_library(qp::authoring_layout ALIAS qp_authoring_layout)

target_include_directories(qp_authoring_layout
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_authoring_layout PUBLIC cxx_std_20)
target_link_libraries(qp_authoring_layout
    PUBLIC qp::graph_ir qp::graph_structure qp::authoring_document qp::diag
    PRIVATE qp_warnings qp_sanitize)
