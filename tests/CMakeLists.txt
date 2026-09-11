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
function(qp_add_test target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE qp::units Catch2::Catch2WithMain ${ARG_LIBS})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "unit")
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
qp_add_test(qp_test_units_symbol_golden
    SOURCES golden/units/test_unit_symbol.cpp)
qp_add_test(qp_test_core_standalone
    SOURCES smoke/test_core_standalone.cpp)

set_tests_properties(qp_test_units_symbol_golden PROPERTIES LABELS "golden")

# ── 门禁自身的测试（enforcement.md §8）─────────────────────────────────────
if(Python3_Interpreter_FOUND)
    add_test(NAME meta.contract_checker
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/meta/check_contract_checker.py)
    set_tests_properties(meta.contract_checker PROPERTIES
        LABELS "gate"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
