# plugins/formats/qpjson: the first document format -- JSON text.
#
# The directory is a category (`formats/`) and the plugin is one directory inside it,
# which is how the plan tree groups content: a *kind* of content, then the specific
# pieces of that kind. A second format -- a compact binary for large models, a CSV
# exporter for traces -- becomes a sibling directory rather than a branch inside this
# one, so an unused format can be deleted without reading the one that stays.
#
# The include path does not repeat `formats`: the directory says what kind of content
# this is, the header path says what the thing is. `qp/plugins/qpjson/...` is shorter
# to read at the point of use and does not need editing if the content is ever
# regrouped.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins --
# and this plugin is a good test of it, because it is a *format*: everything it needs
# from the platform is `authoring/persist`'s interface plus the graph it describes.
# Nothing in core knows this file exists.
add_library(qp_plugins_qpjson STATIC
    src/qpjson_format.cpp)

add_library(qp::plugins::qpjson ALIAS qp_plugins_qpjson)

target_include_directories(qp_plugins_qpjson
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_qpjson PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_qpjson
    PUBLIC qp::authoring_persist qp::authoring_document qp::graph_structure qp::graph_ir
           qp::units qp::diag
    PRIVATE qp_warnings qp_sanitize)
