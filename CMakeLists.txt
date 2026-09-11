cmake_minimum_required(VERSION 3.24)

# Note on MSVC dependency tracking: it is NOT fixed here.
#
# Ninja matches cl.exe's `/showIncludes` output against `msvc_deps_prefix` to
# learn a translation unit's header dependencies. The prefix is localized, and
# CMake detects it during compiler detection with `execute_process(ENCODING
# AUTO)`, which decodes using the **console code page** while cl.exe writes
# UTF-8. On a legacy code page (437, 936, ...) CMake records mojibake, Ninja
# matches nothing, and dependency tracking fails silently: headers change,
# Ninja says "no work to do", and tests run against stale binaries.
#
# The fix has to happen before `cmake` is invoked, because the code page is
# process-global: see the `chcp 65001` in scripts/build.ps1. Setting VSLANG here
# does not work -- measured on this machine, cl.exe prints the localized prefix
# regardless of VSLANG, because the language comes from the installed MUI
# language pack rather than the environment.
#
# `scripts/check_build_deps.ps1` is the regression test; run it against
# build-msvc after any change to the build entry points.

project(qp
    VERSION 0.1.0
    DESCRIPTION "General-purpose physics experiment simulation platform"
    LANGUAGES CXX)

# -- Options ------------------------------------------------------------------
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

# -- Warnings -----------------------------------------------------------------
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

# -- Test dependency: Catch2 v3 ----------------------------------------------
#
# Three probes, highest priority first:
#   1. external/Catch2  ---- fetched by scripts/fetch_deps.ps1 (recommended; works offline)
#   2. find_package     ---- a system / vcpkg install
#   3. FetchContent     ---- only when QP_FETCH_DEPS is explicitly enabled
#
# Design intent: **configure does not touch the network by default**. Otherwise
# "will configure succeed" would depend on the network, unacceptable offline and in CI.
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

# -- Qt discovery -------------------------------------------------------------
#
# Qt is searched for here rather than inside views/ for one reason: to fail with a
# useful message. A missing Qt inside views/ produces a `find_package` error that
# names a package, while the actual problem is usually "the GUI is a consumer and
# you have not pointed the build at a Qt yet".
#
# Two facts worth knowing before configuring with QP_BUILD_VIEWS=ON:
#
#   1. **The MSVC kit only.** Qt 6 ships `win64_msvc2022_64` and a 64-bit MinGW
#      kit; there is no 32-bit Qt 6 at all. This project's GCC toolchain is MinGW
#      i686, so the view layer cannot be built with it -- the GUI is an MSVC x64
#      target. That is a property of Qt, not a compromise in the architecture:
#      core stays compiler-agnostic and is verified on both compilers.
#
#   2. **Not QML.** Qt Quick lives in the `qtdeclarative` module, which is a
#      separate download. The decision recorded in ADR-0006 is Widgets, so the
#      required components below are Core, Gui and Widgets only.
#
# Acquisition is scripted: scripts/fetch_deps.ps1 -WithQt installs Qt into
# external/Qt via aqtinstall. Override the location with -DQP_QT_ROOT=<path>.
set(QP_QT_ROOT "${CMAKE_SOURCE_DIR}/external/Qt" CACHE PATH
    "Root under which Qt kits are installed (qp fetch_deps.ps1 -WithQt)")

if(QP_BUILD_VIEWS)
    # Prefer an explicit QP_QT_ROOT; also accept an existing CMAKE_PREFIX_PATH or
    # the conventional C:/Qt location, so a machine with Qt already installed works
    # without being told twice.
    set(QP_QT_KIT "${QP_QT_ROOT}/6.8.3/msvc2022_64")
    if(EXISTS "${QP_QT_KIT}/lib/cmake/Qt6/Qt6Config.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${QP_QT_KIT}")
        message(STATUS "Qt: using kit ${QP_QT_KIT}")
    elseif(EXISTS "C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/Qt6Config.cmake")
        list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.8.3/msvc2022_64")
        message(STATUS "Qt: using kit C:/Qt/6.8.3/msvc2022_64")
    else()
        message(STATUS "Qt: no kit under QP_QT_ROOT (${QP_QT_ROOT}); "
                       "falling back to find_package's own search")
    endif()

    # Found here rather than inside views/ so that the imported targets are visible
    # to the whole project. A package located in a subdirectory yields targets
    # scoped to that directory, and tests/ legitimately needs Qt6::Core -- to learn
    # where Qt's DLLs live, so CTest can launch a GUI test at all.
    #
    # Only Core, Gui and Widgets. Qt Charts is deliberately absent: it is GPL-3.0
    # rather than LGPL, and scripts/check_qt_modules.py fails the build if it ever
    # appears on a link line.
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
endif()

# -- Directories --------------------------------------------------------------
# Note: the core layer is always present; views / plugins are consumers, switchable off
add_subdirectory(core)

# The Qt-FREE half of the view layer, added unconditionally.
#
# It lives outside core/ because it is a consumer -- core must not depend on it --
# but it needs no Qt, so it is built in every configuration. That is deliberate: the
# property panel's editor-selection rule is the part of the view layer most likely
# to be subtly wrong, and keeping it here means it is asserted on BOTH compilers,
# including the GCC build that cannot link Qt at all.
#
# The Qt half (views/) stays behind QP_BUILD_VIEWS.
add_subdirectory(views/model)

if(QP_BUILD_VIEWS)
    add_subdirectory(views)
endif()
if(QP_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

# -- Tests --------------------------------------------------------------------
if(QP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
