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
    # below, and `plugins` to the third. Test ownership is a global judgement: the
    # gate checks both that every @tests id exists and that every TEST_CASE is
    # claimed, so the invocations must see **disjoint** sets of test cases or each
    # reports the other's legitimate cases as orphans.
    add_test(NAME gate.contracts
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_contracts.py
                --headers ${CMAKE_SOURCE_DIR}/core
                --tests   ${CMAKE_SOURCE_DIR}/tests
                --exclude unit/views
                --exclude model
                --exclude plugins
                --quiet)
    set_tests_properties(gate.contracts PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The complement of the gate above. This invocation owns exactly one subtree of the
    # tests, stated positively.
    #
    # It used to enumerate what to skip: unit/views, unit/graph, unit/diag, unit/ports,
    # unit/runtime, unit/plugin, unit/authoring, unit/units, golden, glue, abi, meta,
    # smoke, support. Adding core/reflect then broke this gate, and broke it in the
    # worst available way: the new directory was not in the list, so its test ids were
    # treated as ids this gate owns, no contract under views/model referenced them, and
    # the gate reported orphans naming reflect -- a module it has nothing to do with.
    # The failure points at the wrong module, so the natural conclusion is that reflect
    # is broken. A gate whose maintenance is "remember to edit me whenever anything is
    # added" gets edited wrongly, and one that fails for the wrong reason gets deleted.
    #
    # `--only model` inverts that: the subject is stated, so a module added tomorrow is
    # out of scope until someone writes it in. No other directory in this tree contains
    # `model` as a path segment, and if one ever does, this gate acquires it visibly --
    # which is the correct failure, not a silent one.
    #
    # The Qt-side views files are excluded because their ids live in headers this gate
    # cannot see -- those need Qt to compile.
    add_test(NAME gate.contracts.views
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_contracts.py
                --headers ${CMAKE_SOURCE_DIR}/views/model
                --tests   ${CMAKE_SOURCE_DIR}/tests
                --only    model
                --quiet)
    set_tests_properties(gate.contracts.views PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # The Qt half of the view layer, as a fourth partition.
    #
    # Registered **unconditionally**, like the plugin gate and unlike the Qt test target: a
    # header that exists in the source tree but is not compiled in this configuration is exactly
    # the one whose contract drifts, and checking its comments costs nothing while compiling it
    # costs a toolkit. Without this, the measurement panel's `@tests` ids are claimed by nobody
    # and the `gaps_are_shown_verbatim` case could be renamed or deleted with the gate still
    # green.
    #
    # `--only unit/views` is what makes it a partition rather than an overlap: test ownership is
    # judged globally, so this invocation must see exactly the Qt-side cases and the other three
    # must not.
    if(EXISTS ${CMAKE_SOURCE_DIR}/views/qt)
        add_test(NAME gate.contracts.views_qt
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/scripts/check_contracts.py
                    --headers ${CMAKE_SOURCE_DIR}/views/qt
                    --headers ${CMAKE_SOURCE_DIR}/views/include
                    --tests   ${CMAKE_SOURCE_DIR}/tests
                    --only    unit/views
                    --quiet)
        set_tests_properties(gate.contracts.views_qt PROPERTIES
            LABELS "gate"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()

    # The third partition: plugin content.
    #
    # Plugins carry contracts like everything else, and for the same reason -- their
    # headers are the documentation a future author reads before writing a fourth
    # integrator -- so they need a gate that scans their headers and sees only their
    # test cases.
    #
    # This one is **unconditional**, unlike the test target it complements, which is
    # built only with QP_BUILD_PLUGINS. The asymmetry is deliberate: a header that
    # exists in the source tree but is never compiled is exactly the one that rots,
    # and checking its contract comments costs nothing while compiling it costs a
    # toolchain. The alternative -- skipping the gate when the plugin is off -- would
    # mean the gate's coverage depends on a configure flag, so a contract could be
    # broken on the machine of whoever has that flag off and discovered by whoever
    # turns it on.
    if(EXISTS ${CMAKE_SOURCE_DIR}/plugins/mechanics)
        add_test(NAME gate.contracts.plugins
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/scripts/check_contracts.py
                    --headers ${CMAKE_SOURCE_DIR}/plugins
                    --tests   ${CMAKE_SOURCE_DIR}/tests
                    --exclude unit/graph
                    --only    plugins
                    --quiet)
        set_tests_properties(gate.contracts.plugins PROPERTIES
            LABELS "gate"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()

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

# -- Core standalone: views/ and plugins/ must really be deletable -----------
#
# `docs/plan-tree.md` calls this the core assertion of the whole layering scheme, and
# the layer gate only enforces the include half of it. This check configures a copy of
# the tree with both content directories removed, which is the consumer's situation.
#
# It found a real violation the first time it ran: `add_subdirectory(views/model)` was
# unconditional, so deleting views/ broke configuration with a message naming a path
# the consumer had deliberately not taken.
if(Python3_Interpreter_FOUND)
    add_test(NAME meta.core_standalone
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_core_standalone.py)
    set_tests_properties(meta.core_standalone PROPERTIES
        LABELS "gate"
        # A configure of a copied tree, so it is seconds rather than milliseconds.
        TIMEOUT 600
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
qp_add_test(qp_test_reflect
    SOURCES unit/reflect/test_reflect.cpp
    LIBS qp::reflect)
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
# The binding half: which node becomes which operator, and what is refused.
#
# The test lives under `unit/plugins/` rather than `unit/graph/` because it owns contracts declared in
# **two** modules -- the loop in `core/graph/execution` and the binder in `plugins/mechanics` -- and the
# contract gate attributes cases per invocation. A case under `unit/graph/` is invisible to the plugins
# gate, so the plugin headers' `@tests` fields were reported as referencing cases that do not exist.
# The file also genuinely needs the plugin: every case builds a spring-damper node and binds it.
if(QP_BUILD_PLUGINS)
    qp_add_test(qp_test_plugins_execution
        SOURCES unit/plugins/test_execution_binding.cpp
        LIBS qp::graph_execution qp::plugins::mechanics qp::graph_kernels)

    # The JSON document format, in the plugin partition because it owns a contract
    # declared in a plugin header. Every case builds a real document and writes it, so
    # there is nothing here a stub could stand in for.
    qp_add_test(qp_test_plugins_qpjson
        SOURCES unit/plugins/test_qpjson_format.cpp
        LIBS qp::plugins::qpjson qp::authoring_persist qp::authoring_document qp::graph_structure)

    # The first real export format. It writes files, so its cases use a temporary directory
    # that removes itself -- and one of them asserts the refusal `ExportRefusal` was missing
    # until a format existed that could fail to write.
    qp_add_test(qp_test_plugins_csv
        SOURCES unit/plugins/test_csv_exporter.cpp
        LIBS qp::plugins::csv qp::runtime_io qp::runtime_trace qp::runtime_store qp::runtime_run
             qp::units)
endif()

# The run loop. Qt-free and graph-only: every case drives a stub operator, so it belongs to the loop's
# module rather than to a plugin. The binding cases -- which node becomes which operator -- live in
# tests/unit/plugins/, because those genuinely need the plugin.
qp_add_test(qp_test_graph_execution
    SOURCES unit/graph/test_execution.cpp
    LIBS qp::graph_execution)

qp_add_test(qp_test_graph_kernels
    SOURCES unit/graph/test_kernels.cpp
            unit/graph/test_kernel_clamp.cpp
    LIBS qp::graph_kernels)
qp_add_test(qp_test_plugin
    SOURCES unit/plugin/test_plugin.cpp
    LIBS qp::plugin)

# -- Plugin loader fixtures ---------------------------------------------------
#
# The only tests in this repository that map real shared libraries into the test
# process, because the loader's subject is the part that cannot be tested from a
# value: whether two separately-built binaries agree on the ABI they both think
# they have.
#
# One target per fixture rather than one library with several entry points. A
# library exports exactly one `qp_plugin_entry`, so several fixtures in one binary
# would collide -- and each fixture has to be malformed in exactly one way for a
# failure to be attributable.
#
# MODULE, not SHARED: no import library is generated, which is what a plugin is.
# Nothing links against these; they are only ever opened by name at runtime.
#
# They deliberately do **not** link qp::plugin. A fixture that linked the host's
# library and used its `Manifest` would agree with the host by construction, so the
# tag, version, and layout checks it exists to exercise could never fire. They
# include only the hand-written layout in tests/support/plugin_fixture_abi.hpp.
set(QP_PLUGIN_FIXTURES
    good
    second
    no_entry
    declines
    bad_tag
    bad_version
    no_manifest
    no_unregister
    rejects
    counting)

set(QP_PLUGIN_FIXTURE_TARGETS "")
foreach(fixture IN LISTS QP_PLUGIN_FIXTURES)
    set(target "qp_fixture_plugin_${fixture}")
    add_library(${target} MODULE fixtures/plugin/${fixture}.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Position-independent regardless of platform, because on Linux a MODULE library
    # that is not PIC fails to link -- and the failure names the linker, not the
    # missing flag.
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
    list(APPEND QP_PLUGIN_FIXTURE_TARGETS ${target})
endforeach()

# The loader's entries are renamed so the symbol the host looks for is unambiguous:
# an unreferenced `qp_plugin_declines_entry` would never be looked up by name, and
# the mapping from "which fixture" to "which entry point" would be invisible.
#
# `declines` and `no_entry` are handled by their own source files, which export the
# real name and a misspelled one respectively -- that pair is the whole distinction
# between "the plugin refused" and "the build is wrong", so neither can be renamed.

add_executable(qp_test_plugin_loader unit/plugin/test_loader.cpp ${QP_TEST_MAIN})
target_include_directories(qp_test_plugin_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qp_test_plugin_loader PRIVATE qp::plugin Catch2::Catch2)
add_dependencies(qp_test_plugin_loader ${QP_PLUGIN_FIXTURE_TARGETS})

# The fixture paths are handed to the test as **generator expressions naming the real
# target files**, one per fixture, rather than as a directory the test appends names
# to.
#
# The first version did the latter, and it failed on the first run: MinGW prefixes a
# MODULE library with `lib` (`libqp_fixture_plugin_good.dll`), MSVC does not, and
# neither matches the name a reader would predict. Every test that loaded a fixture
# then reported `file_missing`, which looks like a fixture that was not built. A test
# that reconstructs a toolchain's output name is testing its own guess.
#
# `file(TO_CMAKE_PATH)` is not cosmetic either: on Windows these expand with
# backslashes, and embedding that in a C++ string literal turns each separator into
# an escape sequence -- `build\test` contains `\t`. Forward slashes work in every
# Windows API used here.
set(QP_PLUGIN_FIXTURE_DEFS "")
foreach(fixture IN LISTS QP_PLUGIN_FIXTURES)
    file(TO_CMAKE_PATH "$<TARGET_FILE:qp_fixture_plugin_${fixture}>" _path)
    list(APPEND QP_PLUGIN_FIXTURE_DEFS
        "QP_FIXTURE_${fixture}=\"${_path}\"")
endforeach()

target_compile_definitions(qp_test_plugin_loader PRIVATE ${QP_PLUGIN_FIXTURE_DEFS})

catch_discover_tests(qp_test_plugin_loader
    TEST_PREFIX "qp_test_plugin_loader."
    PROPERTIES LABELS "unit;loader"
    ADD_TAGS_AS_LABELS)
add_test(NAME "qp_test_plugin_loader.all" COMMAND qp_test_plugin_loader)
set_tests_properties("qp_test_plugin_loader.all" PROPERTIES LABELS "unit;loader;aggregate")

# -- Plugin content -----------------------------------------------------------
#
# Built only with QP_BUILD_PLUGINS, so core/ configures and its whole suite passes
# with plugins/ deleted. That is the promise the layer gate enforces structurally
# and this conditional enforces at configure time.
if(QP_BUILD_PLUGINS)
    qp_add_test(qp_test_plugins_mechanics
        SOURCES unit/plugins/test_rk4_oscillator.cpp
                unit/plugins/test_incline_collision.cpp
        LIBS qp::plugins::mechanics)

    # Charter C7: fixed inputs, bit-for-bit comparison. Separate from the cases above because it
    # answers a different question -- those assert the physics against closed forms, this asserts
    # that the arithmetic has not moved by one bit. Labelled `golden` so it can be selected as a
    # group, as the units golden test is.
    qp_add_test(qp_test_plugins_golden
        SOURCES golden/plugins/test_plugin_golden.cpp
        LIBS qp::plugins::mechanics
        LABELS "golden")
endif()

qp_add_test(qp_test_capability
    SOURCES unit/authoring/test_capability.cpp
    LIBS qp::authoring_capability)
qp_add_test(qp_test_authoring_session
    SOURCES unit/authoring/test_session.cpp
    LIBS qp::authoring_commands)
qp_add_test(qp_test_authoring_document
    SOURCES unit/authoring/test_document.cpp
    LIBS qp::authoring_document)
# The persistence contract. A core target, and it stays one: the *interface* is what is
# tested here, with a stub format, so the suite that has to pass with `plugins/` absent
# still covers it. The concrete JSON format is tested where it lives, under
# `unit/plugins/`, because that test genuinely needs the plugin.
#
# The split is also what the contract gate needs. Test ownership is judged per
# invocation, and the ids in `core/authoring/persist/...` are read by the core
# invocation -- so their cases have to be visible to it, which a file under
# `unit/plugins/` is not.
qp_add_test(qp_test_authoring_persist
    SOURCES unit/authoring/test_persist.cpp
    LIBS qp::authoring_persist qp::graph_structure)
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
qp_add_test(qp_test_runtime_trace
    SOURCES unit/runtime/test_trace.cpp
    LIBS qp::runtime_trace)
qp_add_test(qp_test_runtime_io
    SOURCES unit/runtime/test_io.cpp
    LIBS qp::runtime_io)

# The Qt-FREE half of the view layer. It is listed here, in the ordinary suite,
# precisely because it does not need Qt: that is what lets the properties panel's
# editor-selection rule be asserted on both compilers, including GCC, which cannot
# link Qt at all.
# tests/model/, not tests/unit/views/: the contract gate needs a test root holding
# exactly these cases. It judges orphan-freedom globally, so two invocations must
# see disjoint sets of test cases or each reports the other's legitimate cases as
# orphans -- and the Qt-side file in tests/unit/views/ is one the gate cannot see at
# all, because it needs a toolkit to compile.
#
# Guarded on the **target**, not on a variable. `views/` is content and may be deleted
# from a consumer's checkout entirely, so this file cannot assume the target exists:
# CMake's `if(TARGET ...)` is the honest way to ask, and it stays correct whether the
# directory is missing, the option is off, or the two disagree.
#
# Two targets, not one, and the second condition is the reason. `qp::views_model` does
# **not** depend on the plugin -- the view layer was deliberately inverted so it could be
# built without one -- but three of the four files below include plugin headers, because
# a confidence panel needs a real integrator to produce a trace worth judging and the Run
# controller needs a real binder. So with `QP_BUILD_VIEWS=ON` and `QP_BUILD_PLUGINS=OFF`
# the link line names a target that does not exist and configure fails outright. Asking
# for both targets converts that into "these tests need a plugin", which is the truth.
if(TARGET qp::views_model AND TARGET qp::plugins::mechanics)
    qp_add_test(qp_test_views_model
        SOURCES model/test_views_model.cpp
                model/test_measurement_model.cpp
                model/test_confidence_model.cpp
        LIBS qp::views_model qp::runtime_io qp::runtime_store qp::runtime_trace qp::runtime_run qp::plugins::mechanics)

    # The window's Run action, in this partition rather than the plugin one, because its
    # cases are claimed by `views/model/run_controller.hpp`. Test ownership is judged per
    # invocation, so the file's **path** decides which gate owns it: `--only model` sees
    # this subtree and the plugin invocation does not, and a case filed under
    # `unit/plugins/` had its contracts in a partition that could not see it -- which the
    # gate reported, correctly, as both orphans and dangling `@tests` ids.
    qp_add_test(qp_test_run_controller
        SOURCES model/test_run_controller.cpp
        LIBS qp::views_model qp::plugins::mechanics qp::graph_execution qp::authoring_commands
             qp::graph_mutate)
endif()

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
