# plugins/instruments: concrete measuring devices, including their error models.
#
# **Why this is its own content directory rather than part of `mechanics/`.** The plan tree splits content by
# what a thing *is*, not by which experiment uses it: a caliper is not a rigid-body model, and the only
# interface the two share is one they do not share at all -- `mechanics/` implements `IBatchAdvancer` and
# `IStateOperator`, this implements `runtime::IInstrument`.
#
# **Why it is the most important content in the repository.** The charter's differentiator is
# "measure, record, quantify the uncertainty, report" -- and until this directory existed, nothing in
# production implemented `IInstrument`. `core/runtime/instrument` had a full contract, a registry, a fault
# barrier and thirteen test cases, and no device ever registered with it: the platform's central loop was
# reachable from a test fixture and from nowhere else. That is the shape of a promise that quietly does not
# hold, and it is the same defect this project keeps finding -- declared, documented, tested, unconnected.
#
# Nothing here includes a Qt header. A device has to be usable from a headless regression run and from a
# batch script as well as from the editor, and the moment a caliper needs a QWidget to exist it stops being
# usable for two of those three.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_instruments STATIC
    src/ruler.cpp
    src/analogue_meter.cpp
    src/instruments.cpp)

add_library(qp::plugins::instruments ALIAS qp_plugins_instruments)

target_include_directories(qp_plugins_instruments
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_instruments PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_instruments
    PUBLIC qp::runtime_instrument qp::units qp::diag
    # `qp::host` is PUBLIC because the public header includes it: `mount_instruments` takes a
    # `PluginHost&`, and a caller that linked only the runtime would fail to compile on a declaration it
    # cannot see. Making it PRIVATE and keeping the include in the header is the combination that produces
    # "the plugin builds and no consumer does".
    PUBLIC qp::host
    PRIVATE qp_warnings qp_sanitize)
