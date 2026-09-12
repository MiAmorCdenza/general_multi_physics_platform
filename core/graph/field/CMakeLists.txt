# core/graph/field: the minimal contract for field semantics, plus the store a bake publishes into.
#
# Dependency rule: field depends on abi only (see plan-tree.md section 3.8 and
# the ALLOWED table in scripts/check_layers.py).
#
# It does NOT depend on units. A field carries a FieldDim -- the exponents of a
# physical dimension -- and never a checked Quantity: converting every sample of
# a float32 field would be a per-sample cost on a hot path for no information,
# and formatting a unit string is a display concern that belongs to the renderer.
#
# It does NOT depend on the rest of graph either. "Is this buffer usable as a
# field" is a question about a lattice and a byte span, not about nodes and
# edges, and keeping the two apart means a plugin can validate a field without
# pulling in the whole graph. `FieldSet` keeps that property: it is keyed by two
# integers rather than by a `NodeId`, so a baker that has no graph can publish
# into it, and `FieldKey` says "who published this" without saying what a node is.

add_library(qp_field STATIC
    src/field_set.cpp)

add_library(qp::field ALIAS qp_field)

target_include_directories(qp_field
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_field PUBLIC cxx_std_20)
target_link_libraries(qp_field
    PUBLIC qp::abi
    PRIVATE qp_warnings qp_sanitize)
