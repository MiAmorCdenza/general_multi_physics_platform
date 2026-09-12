# core/runtime/io: the export contract and the format registry.
#
# Dependency rule: io depends on store / trace / diag
# (see plan-tree.md section 4.4 and the ALLOWED table in scripts/check_layers.py).
#
# The decision this module encodes: **the formats are plugins.** CSV, TSV, JSON,
# HDF5, a spreadsheet, a figure, a lab's own templated report -- each is defensible,
# and which is right depends on the course, the institution and the journal. A
# platform that embedded CSV would be deciding for everybody, and the second format
# it needed would be a core change.
#
# So this module owns the interface and the registry and writes no file. What it
# does own is the pre-flight check, which is where the charter's promise 3 lives:
# "changing an instrument's precision must actually change the final uncertainty"
# survives an export only if the format can carry the uncertainty, and the caller
# can find that out *before* the write rather than after.

add_library(qp_runtime_io STATIC
    src/io.cpp)

add_library(qp::runtime_io ALIAS qp_runtime_io)

target_include_directories(qp_runtime_io
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_runtime_io PUBLIC cxx_std_20)
target_link_libraries(qp_runtime_io
    PUBLIC qp::runtime_store qp::runtime_trace qp::diag
    PRIVATE qp_warnings qp_sanitize)
