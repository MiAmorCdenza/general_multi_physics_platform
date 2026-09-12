# plugins/mechanics: mechanics content -- integrators and models.
#
# This is the first directory under plugins/, and its job is to prove the split
# works: everything here could be deleted and core/ would still build and pass every
# test. Nothing in core/ includes anything from here, and the layer gate refuses it
# if that ever changes.
#
# Nothing here includes a Qt header either. A plugin is content, not a view; the
# same integrator has to be usable from a batch script with no display, from the
# node editor, and from a regression test that runs in CI with no window system at
# all. The moment an integrator needs a QWidget to exist, it stops being usable for
# two of those three.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_mechanics STATIC
    src/rk4_oscillator.cpp
    src/incline.cpp
    src/collision.cpp
    src/batch_operator.cpp
    src/mechanics_binder.cpp)

add_library(qp::plugins::mechanics ALIAS qp_plugins_mechanics)

target_include_directories(qp_plugins_mechanics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_mechanics PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_mechanics
    PUBLIC qp::graph_kernels qp::graph_execution qp::field qp::diag
    PRIVATE qp_warnings qp_sanitize)
