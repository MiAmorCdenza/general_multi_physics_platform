# 测试：八类测试的分类落在目录结构上（standards/test-taxonomy.md）
#
#   unit/       第 1 类 单元 + 第 3 类 性质（用 [property] tag 标记）
#   abi/        第 2 类 静态布局契约
#   golden/     第 4 类 黄金回归
#   determinism/ 第 5 类 确定性
#   contract/   第 8 类 契约合规套件
#   smoke/      core 独立性证明
#   meta/       门禁自身的测试
#
# 不使用 Catch2 的自动发现（Catch2Main 自行注册用例），因此每个可执行文件
# 只需向 CTest 注册一次。

# Catch2 的 extras 模块提供 catch_discover_tests
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

# ── Python 解释器（门禁脚本需要）────────────────────────────────────────────
find_package(Python3 COMPONENTS Interpreter QUIET)

# ── 契约完备性门禁（enforcement.md §5）──────────────────────────────────────
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

# ── 测试目标 ─────────────────────────────────────────────────────────────────
#
# 用 catch_discover_tests 而不是 add_test(COMMAND <exe>)：
#   注册到 CTest 的是**每个 TEST_CASE**，而不是整个可执行文件。
#   收益：CI 日志直接点名失败用例；可按 [property] / [golden] 等 tag 过滤；
#         失败粒度与契约 @tests 的粒度对齐（契约里写的就是 TEST_CASE id）。
#
# 每个可执行文件额外保留一个 <target>.all 聚合测试，便于本地快速全跑。
function(qp_add_test target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;LABELS" ${ARGN})
    if(NOT ARG_LABELS)
        set(ARG_LABELS "unit")
    endif()

    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE qp::units Catch2::Catch2WithMain ${ARG_LIBS})

    catch_discover_tests(${target}
        TEST_PREFIX "${target}."
        PROPERTIES LABELS "${ARG_LABELS}"
        ADD_TAGS_AS_LABELS)          # [golden] / [property] 等 tag 直接成为 CTest 标签

    # 聚合测试：一次跑完该可执行文件的全部用例
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

# ── 死亡测试：验证"违反 @pre 必然终止进程" ─────────────────────────────────
#
# 被测用例带 `[.]` 标签（Catch2 hidden），因此 catch_discover_tests 不会
# 为它注册普通测试项——这一点很重要：否则它会以 SKIP 通过，
# 掩盖"死亡测试根本没跑"这个事实。
#
# 这里的判据是**进程退出码**：
#   - 用 PASS_REGULAR_EXPRESSION 要求终止原因确实是契约违反，
#     否则任意崩溃都会让这个测试"通过"。
#   - 同时要求 Catch2 报出"没有失败断言"——即进程是被 terminate 掉的，
#     而不是某个 REQUIRE 失败导致的非零退出。
add_test(NAME qp_test_diag.death.contract_violation
    COMMAND ${CMAKE_COMMAND} -E env QP_TEST_SELF=$<TARGET_FILE:qp_test_diag>
            $<TARGET_FILE:qp_test_diag> "diag.contract.violation_terminates")
set_tests_properties(qp_test_diag.death.contract_violation PROPERTIES
    LABELS "unit;death"
    PASS_REGULAR_EXPRESSION "契约违反")

# ── 层级依赖门禁（enforcement.md §4）───────────────────────────────────────
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

# ── 门禁自身的测试（enforcement.md §8）─────────────────────────────────────
if(Python3_Interpreter_FOUND)
    add_test(NAME meta.contract_checker
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_contract_checker.py)
    set_tests_properties(meta.contract_checker PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ── ABI 布局断言（enforcement.md §7）────────────────────────────────────────
# 需要同时链接 units：用于守住 abi::FieldDim 与 units::Dim 的一致性。
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