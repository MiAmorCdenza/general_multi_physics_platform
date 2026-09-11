/**
 * @file test_diag.cpp
 * @brief diag 模块的单元、性质与并发测试。
 *
 * 用例 id 与 core/diag/include/qp/diag/*.hpp 的 @tests 字段逐字对应。
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/diag.hpp>

#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace qp::diag;


// ═══════════════════════════════════════════════════════════════════════════
// error.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diag.error_code_values_are_stable", "[diag]") {
    // 错误码是**稳定标识**，不是内部序号：数值一旦发布不得重排。
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::ok) == 0);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::invalid_argument) == 0x0101);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::dimension_mismatch) == 0x0203);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::cycle_detected) == 0x0303);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::plugin_incompatible) == 0x0402);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::seed_required) == 0x0502);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::internal_error) == 0x0901);
    STATIC_REQUIRE(sizeof(ErrorCode) == 2);

    // 短名稳定且非空
    REQUIRE(to_string(ErrorCode::ok) == "ok");
    REQUIRE(to_string(ErrorCode::cycle_detected) == "cycle_detected");
    REQUIRE(to_string(ErrorCode::dimension_mismatch) == "dimension_mismatch");
    REQUIRE_FALSE(to_string(ErrorCode::internal_error).empty());
}

TEST_CASE("diag.error_code_names_are_unique", "[diag][property]") {
    // 不同错误码不得共用短名（否则日志无法区分）
    const ErrorCode all[] = {
        ErrorCode::ok, ErrorCode::invalid_argument, ErrorCode::malformed_document,
        ErrorCode::unsupported_version, ErrorCode::missing_field, ErrorCode::out_of_range,
        ErrorCode::unknown_port_type, ErrorCode::type_mismatch, ErrorCode::dimension_mismatch,
        ErrorCode::unit_mismatch, ErrorCode::unknown_node, ErrorCode::unknown_port,
        ErrorCode::cycle_detected, ErrorCode::duplicate_connection, ErrorCode::not_connected,
        ErrorCode::graph_busy, ErrorCode::plugin_not_found, ErrorCode::plugin_incompatible,
        ErrorCode::plugin_load_failed, ErrorCode::plugin_capability_missing,
        ErrorCode::run_not_found, ErrorCode::seed_required, ErrorCode::dataset_empty,
        ErrorCode::fit_failed, ErrorCode::internal_error, ErrorCode::not_implemented,
        ErrorCode::cancelled,
    };
    for (std::size_t i = 0; i < std::size(all); ++i) {
        REQUIRE_FALSE(to_string(all[i]).empty());
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            INFO("i=" << i << " j=" << j);
            REQUIRE(to_string(all[i]) != to_string(all[j]));
        }
    }
}

TEST_CASE("diag.error_domain_mapping", "[diag]") {
    STATIC_REQUIRE(domain_of(ErrorCode::invalid_argument) == ErrorDomain::input);
    STATIC_REQUIRE(domain_of(ErrorCode::malformed_document) == ErrorDomain::input);
    STATIC_REQUIRE(domain_of(ErrorCode::dimension_mismatch) == ErrorDomain::typing);
    STATIC_REQUIRE(domain_of(ErrorCode::unit_mismatch) == ErrorDomain::typing);
    STATIC_REQUIRE(domain_of(ErrorCode::cycle_detected) == ErrorDomain::graph);
    STATIC_REQUIRE(domain_of(ErrorCode::plugin_incompatible) == ErrorDomain::plugin);
    STATIC_REQUIRE(domain_of(ErrorCode::seed_required) == ErrorDomain::runtime);
    STATIC_REQUIRE(domain_of(ErrorCode::internal_error) == ErrorDomain::internal);
    STATIC_REQUIRE(domain_of(ErrorCode::ok) == ErrorDomain::internal);  // 无意义，仅保证全覆盖
}

TEST_CASE("diag.consequence_ordering", "[diag]") {
    // 顺序即严重程度：可比较、可排序
    STATIC_REQUIRE(Consequence::recoverable < Consequence::degraded);
    STATIC_REQUIRE(Consequence::degraded < Consequence::run_aborted);
    STATIC_REQUIRE(Consequence::run_aborted < Consequence::fatal);
    STATIC_REQUIRE(sizeof(Consequence) == 1);

    // 默认后果按域推断
    STATIC_REQUIRE(default_consequence(ErrorCode::invalid_argument) == Consequence::recoverable);
    STATIC_REQUIRE(default_consequence(ErrorCode::dimension_mismatch) == Consequence::recoverable);
    STATIC_REQUIRE(default_consequence(ErrorCode::cycle_detected) == Consequence::degraded);
    STATIC_REQUIRE(default_consequence(ErrorCode::plugin_load_failed) == Consequence::degraded);
    STATIC_REQUIRE(default_consequence(ErrorCode::seed_required) == Consequence::run_aborted);
    STATIC_REQUIRE(default_consequence(ErrorCode::internal_error) == Consequence::fatal);
}

// ═══════════════════════════════════════════════════════════════════════════
// result.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diag.result.ok_value", "[diag]") {
    const Result<int> r{42};
    REQUIRE(r.has_value());
    REQUIRE(static_cast<bool>(r));
    REQUIRE(r.error() == ErrorCode::ok);
    REQUIRE(r.value() == 42);
    REQUIRE(r.consequence() == Consequence::recoverable);
}

TEST_CASE("diag.result.layout", "[diag][abi]") {
    // Result 的形状是冻结契约的一部分
    STATIC_REQUIRE(std::is_same_v<Result<int>::value_type, int>);
    // 平凡可复制性**取决于载荷**：int 时可平凡复制，string 时不可。
    // 因此这里只断言"没有引入额外的间接层"，而不是断言某个固定取值。
    STATIC_REQUIRE(sizeof(Result<int>) <= sizeof(std::optional<int>) + alignof(std::optional<int>)
                                               + sizeof(ErrorCode));
    STATIC_REQUIRE(std::is_copy_constructible_v<Result<int>>);
    STATIC_REQUIRE(std::is_move_constructible_v<Result<int>>);
    // 不得隐式转成 bool 以外的类型（防止 if (r.value()) 之类的误用）
    STATIC_REQUIRE(std::is_convertible_v<Result<int>, bool> == false);  // explicit operator bool
}

TEST_CASE("diag.result.err_only", "[diag]") {
    const Result<int> r{ErrorCode::dimension_mismatch};
    REQUIRE_FALSE(r.has_value());
    REQUIRE_FALSE(static_cast<bool>(r));
    REQUIRE(r.error() == ErrorCode::dimension_mismatch);
    REQUIRE(r.consequence() == Consequence::recoverable);  // typing 域 → 用户改参数即可
}

TEST_CASE("diag.result.value_or_fallback", "[diag]") {
    REQUIRE(Result<int>{7}.value_or(-1) == 7);
    REQUIRE(Result<int>{ErrorCode::fit_failed}.value_or(-1) == -1);
    REQUIRE(Result<std::string>{"a"}.value_or("z") == "a");
    REQUIRE(Result<std::string>{ErrorCode::dataset_empty}.value_or("z") == "z");
}

TEST_CASE("diag.result.monadic_chaining", "[diag]") {
    // 成功链路：值被逐级变换
    const Result<int> start{10};
    const auto doubled = start.and_then([](int v) { return Result<int>{v * 2}; });
    REQUIRE(doubled.has_value());
    REQUIRE(doubled.value() == 20);

    // 失败链路：错误码必须被原样透传，不得被吞掉
    const Result<int> bad{ErrorCode::out_of_range};
    const auto passed = bad.and_then([](int v) { return Result<int>{v * 2}; });
    REQUIRE_FALSE(passed.has_value());
    REQUIRE(passed.error() == ErrorCode::out_of_range);

    // 链路中段失败也要透传
    const auto mid = start.and_then([](int) { return Result<int>{ErrorCode::fit_failed}; });
    REQUIRE_FALSE(mid.has_value());
    REQUIRE(mid.error() == ErrorCode::fit_failed);
}

TEST_CASE("diag.result.void_specialization", "[diag]") {
    const Result<void> good = ok();
    REQUIRE(good.has_value());
    REQUIRE(good.error() == ErrorCode::ok);
    STATIC_REQUIRE(std::is_same_v<Result<void>::value_type, void>);

    const Result<void> bad = fail(ErrorCode::cycle_detected);
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error() == ErrorCode::cycle_detected);
    REQUIRE(bad.consequence() == Consequence::degraded);
}

TEST_CASE("diag.result.void_ok_is_truthy", "[diag]") {
    STATIC_REQUIRE(static_cast<bool>(Result<void>{}) == true);
    STATIC_REQUIRE(static_cast<bool>(Result<void>{ErrorCode::cancelled}) == false);
}

TEST_CASE("diag.result.no_throw_on_access", "[diag]") {
    // 本项目的失败表达不用异常：Result 的所有成员都是 noexcept 语义
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().value()));
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().has_value()));
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().error()));
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().value_or(0)));
    STATIC_REQUIRE(noexcept(std::declval<const Result<void>&>().has_value()));
    STATIC_REQUIRE(std::is_nothrow_constructible_v<Result<int>, ErrorCode>);
    // void 特化应当无状态开销
    STATIC_REQUIRE(sizeof(Result<void>) == sizeof(ErrorCode));
}

TEST_CASE("diag.result.move_only_payload", "[diag]") {
    // 只可移动的载荷也要能放进来
    struct MoveOnly {
        int v = 0;
        explicit MoveOnly(int x) : v(x) {}
        MoveOnly(MoveOnly&&) = default;
        MoveOnly& operator=(MoveOnly&&) = default;
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
    };
    Result<MoveOnly> r{MoveOnly{5}};
    REQUIRE(r.has_value());
    REQUIRE(r.value().v == 5);
}

// ═══════════════════════════════════════════════════════════════════════════
// diagnostic.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diag.diagnostic.construction", "[diag]") {
    const Diagnostic d{ErrorCode::cycle_detected, "节点 n3 形成环", SourceId{"graph"}};
    REQUIRE(d.code() == ErrorCode::cycle_detected);
    REQUIRE(d.domain() == ErrorDomain::graph);
    REQUIRE(d.consequence() == Consequence::degraded);  // 从域推断
    REQUIRE(d.source().value == "graph");
    REQUIRE(d.message() == "节点 n3 形成环");

    // 显式覆盖后果级别
    const Diagnostic fatal{ErrorCode::invalid_argument, "x", SourceId{"in"}, Consequence::fatal};
    REQUIRE(fatal.consequence() == Consequence::fatal);
    REQUIRE(fatal.domain() == ErrorDomain::input);  // 域不受影响
}

TEST_CASE("diag.diagnostic.stable_text", "[diag]") {
    const Diagnostic d{ErrorCode::dimension_mismatch, "长度不能接到时间", SourceId{"validate"}};
    const std::string text = d.to_text();
    REQUIRE(text == "validate: dimension_mismatch — 长度不能接到时间");
    // 幂等：同一诊断每次给出同一文本
    REQUIRE(d.to_text() == text);

    // 无来源时省略前缀
    const Diagnostic bare{ErrorCode::cancelled, "用户取消"};
    REQUIRE(bare.to_text() == "cancelled — 用户取消");

    // with_detail 追加而不改 code / consequence
    Diagnostic with_detail{ErrorCode::missing_field, "缺 unit"};
    with_detail.with_detail("端口 out 上");
    REQUIRE(with_detail.code() == ErrorCode::missing_field);
    REQUIRE(with_detail.to_text() == "missing_field — 缺 unit；端口 out 上");
}

TEST_CASE("diag.diagnostic.no_live_references", "[diag]") {
    // 诊断必须**自持**：拷贝后原对象销毁，副本仍然可用。
    // 这条性质保证诊断能跨线程/跨进程/跨语言传递。
    std::string text;
    {
        Diagnostic origin{ErrorCode::plugin_load_failed, "x.dll 载入失败", SourceId{"plugin"}};
        const Diagnostic copy = origin;      // 拷贝
        text = copy.to_text();
    }
    REQUIRE(text == "plugin: plugin_load_failed — x.dll 载入失败");
    REQUIRE_FALSE(text.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// sink.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diag.sink.null_sink", "[diag]") {
    NullSink sink;
    sink.emit(Diagnostic{ErrorCode::internal_error, "被丢弃", SourceId{"t"}});
    REQUIRE(std::string(sink.name()) == "null");
}

TEST_CASE("diag.sink.collecting_sink", "[diag]") {
    CollectingSink sink;
    REQUIRE(sink.empty());
    REQUIRE(sink.size() == 0);

    sink.emit(Diagnostic{ErrorCode::invalid_argument, "a", SourceId{"t"}});
    sink.emit(Diagnostic{ErrorCode::cycle_detected, "b", SourceId{"t"}});
    sink.emit(Diagnostic{ErrorCode::internal_error, "c", SourceId{"t"}});

    REQUIRE(sink.size() == 3);
    REQUIRE_FALSE(sink.empty());
    REQUIRE(sink.contains(ErrorCode::cycle_detected));
    REQUIRE_FALSE(sink.contains(ErrorCode::fit_failed));

    // 顺序即 emit 顺序
    const auto items = sink.items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].message() == "a");
    REQUIRE(items[1].message() == "b");
    REQUIRE(items[2].message() == "c");

    // 最严重者
    REQUIRE(sink.worst_consequence() == Consequence::fatal);

    sink.clear();
    REQUIRE(sink.empty());
    REQUIRE(std::string(sink.name()) == "collecting");
}

TEST_CASE("diag.sink.collecting_sink_thread_safe", "[diag][concurrency]") {
    // 求值线程与主线程都可能产生诊断，emit 必须线程安全。
    CollectingSink sink;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&sink, t] {
            for (int i = 0; i < kPerThread; ++i) {
                sink.emit(Diagnostic{ErrorCode::graph_busy, "x", SourceId{"worker"}});
            }
            (void)t;
        });
    }
    for (auto& w : workers) w.join();
    REQUIRE(sink.size() == static_cast<std::size_t>(kThreads * kPerThread));
}

// ═══════════════════════════════════════════════════════════════════════════
// contract.hpp —— 前置条件的"死亡测试"
// ═══════════════════════════════════════════════════════════════════════════
//
// 违反 @pre 必须**终止进程**，因此不能在同一进程里断言——那会杀掉测试
// 运行器本身。做法：本可执行文件在设置 QP_TEST_SELF 后以"死亡子进程"
// 模式运行，由 CMake 注册成一个 WILL_FAIL 的测试项。
//
// 得到的是**真实的进程退出码**证据，而不是模拟。

TEST_CASE("diag.contract.holds_does_nothing", "[diag]") {
    // 条件成立时必须正常返回，且无副作用
    precondition(true, "true", __FILE__, __LINE__);
    precondition(1 + 1 == 2, "1+1==2", __FILE__, __LINE__);
    QP_PRECONDITION(true);
    QP_INVARIANT(true);
    SUCCEED("条件成立时不应终止");
}

TEST_CASE("diag.contract.violation_terminates", "[.][diag][death]") {
    const char* self = std::getenv("QP_TEST_SELF");
    if (self == nullptr || *self == '\0') {
        SKIP("QP_TEST_SELF 未设置：本用例需由 CMake 以死亡子进程方式运行");
    }

    // ── 以下代码只会跑在死亡子进程里 ──
    // precondition 内部走 fprintf + std::terminate，不抛异常、不返回。
    std::fflush(stdout);
    QP_PRECONDITION(false);   // 必须在此终止
    std::_Exit(0);            // 若真走到这里，说明前置条件检查失效了
}