# plugins/models: concrete physics models as node types.
#
# **The gap this fills is a refusal, not an absence.** `plugins/mechanics`' oscillator integrates the *undamped*
# `x'' = -w^2 x`, and its binder deliberately refuses a node whose damping coefficient is non-zero, because the
# kernel has no damping term -- which is the right answer and leaves a user who wants decay with nowhere to put the
# number. A damped oscillator is the first thing a mechanics lab measures (the decay rate is the measurement), so
# the refusal named a real missing model rather than a limitation to work around.
#
# Three models, one integrator. The Runge-Kutta arithmetic is written once in `Rk4Model` and each model is a
# right-hand side plus its own claims about purity and reversibility; writing the step three times would be three
# places for a coefficient to be mistyped. `models.hpp`'s file comment explains why the state layouts differ and
# why the projectile declares itself **not** time-reversible even though its equations are linear.
#
# Nothing here includes a Qt header: a model has to be runnable from a headless regression test.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_models STATIC
    src/models.cpp
    src/models_binder.cpp)

add_library(qp::plugins::models ALIAS qp_plugins_models)

target_include_directories(qp_plugins_models
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_models PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_models
    # `qp::host` is PUBLIC because the public header includes it: `ModelsBinder::mount` takes a `PluginHost&`, and
    # a caller that linked only the execution engine would fail to compile on a declaration it cannot see.
    PUBLIC qp::graph_execution qp::graph_ir qp::host qp::ports qp::diag
    PRIVATE qp_warnings qp_sanitize)
