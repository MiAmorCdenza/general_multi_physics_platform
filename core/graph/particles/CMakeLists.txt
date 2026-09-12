# core/graph/particles: where a particle batch lives, and the loop that drives kernels over it.
#
# **Why this module exists, and what it is evidence about.** `graph/kernels` has had a complete contract since it
# was written -- `IBatchAdvancer`, `BatchView`, `ParamBlock`, `AdvanceContext`, `ClampPolicy`, `KernelRegistry` --
# and until this module, **no production code ever called any of it**. `KernelRegistry` appeared in the host's own
# test and nowhere else; nothing in the tree had constructed a `BatchView` or driven a single step. That is the
# defect this project keeps finding: a contract declared, documented and unit-tested, and unreachable from the
# running program. `graph/domain` decides *which* nodes belong to the particle domain and in what order and stops
# there; this module carries the plan the rest of the way.
#
# **What is foundation here and what is content.** Two things, and neither is physics:
#
#   - `ParticleState` -- where a batch lives, as four typed slots with the dimensions they hold. A kernel reads
#     positions in metres, velocities in metres per second, and a charge-to-mass ratio in coulombs per kilogram,
#     and the descriptions are what say so.
#   - `ParticleExecutor` -- prepare once, advance per step, clamp and count what a kernel wrote.
#
# The **mapping** from a node's parameters to a kernel's parameter block is content, exactly as it is for
# `IStateOperator`: it knows which of a node's parameters mean what, and it belongs to whichever plugin ships the
# kernel. That is why this directory has no dependency on `domain` -- see the ALLOWED table for the long version.
#
# Dependency rule: units / diag / abi / field / kernels (see plan-tree.md section 3.7 and scripts/check_layers.py).

add_library(qp_graph_particles STATIC
    src/particle_state.cpp
    src/executor.cpp)

add_library(qp::graph_particles ALIAS qp_graph_particles)

target_include_directories(qp_graph_particles
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_graph_particles PUBLIC cxx_std_20)

# `abi` is PUBLIC because the public header names `abi::ElementType` and `abi::ComponentKind` in the slot table
# and `abi::LatticeDesc` through `field::FieldValue`; a consumer that linked only `field` would fail to compile on
# a type it cannot see. `kernels` is PUBLIC because `ParticleExecutor` takes a `std::vector<StepPlan>` whose
# `StepPlan` holds an `IBatchAdvancer*` and a `ParamBlock` -- both are the interface's, not this module's.
target_link_libraries(qp_graph_particles
    PUBLIC qp::abi qp::diag qp::units qp::field qp::graph_kernels
    PRIVATE qp_warnings qp_sanitize)
