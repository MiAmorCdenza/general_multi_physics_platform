# core/plugin: the plugin contract -- manifest schema, compatibility judgement, load
# order, and the exported C entry point that maps a file into this process.
#
# Dependency rule: plugin depends on abi / diag / ports / reflect
# (see plan-tree.md section 2.4 and the ALLOWED table in scripts/check_layers.py).
#
# It does NOT parse a file. The manifest schema is foundation; the format that
# carries it is a serialisation choice and therefore a plugin, exactly as
# plan-tree.md section 2.2 states for the ABI itself. A YAML adapter builds a
# Manifest and nothing downstream can tell where it came from.
#
# It DOES map a shared library, and that is a deliberate narrowing of the original
# "not here" decision. The reasoning that put it here rather than in a separate
# module is that the loader has no dependencies of its own: it needs only diag and
# this module's Manifest. A new module would have needed a new layer entry, a new
# spec, and a new contract surface, to hold two platform calls and an ordering
# argument. What mattered about the original decision -- that the platform-specific
# part stays in one place and out of the header -- is kept: all of it is inside
# src/loader.cpp, and loader.hpp includes no system header.
#
# `dl` is linked only where the C library does not already provide it.

add_library(qp_plugin STATIC
    src/manifest.cpp
    src/loader.cpp
    src/guard.cpp)

add_library(qp::plugin ALIAS qp_plugin)

target_include_directories(qp_plugin
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_plugin PUBLIC cxx_std_20)
target_link_libraries(qp_plugin
    PUBLIC qp::abi qp::diag qp::ports
    PRIVATE qp_warnings qp_sanitize)

# libdl is separate from libc on Linux and on the BSDs; on Windows the dynamic
# loader is in kernel32, which the toolchain links by default. Adding it
# unconditionally on Windows would be harmless but a lie about what is needed.
if(UNIX AND NOT APPLE)
    target_link_libraries(qp_plugin PRIVATE dl)
endif()
