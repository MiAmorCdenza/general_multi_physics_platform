# core/runtime/file: whole-file I/O, and the platform's one UTF-8 path conversion.
#
# Dependency rule: file depends on nothing but the standard library (see the ALLOWED
# table in scripts/check_layers.py). It is deliberately below the format contracts
# rather than inside one of them: a CSV trace exporter and the document persistence
# contract both need to move bytes to and from a path, and either one hosting the
# helper would make the other depend on a module whose subject is something else.
#
# What it owns is small and worth naming: reading and writing a whole file **as
# binary**, telling "no such file" apart from "not allowed to read it", and turning a
# UTF-8 path into whatever the platform's open call actually wants. That last one is
# the reason the module exists -- on Windows a narrow path is read in the process code
# page, so a directory whose name is not ASCII opens something else or nothing.

add_library(qp_runtime_file STATIC
    src/file.cpp)

add_library(qp::runtime_file ALIAS qp_runtime_file)

target_include_directories(qp_runtime_file
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_runtime_file PUBLIC cxx_std_20)
target_link_libraries(qp_runtime_file
    PUBLIC qp::diag
    PRIVATE qp_warnings qp_sanitize)
