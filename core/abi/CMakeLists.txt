# core/abi：跨边界内存布局。header-only，**零 core 依赖**。
#
# 依赖规则：abi 是唯一连 units 都不 include 的模块。
# 理由见 include/qp/abi.hpp 的说明：ABI 必须能被外部语言绑定独立消费。
# 布局重复（FieldDim vs units::Dim）由 tests/abi/ 的断言守住。

add_library(qp_abi INTERFACE)
add_library(qp::abi ALIAS qp_abi)

target_include_directories(qp_abi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(qp_abi INTERFACE cxx_std_20)
target_link_libraries(qp_abi INTERFACE qp_warnings qp_sanitize)

# FieldBuffer 用 std::atomic，需要线程库
find_package(Threads REQUIRED)
target_link_libraries(qp_abi INTERFACE Threads::Threads)
