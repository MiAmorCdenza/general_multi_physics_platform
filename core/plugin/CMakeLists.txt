# core/plugin: the plugin contract -- manifest schema, compatibility judgement, load order.
#
# Dependency rule: plugin depends on abi / diag / ports / reflect
# (see plan-tree.md section 2.4 and the ALLOWED table in scripts/check_layers.py).
#
# It does NOT parse a file. The manifest schema is foundation; the format that
# carries it is a serialisation choice and therefore a plugin, exactly as
# plan-tree.md section 2.2 states for the ABI itself. A YAML adapter builds a
# Manifest and nothing downstream can tell where it came from.
#
# It does NOT load a shared library either. That is platform-specific, needs a
# filesystem and a dynamic loader, and is the one part of a plugin system that is
# genuinely different on every OS. What is here is everything that can be decided
# **before** any code is mapped into the process: identity, version, layout,
# capabilities, dependency order. Deciding early is the point -- a version
# mismatch discovered after dlopen has already run a constructor is too late.

add_library(qp_plugin STATIC
    src/manifest.cpp)

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
