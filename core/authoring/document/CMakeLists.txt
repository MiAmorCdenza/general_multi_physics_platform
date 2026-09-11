# core/authoring/document: document identity and per-view layout metadata.
#
# Dependency rule: document depends on ir / abi / diag
# (see plan-tree.md section 5.1 and the ALLOWED table in scripts/check_layers.py).
#
# The structural rule this module encodes: a node's position on screen is NOT a
# property of the graph. Coordinates live in ViewLayouts keyed by view id, and the
# core never reads them -- it does not know what "x" means and must not, because
# the moment it does, adding a view becomes a core change.
#
# This module deliberately does NOT depend on commands::Session. Session owns the
# live graph being edited; this owns what a saved document is. Keeping them apart
# is what stops "the document" from becoming a second mutable handle on the graph,
# which would reintroduce the two-copies problem Session exists to prevent.
#
# It also performs no I/O. Reading and writing a file is a serialisation choice,
# and serialisation is a plugin (plan-tree.md section 2.2).

add_library(qp_authoring_document STATIC
    src/document.cpp)

add_library(qp::authoring_document ALIAS qp_authoring_document)

target_include_directories(qp_authoring_document
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_authoring_document PUBLIC cxx_std_20)
target_link_libraries(qp_authoring_document
    PUBLIC qp::graph_ir qp::abi qp::diag
    PRIVATE qp_warnings qp_sanitize)
