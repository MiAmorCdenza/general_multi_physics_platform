# core/authoring/capability: capability negotiation.
#
# Dependency rule: capability depends on plugin / diag
# (see plan-tree.md section 5.2 and the ALLOWED table in scripts/check_layers.py).
#
# Separation of duties: core/plugin answers "may this plugin load", which is a
# question about one manifest. This module answers "who can do X", which is a
# question about the whole set of loaded plugins. Keeping them apart means a
# permission decision is made once, from the manifest, and never re-litigated by
# a caller scanning for a provider.

add_library(qp_authoring_capability STATIC
    src/capability.cpp)

add_library(qp::authoring_capability ALIAS qp_authoring_capability)

target_include_directories(qp_authoring_capability
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_authoring_capability PUBLIC cxx_std_20)
target_link_libraries(qp_authoring_capability
    PUBLIC qp::plugin
    PRIVATE qp_warnings qp_sanitize)
