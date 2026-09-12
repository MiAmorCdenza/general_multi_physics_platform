# plugins/formats/csv: the trace export format -- a comma-separated table.
#
# The second member of `formats/`, and the reason the category exists: a document format and a data
# format answer different questions (`authoring/persist` and `runtime/io` respectively), and neither
# should have to be read to change the other. Deleting this directory removes the ability to export a
# trace and changes nothing else.
#
# The exporter is the first **real** `IExporter`. Until this existed, `runtime/io`'s pre-flight had only
# test stubs to check against, so "does this format keep your uncertainties" had never been answered by
# a format that writes a file -- which is also how the missing `could_not_write` refusal was found.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.
add_library(qp_plugins_csv STATIC
    src/csv_exporter.cpp)

add_library(qp::plugins::csv ALIAS qp_plugins_csv)

target_include_directories(qp_plugins_csv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_csv PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_csv
    PUBLIC qp::runtime_io qp::runtime_file qp::runtime_trace qp::runtime_store qp::units
    PRIVATE qp_warnings qp_sanitize)
