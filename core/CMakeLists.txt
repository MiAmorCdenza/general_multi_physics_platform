# core: foundation + graph + runtime + view services
#
# Iron rule (standards/enforcement.md section 4):
#   No Qt header may appear anywhere under core, and core must not depend on
#   views/ or plugins/. This directory must build and pass tests with QP_BUILD_VIEWS=OFF

add_subdirectory(units)
add_subdirectory(diag)
add_subdirectory(abi)
add_subdirectory(ports)
add_subdirectory(plugin)
add_subdirectory(graph)
