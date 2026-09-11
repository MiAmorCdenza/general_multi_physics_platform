# core：地基 + 图 + 运行时 + 视图服务
#
# 铁律（standards/enforcement.md §4）：
#   core 下不得出现任何 Qt 头文件，不得依赖 views/ 或 plugins/。
#   本目录必须能在 QP_BUILD_VIEWS=OFF 且未安装 Qt 的机器上完整构建并通过测试。

add_subdirectory(units)
