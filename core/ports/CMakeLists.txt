# core/ports：端口类型注册表。首个含 .cpp 的模块。
#
# 依赖规则（plan-tree.md §8 与 scripts/check_layers.py 的 ALLOWED）：
#   ports 允许依赖 units、diag；另外实际还用到 abi::LatticeDesc
#   （Value 的 field_handle 载荷），因此 ALLOWED 里也已登记 abi。
#   这条依赖是刻意的：端口要能传递场句柄，而场句柄的布局定义在 abi。

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

# ports 目前不使用线程，但 Value 的 std::string 载荷与注册表的静态初始化
# 需要与其它模块一致的线程配置。用 Threads::Threads 而非裸 `threads`
# ——后者会被当成 -lthreads 传给链接器（MinGW 上没有这个库）。
find_package(Threads REQUIRED)

target_compile_features(qp_ports PUBLIC cxx_std_20)
target_link_libraries(qp_ports
    PUBLIC qp::units qp::diag qp::abi
    PRIVATE qp_warnings qp_sanitize Threads::Threads)
