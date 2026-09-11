# core/ports: the port type registry. The first module with .cpp files.
#
# Dependency rule (plan-tree.md section 8 and ALLOWED in scripts/check_layers.py):
#   ports may depend on units and diag; it also uses abi::LatticeDesc (the field_handle
#   payload of Value), so abi is registered in ALLOWED too. That dependency is
#   deliberate: ports carry field handles, and a field handle's layout lives in abi.

add_library(qp_ports STATIC
    src/registry.cpp
    src/check.cpp)

add_library(qp::ports ALIAS qp_ports)

target_include_directories(qp_ports
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ports does not use threads today, but the std::string payload of Value and the static
# initialization of the registry need the same thread setup as every other module: use
# Threads::Threads, not a bare `threads`, which reaches the linker as a missing -lthreads.
find_package(Threads REQUIRED)

target_compile_features(qp_ports PUBLIC cxx_std_20)
target_link_libraries(qp_ports
    PUBLIC qp::units qp::diag qp::abi
    PRIVATE qp_warnings qp_sanitize Threads::Threads)
