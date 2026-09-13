# core/authoring/persist: the contract for saving and loading a document.
#
# Dependency rule: persist depends on ir, structure and document (see
# plan-tree.md section 5.6 and the ALLOWED table in scripts/check_layers.py).
#
# It needs `structure` -- which `document` deliberately does not -- because a
# document holds a graph, and a graph is a container of nodes and edges rather
# than a list of node descriptors. Putting this contract in `document` would
# have meant widening that module's dependency set for a job it explicitly does
# not do: `document` owns "what a saved document is" (identity, title, per-view
# layout slots) and says in its own file comment that it holds no graph.
#
# The FORMAT is not here. JSON, a compact binary, or a teacher's YAML are
# choices, and choices are content (plan-tree.md section 2.2). What cannot be a
# plugin is the shape of the question, so that is what this module owns, and a
# plugin answers it.
#
# Header-only: the module is an interface plus one enum and a description struct.
# There is no state to hide and no implementation to compile, so a STATIC
# library would only add a link step.
add_library(qp_authoring_persist INTERFACE)
add_library(qp::authoring_persist ALIAS qp_authoring_persist)

target_include_directories(qp_authoring_persist INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(qp_authoring_persist INTERFACE cxx_std_20)
# `qp::runtime_store` is here because a saved document now carries the session's measurements. See the layer table's
# entry for this module: the widening is deliberate, and its reason is that losing a student's readings on Save was
# losing the thing the platform exists to produce.
target_link_libraries(qp_authoring_persist INTERFACE
    qp::graph_ir qp::graph_structure qp::authoring_document qp::runtime_store qp_warnings qp_sanitize)
