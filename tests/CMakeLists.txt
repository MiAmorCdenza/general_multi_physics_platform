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
    add_test(NAME gate.contracts
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_contracts.py
                --headers ${CMAKE_SOURCE_DIR}/core
                --tests   ${CMAKE_SOURCE_DIR}/tests
                --quiet)
    set_tests_properties(gate.contracts PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
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
function(qp_add_test target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})
    if(NOT ARG_LABELS)
        set(ARG_LABELS "unit")
    endif()

    add_executable(${target} ${ARG_SOURCES} ${QP_TEST_MAIN})
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
