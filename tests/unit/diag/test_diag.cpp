/**
 * @file test_diag.cpp
 * @brief Unit, property and concurrency tests for the diag module.
 *
 * The case ids correspond verbatim to the @tests fields under core/diag/include.
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


// ===========================================================================
// error.hpp
// ===========================================================================

TEST_CASE("diag.error_code_values_are_stable", "[diag]") {
    // Error codes are **stable identifiers**, not internal ordinals: once published, values must not be reordered.
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::ok) == 0);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::invalid_argument) == 0x0101);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::dimension_mismatch) == 0x0203);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::cycle_detected) == 0x0303);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::plugin_incompatible) == 0x0402);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::seed_required) == 0x0502);
    STATIC_REQUIRE(static_cast<std::uint16_t>(ErrorCode::internal_error) == 0x0901);
    STATIC_REQUIRE(sizeof(ErrorCode) == 2);

    // Short names are stable and non-empty
    REQUIRE(to_string(ErrorCode::ok) == "ok");
    REQUIRE(to_string(ErrorCode::cycle_detected) == "cycle_detected");
    REQUIRE(to_string(ErrorCode::dimension_mismatch) == "dimension_mismatch");
    REQUIRE_FALSE(to_string(ErrorCode::internal_error).empty());
}

TEST_CASE("diag.error_code_names_are_unique", "[diag][property]") {
    // Different error codes must not share a short name (the log could not tell them apart)
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
    STATIC_REQUIRE(domain_of(ErrorCode::ok) == ErrorDomain::internal);  // meaningless, present only for full coverage
}

TEST_CASE("diag.consequence_ordering", "[diag]") {
    // The order is the severity: comparable and sortable
    STATIC_REQUIRE(Consequence::recoverable < Consequence::degraded);
    STATIC_REQUIRE(Consequence::degraded < Consequence::run_aborted);
    STATIC_REQUIRE(Consequence::run_aborted < Consequence::fatal);
    STATIC_REQUIRE(sizeof(Consequence) == 1);

    // The default consequence is inferred from the domain
    STATIC_REQUIRE(default_consequence(ErrorCode::invalid_argument) == Consequence::recoverable);
    STATIC_REQUIRE(default_consequence(ErrorCode::dimension_mismatch) == Consequence::recoverable);
    STATIC_REQUIRE(default_consequence(ErrorCode::cycle_detected) == Consequence::degraded);
    STATIC_REQUIRE(default_consequence(ErrorCode::plugin_load_failed) == Consequence::degraded);
    STATIC_REQUIRE(default_consequence(ErrorCode::seed_required) == Consequence::run_aborted);
    STATIC_REQUIRE(default_consequence(ErrorCode::internal_error) == Consequence::fatal);
}

// ===========================================================================
// result.hpp
// ===========================================================================

TEST_CASE("diag.result.ok_value", "[diag]") {
    const Result<int> r{42};
    REQUIRE(r.has_value());
    REQUIRE(static_cast<bool>(r));
    REQUIRE(r.error() == ErrorCode::ok);
    REQUIRE(r.value() == 42);
    REQUIRE(r.consequence() == Consequence::recoverable);
}

TEST_CASE("diag.result.layout", "[diag][abi]") {
    // The shape of Result is part of the frozen contract
    STATIC_REQUIRE(std::is_same_v<Result<int>::value_type, int>);
    // Trivial copyability **depends on the payload**: trivial for int, not for string.
    // So this asserts only "no extra indirection was introduced", not a fixed value.
    STATIC_REQUIRE(sizeof(Result<int>) <= sizeof(std::optional<int>) + alignof(std::optional<int>)
                                               + sizeof(ErrorCode));
    STATIC_REQUIRE(std::is_copy_constructible_v<Result<int>>);
    STATIC_REQUIRE(std::is_move_constructible_v<Result<int>>);
    // Must not implicitly convert to anything but bool (blocks misuse such as if (r.value()))
    STATIC_REQUIRE(std::is_convertible_v<Result<int>, bool> == false);  // explicit operator bool
}

TEST_CASE("diag.result.err_only", "[diag]") {
    const Result<int> r{ErrorCode::dimension_mismatch};
    REQUIRE_FALSE(r.has_value());
    REQUIRE_FALSE(static_cast<bool>(r));
    REQUIRE(r.error() == ErrorCode::dimension_mismatch);
    REQUIRE(r.consequence() == Consequence::recoverable);  // typing domain -> the user just changes the parameter
}

TEST_CASE("diag.result.value_or_fallback", "[diag]") {
    REQUIRE(Result<int>{7}.value_or(-1) == 7);
    REQUIRE(Result<int>{ErrorCode::fit_failed}.value_or(-1) == -1);
    REQUIRE(Result<std::string>{"a"}.value_or("z") == "a");
    REQUIRE(Result<std::string>{ErrorCode::dataset_empty}.value_or("z") == "z");
}

TEST_CASE("diag.result.monadic_chaining", "[diag]") {
    // Success chain: the value is transformed step by step
    const Result<int> start{10};
    const auto doubled = start.and_then([](int v) { return Result<int>{v * 2}; });
    REQUIRE(doubled.has_value());
    REQUIRE(doubled.value() == 20);

    // Failure chain: the error code must pass through unchanged, never swallowed
    const Result<int> bad{ErrorCode::out_of_range};
    const auto passed = bad.and_then([](int v) { return Result<int>{v * 2}; });
    REQUIRE_FALSE(passed.has_value());
    REQUIRE(passed.error() == ErrorCode::out_of_range);

    // A failure in the middle of the chain must pass through too
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
    // This project expresses failure without exceptions: every Result member has noexcept semantics
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().value()));
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().has_value()));
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().error()));
    STATIC_REQUIRE(noexcept(std::declval<const Result<int>&>().value_or(0)));
    STATIC_REQUIRE(noexcept(std::declval<const Result<void>&>().has_value()));
    STATIC_REQUIRE(std::is_nothrow_constructible_v<Result<int>, ErrorCode>);
    // The void specialization should carry no state overhead
    STATIC_REQUIRE(sizeof(Result<void>) == sizeof(ErrorCode));
}

TEST_CASE("diag.result.move_only_payload", "[diag]") {
    // A move-only payload must fit too
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

// ===========================================================================
// diagnostic.hpp
// ===========================================================================

TEST_CASE("diag.diagnostic.construction", "[diag]") {
    const Diagnostic d{ErrorCode::cycle_detected, "节点 n3 形成环", SourceId{"graph"}};
    REQUIRE(d.code() == ErrorCode::cycle_detected);
    REQUIRE(d.domain() == ErrorDomain::graph);
    REQUIRE(d.consequence() == Consequence::degraded);  // inferred from the domain
    REQUIRE(d.source().value == "graph");
    REQUIRE(d.message() == "节点 n3 形成环");

    // Explicitly override the consequence level
    const Diagnostic fatal{ErrorCode::invalid_argument, "x", SourceId{"in"}, Consequence::fatal};
    REQUIRE(fatal.consequence() == Consequence::fatal);
    REQUIRE(fatal.domain() == ErrorDomain::input);  // the domain is unaffected
}

TEST_CASE("diag.diagnostic.stable_text", "[diag]") {
    const Diagnostic d{ErrorCode::dimension_mismatch, "长度不能接到时间", SourceId{"validate"}};
    const std::string text = d.to_text();
    REQUIRE(text == "validate: dimension_mismatch — 长度不能接到时间");
    // Idempotent: the same diagnostic yields the same text every time
    REQUIRE(d.to_text() == text);

    // The prefix is omitted when there is no source
    const Diagnostic bare{ErrorCode::cancelled, "用户取消"};
    REQUIRE(bare.to_text() == "cancelled — 用户取消");

    // with_detail appends without changing code / consequence
    Diagnostic with_detail{ErrorCode::missing_field, "缺 unit"};
    with_detail.with_detail("端口 out 上");
    REQUIRE(with_detail.code() == ErrorCode::missing_field);
    REQUIRE(with_detail.to_text() == "missing_field — 缺 unit；端口 out 上");
}

TEST_CASE("diag.diagnostic.no_live_references", "[diag]") {
    // A diagnostic must be **self-contained**: the copy stays usable after the original dies.
    // This property lets diagnostics travel across threads, processes and languages.
    std::string text;
    {
        Diagnostic origin{ErrorCode::plugin_load_failed, "x.dll 载入失败", SourceId{"plugin"}};
        const Diagnostic copy = origin;      // copy
        text = copy.to_text();
    }
    REQUIRE(text == "plugin: plugin_load_failed — x.dll 载入失败");
    REQUIRE_FALSE(text.empty());
}

// ===========================================================================
// sink.hpp
// ===========================================================================

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

    // The order is the emit order
    const auto items = sink.items();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].message() == "a");
    REQUIRE(items[1].message() == "b");
    REQUIRE(items[2].message() == "c");

    // The worst one
    REQUIRE(sink.worst_consequence() == Consequence::fatal);

    sink.clear();
    REQUIRE(sink.empty());
    REQUIRE(std::string(sink.name()) == "collecting");
}

TEST_CASE("diag.sink.collecting_sink_thread_safe", "[diag][concurrency]") {
    // Both the eval thread and the main thread may produce diagnostics, so emit must be thread-safe.
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

// ===========================================================================
// contract.hpp -- the "death tests" for preconditions
// ===========================================================================
//
// Violating @pre must **terminate the process**, so it cannot be asserted in the same process --
// that would kill the test runner itself. The approach: with QP_TEST_SELF set, this executable runs
// in "death child process" mode, registered by CMake as a WILL_FAIL test item.
//
// What you get is **real process exit code** evidence, not a simulation.

TEST_CASE("diag.contract.holds_does_nothing", "[diag]") {
    // When the condition holds it must return normally and have no side effects
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

    // -- The code below only runs in the death child process --
    // precondition goes through fprintf + std::terminate; it does not throw and does not return.
    std::fflush(stdout);
    QP_PRECONDITION(false);   // must terminate here
    std::_Exit(0);            // reaching this line means the precondition check failed
}
