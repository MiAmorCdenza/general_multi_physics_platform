# Tests: the eight test categories map onto the directory structure (standards/test-taxonomy.md)
#
#   unit/        category 1 unit + category 3 property (marked with the [property] tag)
#   abi/         category 2 static layout contract
#   golden/      category 4 golden regression
#   determinism/ category 5 determinism
#   contract/    category 8 contract compliance suite
#   smoke/      proof that core stands alone
#   meta/       tests of the gates themselves
#
# Catch2's automatic discovery is not used (Catch2Main registers the cases itself), so each
# executable only needs to be registered with CTest once.

# Shared test entry point: turns off the MSVC debug runtime's abort dialog (see that file).
set(QP_TEST_MAIN ${CMAKE_CURRENT_SOURCE_DIR}/support/test_main.cpp)

# Catch2's extras module provides catch_discover_tests
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/Catch2/extras")
if(EXISTS "${CMAKE_SOURCE_DIR}/external/Catch2/extras/Catch.cmake")
    include(Catch)
else()
    include(Catch OPTIONAL RESULT_VARIABLE _catch_included)
    if(_catch_included STREQUAL "NOTFOUND")
        message(FATAL_ERROR "找不到 Catch2 的 Catch.cmake（extras）。"
            "若用系统安装的 Catch2，请确认 CMAKE_MODULE_PATH 含其 extras 目录。")
    endif()
endif()

# -- Python interpreter (the gate scripts need it) ---------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)

# -- Contract completeness gate (enforcement.md §5) --------------------------
if(Python3_Interpreter_FOUND)
    # `--exclude views` hands the view layer's tests to the second invocation
    # below. Test ownership is a global judgement: the gate checks both that every
    # @tests id exists and that every TEST_CASE is claimed, so two invocations must
    # see **disjoint** sets of test cases or each reports the other's legitimate
    # cases as orphans.
    add_test(NAME gate.contracts
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_contracts.py
                --headers ${CMAKE_SOURCE_DIR}/core
                --tests   ${CMAKE_SOURCE_DIR}/tests
                --exclude views
                --quiet)
    set_tests_properties(gate.contracts PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The Qt-free half of the view layer has contract blocks of its own, and they
    # are NOT covered by the gate above: that scan's header root is core/.
    #
    # Wiring them in needs one more thing first -- a test root containing only the
    # model tests -- because the gate judges orphan-freedom globally, so any other
    # directory in scope makes every core test case look unclaimed. The model tests
    # currently share tests/unit/views/ with the Qt-side file, which the gate cannot
    # see at all (Qt does not compile there). Recorded as follow-up rather than
    # approximated with a scope that would report hundreds of false orphans.
    message(STATUS "gate.contracts covers core/; views/model contracts are not yet gated")

else()
    message(WARNING "未找到 Python3，跳过 gate.contracts（契约门禁未启用）")
endif()

# -- Test targets -------------------------------------------------------------
#
# catch_discover_tests instead of add_test(COMMAND <exe>):
#   what gets registered with CTest is **each TEST_CASE**, not the whole executable.
#   Benefits: the CI log names the failing case; filtering by [property] / [golden] works;
#         the failure granularity matches the contract @tests granularity (contracts name TEST_CASE ids).
#
# Each executable also keeps a <target>.all aggregate test for a fast local full run.
#
# Catch2::Catch2 plus a custom main (not Catch2WithMain):
#   the death test deliberately calls abort(), and the MSVC debug runtime turns abort() into a
#   **modal dialog**, so the process blocks until someone clicks a button -- automation hangs forever.
#   tests/support/test_main.cpp turns that dialog off (keeping the diagnostic text on stderr).
# OWN_MAIN: skip the shared test main. A test that needs to construct something
# before any test case runs -- a QApplication, most obviously -- has to provide its
# own, and linking both would be a duplicate-symbol error rather than a warning.
function(qp_add_test target)
    cmake_parse_arguments(ARG "OWN_MAIN" "" "SOURCES;LIBS;LABELS" ${ARGN})
    if(NOT ARG_LABELS)
        set(ARG_LABELS "unit")
    endif()

    set(_main ${QP_TEST_MAIN})
    if(ARG_OWN_MAIN)
        set(_main "")
    endif()
    add_executable(${target} ${ARG_SOURCES} ${_main})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE qp::units Catch2::Catch2 ${ARG_LIBS})

    catch_discover_tests(${target}
        TEST_PREFIX "${target}."
        PROPERTIES LABELS "${ARG_LABELS}"
        ADD_TAGS_AS_LABELS)          # tags such as [golden] / [property] become CTest labels directly

    # Aggregate test: run every case of that executable in one go
    add_test(NAME "${target}.all" COMMAND ${target})
    set_tests_properties("${target}.all" PROPERTIES
        LABELS "${ARG_LABELS};aggregate"
        RUN_SERIAL FALSE)
endfunction()

if(NOT TARGET Catch2::Catch2WithMain)
    message(FATAL_ERROR "Catch2 v3 未就绪（根 CMakeLists.txt 应已给出具体原因）")
endif()

qp_add_test(qp_test_units_dim
    SOURCES unit/units/test_dim.cpp)
qp_add_test(qp_test_units_quantity
    SOURCES unit/units/test_quantity.cpp)
qp_add_test(qp_test_units_conversion
    SOURCES unit/units/test_conversion.cpp)
qp_add_test(qp_test_floating_point_env
    SOURCES unit/units/test_floating_point_env.cpp
    LABELS "golden")
qp_add_test(qp_test_units_symbol_golden
    SOURCES golden/units/test_unit_symbol.cpp
    LABELS "golden")
qp_add_test(qp_test_core_standalone
    SOURCES smoke/test_core_standalone.cpp
    LABELS "smoke")

qp_add_test(qp_test_diag
    SOURCES unit/diag/test_diag.cpp
    LIBS qp::diag)
qp_add_test(qp_test_diag_log
    SOURCES unit/diag/test_log.cpp
    LIBS qp::diag)

# -- Death test: verify that "violating @pre always terminates the process" --
#
# The case under test carries the `[.]` tag (Catch2 hidden), so catch_discover_tests does not
# register an ordinary test for it -- and that matters: otherwise it would pass as SKIP and
# hide the fact that the death test never ran.
#
# The criterion here is the **process exit code**:
#   - PASS_REGULAR_EXPRESSION requires the termination reason to really be a contract violation,
#     otherwise any crash would make this test "pass".
#   - It also requires Catch2 to report "no failed assertions" -- that is, the process was
#     terminated rather than exiting non-zero because some REQUIRE failed.
add_test(NAME qp_test_diag.death.contract_violation
    COMMAND ${CMAKE_COMMAND} -E env QP_TEST_SELF=$<TARGET_FILE:qp_test_diag>
            $<TARGET_FILE:qp_test_diag> "diag.contract.violation_terminates")
set_tests_properties(qp_test_diag.death.contract_violation PROPERTIES
    LABELS "unit;death"
    PASS_REGULAR_EXPRESSION "contract violation")

# -- Layer dependency gate (enforcement.md §4) ------------------------------
if(Python3_Interpreter_FOUND)
    add_test(NAME gate.layers
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_layers.py --quiet)
    set_tests_properties(gate.layers PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME meta.layer_checker
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_layer_checker.py)
    set_tests_properties(meta.layer_checker PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# -- Code dialect gate (code-dialect.md §6) ---------------------------------
# D1 pictographs / D2 source comments must be ASCII English / D3 no non-ASCII graphics in comments.
#
# The baseline is zero (127 files, 0 violations), so this is registered as a mandatory gate.
# Before that it was registered as the meta self-check only, on purpose: an always-red gate gets
# ignored, and "no gate" and "a red gate" are two different failures that must not be confused.
if(Python3_Interpreter_FOUND)
    add_test(NAME gate.dialect
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_dialect.py --quiet)
    set_tests_properties(gate.dialect PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME meta.dialect_checker
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_dialect_gate.py)
    set_tests_properties(meta.dialect_checker PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# -- File encoding gate (line endings, UTF-8, .ps1 BOM) ---------------------
# The compiler checks none of these three, but each has really bitten this project: Chinese
# comments saved as GBK, CRLF polluting a golden comparison, and a BOM-less .ps1 decoded as ANSI by PowerShell 5.1.
if(Python3_Interpreter_FOUND)
    add_test(NAME gate.encoding
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_encoding.py --quiet)
    set_tests_properties(gate.encoding PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME meta.encoding_checker
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_encoding_gate.py)
    set_tests_properties(meta.encoding_checker PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# -- Qt licence gate (THIRD_PARTY_NOTICES.md section 1) ----------------------
# Reads the generated link lines and refuses any Qt module that is not
# LGPL-compatible. Checking linkage rather than source is what makes it catch a
# transitive pull, which a #include scan cannot see.
#
# Registered unconditionally: with no Qt build tree it passes and says so, which
# is the configuration the project promises keeps working (core builds and tests
# with Qt absent entirely).
if(Python3_Interpreter_FOUND)
    add_test(NAME gate.qt_licence
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_qt_modules.py --quiet)
    set_tests_properties(gate.qt_licence PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    add_test(NAME meta.qt_licence_checker
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_qt_licence_gate.py)
    set_tests_properties(meta.qt_licence_checker PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# -- Tests of the gates themselves (enforcement.md §8) ----------------------
if(Python3_Interpreter_FOUND)
    add_test(NAME meta.contract_checker
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_contract_checker.py)
    set_tests_properties(meta.contract_checker PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# -- ABI layout assertions (enforcement.md §7) -------------------------------
# Must also link units: this guards the consistency of abi::FieldDim and units::Dim.
qp_add_test(qp_test_abi_layout
    SOURCES abi/test_abi_layout.cpp
    LIBS qp::abi qp::units
    LABELS "abi;unit")
qp_add_test(qp_test_ports
    SOURCES unit/ports/test_ports.cpp
    LIBS qp::ports)
qp_add_test(qp_test_graph_ir
    SOURCES unit/graph/test_ir.cpp
    LIBS qp::graph_ir)
qp_add_test(qp_test_graph_structure
    SOURCES unit/graph/test_structure.cpp
    LIBS qp::graph_structure)
qp_add_test(qp_test_graph_mutate
    SOURCES unit/graph/test_mutate.cpp
    LIBS qp::graph_mutate)
qp_add_test(qp_test_graph_validate
    SOURCES unit/graph/test_validate.cpp
    LIBS qp::graph_validate)
qp_add_test(qp_test_graph_eval
    SOURCES unit/graph/test_eval.cpp
    LIBS qp::graph_eval)
qp_add_test(qp_test_graph_domain
    SOURCES unit/graph/test_domain.cpp
    LIBS qp::graph_domain)
qp_add_test(qp_test_graph_field
    SOURCES unit/graph/test_field.cpp
    LIBS qp::field)
qp_add_test(qp_test_graph_kernels
    SOURCES unit/graph/test_kernels.cpp
    LIBS qp::graph_kernels)
qp_add_test(qp_test_plugin
    SOURCES unit/plugin/test_plugin.cpp
    LIBS qp::plugin)
qp_add_test(qp_test_capability
    SOURCES unit/authoring/test_capability.cpp
    LIBS qp::authoring_capability)
qp_add_test(qp_test_authoring_session
    SOURCES unit/authoring/test_session.cpp
    LIBS qp::authoring_commands)
qp_add_test(qp_test_authoring_document
    SOURCES unit/authoring/test_document.cpp
    LIBS qp::authoring_document)
qp_add_test(qp_test_authoring_layout
    SOURCES unit/authoring/test_layout.cpp
    LIBS qp::authoring_layout)
qp_add_test(qp_test_authoring_portui
    SOURCES unit/authoring/test_port_ui.cpp
    LIBS qp::authoring_portui)
qp_add_test(qp_test_runtime_run
    SOURCES unit/runtime/test_run.cpp
    LIBS qp::runtime_run)
qp_add_test(qp_test_runtime_store
    SOURCES unit/runtime/test_store.cpp
    LIBS qp::runtime_store)

# The Qt-FREE half of the view layer. It is listed here, in the ordinary suite,
# precisely because it does not need Qt: that is what lets the properties panel's
# editor-selection rule be asserted on both compilers, including GCC, which cannot
# link Qt at all.
qp_add_test(qp_test_views_model
    SOURCES unit/views/test_views_model.cpp
    LIBS qp::views_model)

# The Qt half of the view layer is tested only when Qt is in the build. Guarded
# rather than skipped: a test that silently does nothing would look like coverage,
# and the Qt-free half above is what carries the coverage on every configuration.
#
# This target is **not** built through qp_add_test, and the reason is a real trap:
# `catch_discover_tests` runs the executable at **build** time to enumerate its
# cases. This executable needs Qt's DLLs on PATH and a QApplication to start, so
# discovery fails -- and on MSVC it fails as a modal "Qt6Widgetsd.dll not found"
# dialog, which blocks the build until someone clicks it. Automation hangs, and the
# message names a DLL rather than the mechanism.
#
# So: a plain add_executable plus one CTest entry, with the DLL directory injected
# through the test's own environment. The granularity lost is real -- a failure
# names the executable, not the case -- and it is accepted because the alternative
# is a build step that depends on a GUI runtime being locatable.
#
# The widget headers are included by name from the test, so views/qt has to be on
# the include path. They are not installed headers: they are the view layer's
# internals, and only this test reaches into them.
if(QP_BUILD_VIEWS)
    add_executable(qp_test_views_qt unit/views/test_views_qt.cpp)
    target_include_directories(qp_test_views_qt PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/views/qt)
    target_link_libraries(qp_test_views_qt PRIVATE qp::views Catch2::Catch2)

    add_test(NAME qp_test_views_qt.all COMMAND qp_test_views_qt)
    set_tests_properties(qp_test_views_qt.all PROPERTIES LABELS "views;qt;aggregate")

    # Taken from the Qt target rather than hard-coded, so it follows whichever kit
    # the configure step resolved.
    get_target_property(_qt_core_location Qt6::Core LOCATION)
    get_filename_component(_qt_dll_dir "${_qt_core_location}" DIRECTORY)
    set_tests_properties(qp_test_views_qt.all PROPERTIES
        ENVIRONMENT "PATH=${_qt_dll_dir};$ENV{PATH}")
endif()
