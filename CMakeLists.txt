cmake_minimum_required(VERSION 3.24)

project(qp
    VERSION 0.1.0
    DESCRIPTION "General-purpose physics experiment simulation platform"
    LANGUAGES CXX)

# ── 选项 ─────────────────────────────────────────────────────────────────────
option(QP_BUILD_TESTS   "构建测试"                             ON)
option(QP_BUILD_VIEWS   "构建 Qt 视图层（消费者）"               OFF)
option(QP_BUILD_PLUGINS "构建插件（内容）"                       OFF)
option(QP_WERROR        "把警告视为错误"                         OFF)
option(QP_SANITIZE      "启用 ASan + UBSan"                     OFF)
option(QP_TIDY          "启用 clang-tidy"                       OFF)
option(QP_FETCH_DEPS    "由 CMake 自动获取测试依赖"              OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ── 警告 ─────────────────────────────────────────────────────────────────────
add_library(qp_warnings INTERFACE)
if(MSVC)
    target_compile_options(qp_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(QP_WERROR)
        target_compile_options(qp_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qp_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
        -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
        -Wimplicit-fallthrough -Wformat=2)
    if(QP_WERROR)
        target_compile_options(qp_warnings INTERFACE -Werror)
    endif()
endif()

add_library(qp_sanitize INTERFACE)
if(QP_SANITIZE)
    target_compile_options(qp_sanitize INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(qp_sanitize INTERFACE -fsanitize=address,undefined)
endif()

# ── 测试依赖：Catch2 v3 ─────────────────────────────────────────────────────
#
# 三级探测，优先级从高到低：
#   1. external/Catch2  —— 由 scripts/fetch_deps.ps1 显式拉取（推荐，离线可用）
#   2. find_package     —— 系统/vcpkg 安装
#   3. FetchContent     —— 仅当显式打开 QP_FETCH_DEPS
#
# 设计意图：**配置阶段默认不联网**。否则"能否配置成功"会取决于网络，
# 这在离线开发和 CI 缓存场景下都不可接受。
if(QP_BUILD_TESTS)
    set(QP_CATCH2_LOCAL "${CMAKE_SOURCE_DIR}/external/Catch2")

    if(EXISTS "${QP_CATCH2_LOCAL}/CMakeLists.txt")
        message(STATUS "Catch2: 使用本地副本 ${QP_CATCH2_LOCAL}")
        add_subdirectory("${QP_CATCH2_LOCAL}" "${CMAKE_BINARY_DIR}/_deps/Catch2" EXCLUDE_FROM_ALL)
        set(QP_CATCH2_SOURCE local)
    else()
        find_package(Catch2 3 QUIET)
        if(Catch2_FOUND)
            message(STATUS "Catch2: 使用系统安装 版本 ${Catch2_VERSION}")
            set(QP_CATCH2_SOURCE package)
        elseif(QP_FETCH_DEPS)
            include(FetchContent)
            FetchContent_Declare(Catch2
                GIT_REPOSITORY https://github.com/catchorg/Catch2.git
                GIT_TAG        v3.5.2)
            FetchContent_MakeAvailable(Catch2)
            list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
            message(STATUS "Catch2: 使用 FetchContent")
            set(QP_CATCH2_SOURCE fetch)
        endif()
    endif()

    if(NOT TARGET Catch2::Catch2WithMain)
        message(FATAL_ERROR
            "需要 Catch2 v3，但三种途径都没有找到。\n"
            "  推荐：pwsh scripts/fetch_deps.ps1\n"
            "  或者：vcpkg install catch2 后加 -DCMAKE_PREFIX_PATH=<前缀>\n"
            "  或者：-DQP_FETCH_DEPS=ON 让 CMake 在配置阶段联网自取")
    endif()
endif()

# ── 目录 ─────────────────────────────────────────────────────────────────────
# 注意：core 层永远在，views / plugins 是消费者，可整体关闭。
add_subdirectory(core)
if(QP_BUILD_VIEWS)
    add_subdirectory(views)
endif()
if(QP_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

# ── 测试 ─────────────────────────────────────────────────────────────────────
if(QP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
