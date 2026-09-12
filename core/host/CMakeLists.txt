# core/host: the composition root of the content system.
#
# Dependency rule: host may depend on every registry owner, and nothing may depend on host except the
# application (see the ALLOWED table in scripts/check_layers.py).
#
# Every other core module is written so that it does not know what its neighbours are: `plugin` judges a
# manifest without knowing what a plugin contributes, `ir` owns descriptors without knowing who fills the
# catalog, `io` owns the format registry without knowing who registers a format. That leaves the question
# "who wires them together", and the answer used to be "the application, by linking its content statically" --
# which is not an answer for a platform whose premise is that content arrives as libraries.
#
# This module is that answer, and it is the only place in `core/` where a fan-in this wide is legal. The
# whitelist entry in scripts/check_layers.py is the confirmation that the coupling is intended rather than
# accidental: adding a fifth registry is an edit there, in one line, reviewed as what it is.
#
# It is not a layer. `plan-tree.md` places it beside `plugin` at L0 in the dependency diagram because both are
# foundation -- host is the foundation's *edge*, the one module allowed to look outward at every consumer
# interface inside core.

add_library(qp_host STATIC
    src/host.cpp)

add_library(qp::host ALIAS qp_host)

target_include_directories(qp_host
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_host PUBLIC cxx_std_20)
target_link_libraries(qp_host
    PUBLIC qp::diag
           qp::plugin
           qp::authoring_capability
           qp::graph_ir
           qp::graph_kernels
           qp::runtime_file
           qp::runtime_instrument
           qp::runtime_io
    PRIVATE qp_warnings qp_sanitize)
