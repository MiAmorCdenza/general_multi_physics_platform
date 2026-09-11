# core/diag: error channel and unified logging.
#
# Dependency rule: diag may depend on units, but in practice depends on no core
# module at all (see plan-tree.md section 8, iron rule 4, and the ALLOWED table
# in scripts/check_layers.py).
#
# Static rather than INTERFACE since logging.cpp exists: the JSON formatter is
# real code, and header-only would force every consumer to compile it.

# CollectingSink uses a mutex, so the thread library must be resolved before the
# link line is generated.
find_package(Threads REQUIRED)

add_library(qp_diag STATIC
    src/logging.cpp)

add_library(qp::diag ALIAS qp_diag)

target_include_directories(qp_diag
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(qp_diag PUBLIC cxx_std_20)
target_link_libraries(qp_diag
    PUBLIC Threads::Threads
    PRIVATE qp_warnings qp_sanitize)
