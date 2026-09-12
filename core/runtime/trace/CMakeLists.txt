# core/runtime/trace: the timeline.
#
# Dependency rule: trace depends on run / store / units / diag
# (see plan-tree.md section 4.3 and the ALLOWED table in scripts/check_layers.py).
#
# The decision this module encodes, from charter C3: **the timeline is a platform
# mechanism, not a view's private state.** The obvious implementation is for the
# plot widget to keep the samples it drew. It works until a second view needs them
# -- a table, an export, a fit -- and then the copies disagree because they were
# sampled at different moments, and a *saved* run has no timeline at all, so the
# numbers in a report cannot be traced back to the run that produced them.
#
# What is deliberately absent: no resampling, no smoothing, no interpolation, no
# statistics beyond the extent. Those are analyses with several defensible answers
# and they belong to plugins; runtime/store already owns the one statistic that is
# definitional (the Type-A uncertainty of a mean).

add_library(qp_runtime_trace STATIC
    src/trace.cpp)

add_library(qp::runtime_trace ALIAS qp_runtime_trace)

target_include_directories(qp_runtime_trace
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_runtime_trace PUBLIC cxx_std_20)
target_link_libraries(qp_runtime_trace
    PUBLIC qp::runtime_run qp::runtime_store qp::units qp::diag
    PRIVATE qp_warnings qp_sanitize)
