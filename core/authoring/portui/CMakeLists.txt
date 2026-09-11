# core/authoring/portui: port-type-driven property panels.
#
# Dependency rule: portui depends on ports / capability / units / diag
# (see plan-tree.md section 5.5 and the ALLOWED table in scripts/check_layers.py).
#
# The promise this module keeps: adding a port type must not require touching any
# panel code. Each type declares how it is edited, and one generic panel renders
# any declaration.
#
# Only DESCRIPTIONS cross into the view layer -- no widgets, no widget-building
# callbacks, no Qt type anywhere. A description that named a widget class would
# drag Qt into core through the back door, which iron rule 1 forbids.
#
# Header-only would be wrong here even though the code is small: the fallback
# policy is a decision (read_only for an unknown type, never `number`), and a
# decision belongs in one compiled place rather than in every translation unit
# that includes the header.

add_library(qp_authoring_portui STATIC
    src/port_ui.cpp)

add_library(qp::authoring_portui ALIAS qp_authoring_portui)

target_include_directories(qp_authoring_portui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_authoring_portui PUBLIC cxx_std_20)
target_link_libraries(qp_authoring_portui
    PUBLIC qp::ports qp::authoring_capability qp::units qp::diag
    PRIVATE qp_warnings qp_sanitize)
