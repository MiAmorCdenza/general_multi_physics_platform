# plugins/formats/value_text: a port value as text, so two formats need one conversion
#
# **Why this is not inside `qpjson`.** It was, until a second format became a real prospect: the conversion from a
# `ports::Value` to something a file can hold is one decision, and two copies of it drift -- with the symptom that
# one format reads a parameter the other wrote as a different thing. That is the failure the module split exists to
# prevent, one layer down.
#
# It is deliberately **not** a format. It produces a kind tag and a text payload, and the format decides which
# quoting or indentation carries those two strings: this layer knows what a parameter *is*, and a format knows how
# a file spells it. The three refusals every text format needs -- a field handle, a non-UTF-8 string, a non-finite
# number -- live here, once, so a new format inherits them rather than re-deriving them.
#
# Nothing here includes a Qt header.
#
# Dependency rule: plugins may depend on core, core may never depend on plugins.

add_library(qp_plugins_value_text STATIC
    src/value_text.cpp)

add_library(qp::plugins::value_text ALIAS qp_plugins_value_text)

target_include_directories(qp_plugins_value_text
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(qp_plugins_value_text PUBLIC cxx_std_20)

target_link_libraries(qp_plugins_value_text
    PUBLIC qp::authoring_persist qp::ports qp::diag
    PRIVATE qp_warnings qp_sanitize)
