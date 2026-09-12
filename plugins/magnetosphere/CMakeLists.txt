# plugins/magnetosphere: the geomagnetic field and the particles in it.
#
# **Why this is content and not `core/`.** `core/graph/field` owns exactly three questions -- is this a field, what
# shape is it, which components exist -- and nothing about physics. A dipole is a physics model: it has a moment,
# a tilt, a sign convention and an error of about 11% against the real field. All of that belongs to whoever ships
# it, which is what this directory is.
#
# **Where the field models live, and why they are one node type each.** The reference implementation this kit is
# ported from gives every field model its own node (`dipole`, `t89`, `t96`, `t01`, `t04`, `ts05`, `ta16`) and
# states the principle in its own comments: each node outputs **one** independent force field and the composition
# happens by **wiring the graph**, never inside a node. That is preserved here, because it is the difference
# between "the platform can model a magnetosphere" and "the platform has the magnetosphere we happened to write".
# The Tsyganenko family arrives as further node types under this directory when a course needs them; they are not
# here yet because each needs an external Fortran expansion, and a dependency that heavy must be pulled by a real
# experiment rather than by symmetry.
#
# **What is in this first cut.** `geomagnetic.hpp` (the constants, each with its source), `units.hpp` (the
# normalized system, each conversion written as the SI expression it stands for), `geometry.hpp` (three doubles
# and the cross product, right-handed, with the convention asserted) and `dipole.hpp` / `dipole.cpp` (the field).
# The node type, the field-domain bake and the Boris kernel follow; they are separate commits because each has a
# real consumer to satisfy and folding them together would make a failure ambiguous.
#
# Nothing here includes a Qt header, and nothing here depends on `core/` beyond `units`.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_magnetosphere STATIC
    src/dipole.cpp)

add_library(qp::plugins::magnetosphere ALIAS qp_plugins_magnetosphere)

target_include_directories(qp_plugins_magnetosphere
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_magnetosphere PUBLIC cxx_std_20)

# `qp::units` is PUBLIC because the public headers name `units::Dim` through the field declarations that will
# follow, and because the normalized constants are defined against SI quantities. Nothing else: the dipole's
# arithmetic is `double` and `std::sqrt`, and the module has no opinion about the graph.
target_link_libraries(qp_plugins_magnetosphere
    PUBLIC qp::units qp::diag
    PRIVATE qp_warnings qp_sanitize)
