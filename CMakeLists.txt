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
set(QP_QT_VERSION "" CACHE STRING
    "Qt version to look for under QP_QT_ROOT; empty means the newest one found")

if(QP_BUILD_VIEWS)
    # The kit is **discovered**, not spelled out. An earlier version of this file
    # hard-coded `6.8.3/msvc2022_64`, which meant a Qt upgrade -- or a colleague with
    # a different Qt installed -- silently configured with no Qt at all, and the
    # symptom was `find_package(Qt6)` failing in views/ rather than a message naming
    # the version mismatch. Discovery also keeps the version out of two places.
    #
    # The search order is: an explicit QP_QT_VERSION, then QP_QT_ROOT, then the
    # conventional C:/Qt. Newest first in each, so an upgrade is picked up without
    # editing anything.
    set(_qt_candidates "")
    foreach(_root IN ITEMS "${QP_QT_ROOT}" "C:/Qt")
        if(NOT EXISTS "${_root}")
            continue()
        endif()
        if(QP_QT_VERSION)
            file(GLOB _versions "${_root}/${QP_QT_VERSION}")
        else()
            file(GLOB _versions "${_root}/*.*")
        endif()
        list(SORT _versions COMPARE NATURAL ORDER DESCENDING)
        foreach(_version_dir IN LISTS _versions)
            # Only the MSVC kit: there is no 32-bit Qt 6, and this project's GCC
            # toolchain is MinGW i686, so a MinGW kit would not match the build.
            file(GLOB _kits "${_version_dir}/msvc*_64")
            foreach(_kit IN LISTS _kits)
                if(EXISTS "${_kit}/lib/cmake/Qt6/Qt6Config.cmake")
                    list(APPEND _qt_candidates "${_kit}")
                endif()
            endforeach()
        endforeach()
    endforeach()

    if(_qt_candidates)
        list(GET _qt_candidates 0 QP_QT_KIT)
        list(APPEND CMAKE_PREFIX_PATH "${QP_QT_KIT}")
        message(STATUS "Qt: using kit ${QP_QT_KIT}")
    else()
        message(STATUS "Qt: no kit found under ${QP_QT_ROOT} or C:/Qt. "
                       "Run: pwsh scripts/fetch_deps.ps1 -WithQt")
    endif()
    unset(_qt_candidates)
    unset(_versions)
    unset(_kits)
    unset(_version_dir)
    unset(_root)

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

# The view layer, in two halves behind two different guards.
#
# `views/model` is Qt-free and `views` needs Qt. Both are **content**: deleting `views/`
# is a thing `docs/plan-tree.md` states as the core assertion of the whole layering
# scheme, and a downstream consumer vendoring `core/` alone will do it.
#
# The Qt-free half is guarded on existence **only**, not on an option, and the
# distinction is the difference between two real costs. `views/model` holds the property
# panel's editor-selection rule -- the part of the view layer most likely to be subtly
# wrong -- and it is plain C++ precisely so the GCC build can assert it. Putting it
# behind QP_BUILD_VIEWS would chain it to the Qt option, and Qt 6 ships no 32-bit kit, so
# the GCC build can never set that option: the coverage would disappear while every count
# in the suite still looked healthy.
#
# The original code was an unconditional `add_subdirectory(views/model)` with a comment
# beside it saying core must not depend on views -- while the line itself made core's
# configuration fail without it:
#
#     add_subdirectory given source "views/model" which is not an existing directory
#
# Nothing caught it. The layer gate reads `#include` lines and there is no include
# pointing at `views/model`; the dependency lived in the build system. A claim was in a
# document and the violation was in a one-line call, which is the shape of every promise
# that quietly stops being true.
#
# `EXISTS` is what makes the directory genuinely optional: present, it builds everywhere;
# absent, nothing references it and configuration proceeds.
#
# `tests/meta/check_core_standalone.py` configures a copy of this tree with the content
# directories removed, so the next time one of these lines loses its guard the suite says
# so instead of a consumer's build saying so.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/views/model")
    add_subdirectory(views/model)
endif()
if(QP_BUILD_VIEWS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/views")
    add_subdirectory(views)
endif()
if(QP_BUILD_PLUGINS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
    add_subdirectory(plugins)
endif()

# -- Tests --------------------------------------------------------------------
if(QP_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
