# core/runtime/run: the identity of one experiment run.
#
# Dependency rule: run depends on units / diag / abi / ports
# (see plan-tree.md section 4.1 and the ALLOWED table in scripts/check_layers.py).
#
# Why this module is foundation rather than a consumer's concern: charter R2
# (reproduce from the same seed) and C1 (deterministic replay) both hang on it,
# and a run identity that each plugin invented for itself would not be comparable
# across plugins -- which is the only thing that makes it useful.
#
# Note what this module does NOT do: it does not run anything. It records what a
# run was made of. Executing belongs to whoever owns the graph, and keeping the
# record separate means a record can be written, compared and audited without a
# scheduler in the picture.
#
# The seed belongs to the RUN, not to a plugin's private state. That is the
# decision the plan tree flags, and it is why `begin()` takes the seed rather than
# letting a plugin ask for one: a plugin that owned its own seed could not be
# reproduced without also reproducing the order in which it was asked.

add_library(qp_runtime_run STATIC
    src/run.cpp)

add_library(qp::runtime_run ALIAS qp_runtime_run)

target_include_directories(qp_runtime_run
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_runtime_run PUBLIC cxx_std_20)
target_link_libraries(qp_runtime_run
    PUBLIC qp::units qp::diag qp::abi qp::ports
    PRIVATE qp_warnings qp_sanitize)
