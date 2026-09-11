# core/diag：错误通道。header-only，零依赖。
#
# 依赖规则：diag 允许依赖 units，但当前实际不依赖任何 core 模块
#（见 plan-tree.md §8 铁律 4 与 scripts/check_layers.py 的 ALLOWED）。

add_library(qp_diag INTERFACE)
add_library(qp::diag ALIAS qp_diag)

target_include_directories(qp_diag INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(qp_diag INTERFACE cxx_std_20)
target_link_libraries(qp_diag INTERFACE qp_warnings qp_sanitize)

# 诊断模块用到线程（CollectingSink 的互斥）
find_package(Threads REQUIRED)
target_link_libraries(qp_diag INTERFACE Threads::Threads)
