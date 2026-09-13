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
# **What is in this cut.** `geomagnetic.hpp` (the constants, each with its source), `units.hpp` (the normalized
# system, each conversion written as the SI expression it stands for), `geometry.hpp` (three doubles and the cross
# product, right-handed, with the convention asserted), `dipole.hpp` / `dipole.cpp` (the field),
# `baked_field.hpp` / `baked_field.cpp` (the model evaluated once onto a lattice, read back by trilinear
# interpolation) and `boris.hpp` / `boris.cpp` (the pusher, as a particle-domain kernel). The node type, the
# field-domain bake and the composition root that binds a field node to a particle slot follow; they are separate
# commits because each has a real consumer to satisfy and folding them together would make a failure ambiguous.
#
# `boris.cpp` costs `qp::graph_kernels`, `qp::graph_particles`, `qp::field` and `qp::abi`, because a pusher **is**
# an `IBatchAdvancer` reading a bound `field::FieldValue` out of the batch the executor builds. The alternative --
# a private particle loop with its own field sampling and its own state arrays -- is the parallel implementation
# this platform exists to not have, and it is exactly what would keep a kit from using the trace, the store or the
# uncertainty chain.
#
# `field_nodes.cpp` and `plan.cpp` add `qp::graph_eval`, `qp::graph_ir`, `qp::graph_structure` and `qp::host`:
# the first because a field node is an `INodeEvaluator` and the second because the composition root reads the
# graph. Those are the widest dependencies any plugin in this tree has, and they are the price of being a
# **domains** kit rather than a model: a node type is a graph citizen, so it needs the graph's vocabulary.
#
# `trace.cpp` is the kit's second numerical method, beside the pusher, and the same argument puts it here rather
# than in `core/graph/field`: that module says what a field *is* and holds no algorithm, and `core/graph/kernels`
# says by its own words that it "contains not a single integrator". How a curve is found is a numerical method
# with a tolerance, which is content -- a cheaper Euler tracer for a preview is a second function here, not a
# change to any interface.
#
# Nothing here includes a Qt header, and nothing here depends on `core/` beyond the graph and the host.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_magnetosphere STATIC
    src/baked_field.cpp
    src/boris.cpp
    src/dipole.cpp
    src/emitter.cpp
    src/field_nodes.cpp
    src/plan.cpp
    src/render_nodes.cpp
    src/run.cpp
    src/trace.cpp
    src/view_item.cpp)

add_library(qp::plugins::magnetosphere ALIAS qp_plugins_magnetosphere)

target_include_directories(qp_plugins_magnetosphere
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_magnetosphere PUBLIC cxx_std_20)

# `qp::units` is PUBLIC because the public headers name `units::Dim` through the field declarations they use, and
# because the normalized constants are defined against SI quantities. The graph modules are PUBLIC for the same
# reason: `boris.hpp` derives from `IBatchAdvancer` and names `field::FieldValue` and `BatchSlot` in its own
# signatures, so anyone using this library needs their headers.
target_link_libraries(qp_plugins_magnetosphere
    PUBLIC qp::units qp::diag qp::abi qp::field qp::graph_kernels qp::graph_particles
           qp::graph_eval qp::graph_ir qp::graph_structure qp::graph_domain qp::graph_execution qp::host
    PRIVATE qp_warnings qp_sanitize)
