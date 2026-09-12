# core/runtime/instrument: instruments as first-class citizens (charter C6).
#
# Dependency rule: instrument depends on units, diag, store and plugin (see the
# ALLOWED table in scripts/check_layers.py).
#
# It sits in L2 beside `store` because a reading **is** a `store::UncertainValue` --
# inventing a parallel reading type would create exactly the two-representations
# problem the platform keeps paying to avoid. It depends on `plugin` for one reason:
# the host calls an instrument through the same fault barrier as an evaluator or an
# operator, so `measure_guarded` is the only sanctioned way in (charter C4).
#
# Why this module exists at all, given that no instrument ships with the framework:
# charter C6 says `IInstrument` and the simulation-model interface must be established
# **together**, "not bolted on later". An instrument is where the platform's closed
# loop starts -- measure -> record -> uncertainty -> report -- and a framework that let a value
# be produced without saying whether it was measured or computed could not express the
# one distinction every later stage depends on. The contract is foundation; the devices
# are content, and arrive as plugins.

add_library(qp_runtime_instrument STATIC
    src/instrument.cpp)

add_library(qp::runtime_instrument ALIAS qp_runtime_instrument)

target_include_directories(qp_runtime_instrument
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_runtime_instrument PUBLIC cxx_std_20)
target_link_libraries(qp_runtime_instrument
    PUBLIC qp::units qp::diag qp::runtime_store qp::plugin
    PRIVATE qp_warnings qp_sanitize)
