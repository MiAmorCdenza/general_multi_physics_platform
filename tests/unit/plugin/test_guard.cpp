/**
 * @file test_guard.cpp
 * @brief Tests for the plugin fault barrier.
 *
 * ## What is worth testing here
 *
 * Charter C4 says a plugin crash must not take the host down, and this file is where that sentence either
 * becomes true or does not. The properties, and the one the file deliberately does **not** claim:
 *
 *   - a plugin that **raises** becomes a failed `Result` with a code that says "the plugin misbehaved",
 *     not an exception through the host's stack;
 *   - everything is caught, including things that are not `std::exception` -- a plugin may `throw 42`, and a
 *     barrier that only catches `std::exception` would let that through while looking like protection;
 *   - a fault is attributed and counted, and after enough of them the host **stops calling** the plugin
 *     rather than paying for it on every step of every run;
 *   - a plugin's own **refusal** is not a fault: the two must stay distinguishable, because one is a
 *     limitation a user can act on and the other is a defect to report;
 *   - and a plugin that **segfaults** is out of scope, which is stated in the header rather than implied
 *     away. The death-test half of that boundary is checked by `plugin.guard.a_hard_crash_is_out_of_scope`
 *     in a separate process, because the honest way to document an unsurvivable failure is to observe it.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugin/guard.hpp>

#include <qp/diag/result.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace qp::plugin;

namespace {

/// @brief What a plugin call can do in these cases.
enum class Behaviour {
    /// Return normally with a value.
    succeed,
    /// Return the plugin's own refusal -- a limitation, not a fault.
    refuse,
    /// Raise a `std::exception` with a message.
    raise_with_message,
    /// Raise something that is not a `std::exception` at all.
    raise_without_words,
};

/// @brief One call, in the shape the barrier requires: it returns a `diag::Result`.
[[nodiscard]] qp::diag::Result<int> one_call(Behaviour behaviour) {
    switch (behaviour) {
        case Behaviour::succeed:
            return 7;
        case Behaviour::refuse:
            return qp::diag::ErrorCode::not_implemented;
        case Behaviour::raise_with_message:
            throw std::runtime_error{"the plugin has a memory bug"};
        case Behaviour::raise_without_words:
            throw 42;   // NOLINT: deliberate -- a plugin may throw anything, and the barrier must catch it
    }
    return qp::diag::ErrorCode::internal_error;
}

}  // namespace

TEST_CASE("plugin.guard.a_throw_becomes_a_failure", "[plugin][guard]") {
    // The property C4 is about. Without the barrier this test does not fail an assertion -- it takes the test
    // process down, which is exactly what it would do to a user's session.
    FaultLog faults;
    const auto ok = call_guarded("demo.spring_damper", &faults,
                                 [] { return one_call(Behaviour::succeed); });
    REQUIRE(ok.has_value());
    REQUIRE(ok.value() == 7);
    REQUIRE(faults.faults().empty());

    const auto raised = call_guarded("demo.spring_damper", &faults,
                                     [] { return one_call(Behaviour::raise_with_message); });
    REQUIRE_FALSE(raised.has_value());
    REQUIRE(raised.error() == qp::diag::ErrorCode::plugin_fault);

    // The fault is recorded with the label the caller chose, the message the plugin gave, and a count -- so a
    // report can say **which** code misbehaved and how often, rather than "something went wrong".
    REQUIRE(faults.faults().size() == 1);
    REQUIRE(faults.faults().front().label == "demo.spring_damper");
    REQUIRE(faults.faults().front().code == qp::diag::ErrorCode::plugin_fault);
    REQUIRE(faults.faults().front().what == "the plugin has a memory bug");
    REQUIRE(faults.faults().front().count == 1);
    REQUIRE_FALSE(faults.faults().front().quarantined);

    // A barrier with no log still protects the host: the exception is caught and reported. Nothing is
    // counted, so nothing is quarantined -- stated in the header, and asserted here so it stays stated.
    const auto unlogged = call_guarded("anonymous", nullptr,
                                       [] { return one_call(Behaviour::raise_with_message); });
    REQUIRE_FALSE(unlogged.has_value());
    REQUIRE(unlogged.error() == qp::diag::ErrorCode::plugin_fault);
}

TEST_CASE("plugin.guard.an_exception_with_no_words_is_still_recorded", "[plugin][guard]") {
    // `throw 42` is legal C++, and a plugin may do it. A barrier that catches only `std::exception` would let
    // it through -- and it would look like protection right up to the moment it was needed. The empty `what`
    // is itself information: the plugin raised something that cannot say why.
    FaultLog faults;
    const auto raised = call_guarded("demo.spring_damper", &faults,
                                     [] { return one_call(Behaviour::raise_without_words); });
    REQUIRE_FALSE(raised.has_value());
    REQUIRE(raised.error() == qp::diag::ErrorCode::plugin_fault);
    REQUIRE(faults.faults().size() == 1);
    REQUIRE(faults.faults().front().what.empty());
    REQUIRE(faults.faults().front().count == 1);
}

TEST_CASE("plugin.guard.a_refusal_is_not_a_fault", "[plugin][guard]") {
    // The distinction the two codes exist for. A plugin returning "I cannot integrate damping" is working:
    // the user can act on it, and the run's own report already has a sentence for it. Counting it as a fault
    // would eventually quarantine a plugin that is merely limited -- and the reason it stopped being called
    // would be invisible.
    FaultLog faults;
    const auto refused = call_guarded("demo.spring_damper", &faults,
                                      [] { return one_call(Behaviour::refuse); });
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == qp::diag::ErrorCode::not_implemented);
    REQUIRE(faults.faults().empty());
    REQUIRE_FALSE(faults.is_quarantined("demo.spring_damper"));
    REQUIRE(faults.quarantined_count() == 0);
}

TEST_CASE("plugin.guard.the_host_stops_calling_a_broken_plugin", "[plugin][guard]") {
    // After enough faults the host stops asking. Two reasons, and both are about the user rather than about
    // tidiness: a broken plugin called once per step of a 4096-step run would fill the log with the same
    // message, and the run would keep paying for a call that cannot succeed. The refusal is *named* -- a
    // caller can tell "we stopped asking" from "it failed again", which are different stories.
    FaultLog faults;
    const char* kLabel = "demo.spring_damper";

    for (int i = 1; i < kFaultLimit; ++i) {
        const auto raised = call_guarded(kLabel, &faults,
                                         [] { return one_call(Behaviour::raise_with_message); });
        REQUIRE(raised.error() == qp::diag::ErrorCode::plugin_fault);
        REQUIRE_FALSE(faults.is_quarantined(kLabel));
    }

    // The fault that crosses the limit is still reported as the fault it was: the caller sees what happened
    // *and* that it was the last straw.
    const auto last = call_guarded(kLabel, &faults,
                                   [] { return one_call(Behaviour::raise_with_message); });
    REQUIRE(last.error() == qp::diag::ErrorCode::plugin_fault);
    REQUIRE(faults.is_quarantined(kLabel));
    REQUIRE(faults.faults().back().quarantined);
    REQUIRE(faults.faults().back().count == kFaultLimit);
    REQUIRE(faults.quarantined_count() == 1);

    // From here the plugin is not called at all, and the code says so. The lambda counts its own invocations,
    // because "was it called" is the property -- not what it returned.
    int calls = 0;
    const auto afterwards = call_guarded(kLabel, &faults, [&calls] {
        ++calls;
        return one_call(Behaviour::succeed);
    });
    REQUIRE(calls == 0);
    REQUIRE_FALSE(afterwards.has_value());
    REQUIRE(afterwards.error() == qp::diag::ErrorCode::plugin_quarantined);

    // The count does not grow while it is out of service: the log records faults, and a call that never
    // happened is not one.
    REQUIRE(faults.faults().size() == static_cast<std::size_t>(kFaultLimit));

    // One plugin's faults are not another's: attributing them together would quarantine an innocent plugin
    // for its neighbour's bug.
    const auto other = call_guarded("demo.incline", &faults,
                                    [] { return one_call(Behaviour::succeed); });
    REQUIRE(other.has_value());
    REQUIRE_FALSE(faults.is_quarantined("demo.incline"));
}

TEST_CASE("plugin.guard.healing_is_not_automatic", "[plugin][guard]") {
    // A quarantined plugin stays quarantined. Something that misbehaved three times has earned a restart,
    // not a retry: clearing the strikes on the next success would let it start over and fail three more
    // times, which is the same log filling up at a slower rate. Clearing is the caller's decision, and the
    // caller's decision is to construct a new log.
    FaultLog faults;
    const char* kLabel = "demo.spring_damper";
    for (int i = 0; i < kFaultLimit; ++i) {
        (void)call_guarded(kLabel, &faults, [] { return one_call(Behaviour::raise_with_message); });
    }
    REQUIRE(faults.is_quarantined(kLabel));

    // A request that would have succeeded is still refused, because it is never made.
    const auto refused = call_guarded(kLabel, &faults, [] { return one_call(Behaviour::succeed); });
    REQUIRE(refused.error() == qp::diag::ErrorCode::plugin_quarantined);

    // A fresh log is a fresh start: the same label may be tried again after a restart, which is what makes
    // the quarantine a policy rather than a permanent verdict.
    FaultLog restarted;
    const auto allowed = call_guarded(kLabel, &restarted, [] { return one_call(Behaviour::succeed); });
    REQUIRE(allowed.has_value());
    REQUIRE_FALSE(restarted.is_quarantined(kLabel));
}

// ===========================================================================
// The boundary of the claim: a hard crash, observed rather than assumed
// ===========================================================================
//
// Charter C4 says a plugin crash must not take the host down, and qualifies it with "schema domain". The
// qualifier is the honest half: a `try` block catches a **throw**, and does nothing at all about a process
// that has already lost its memory safety. Rather than leaving that as a sentence in a header, this case
// observes it -- in a child process, because the observation is the process dying.
//
// The criterion is two-sided on purpose:
//   - the crash marker must appear, so the child really reached the fault;
//   - the survived marker must **not** appear, so a build where the fault turned out to be survivable cannot
//     pass by printing the first marker and then exiting cleanly.
//
// The null dereference below is a deliberate language violation in a process that exists only to die. It is
// the shortest honest way to produce the thing under discussion, and it is confined to a test binary.

TEST_CASE("plugin.guard.a_hard_crash_is_out_of_scope", "[.][plugin][guard][death]") {
    const char* self = std::getenv("QP_TEST_SELF");
    if (self == nullptr || *self == '\0') {
        SKIP("QP_TEST_SELF is not set: this case runs as a death child process, registered by CMake");
    }

    // -- The code below only runs in the death child process --
    std::fputs("plugin guard: a hard crash is out of scope -- about to fault\n", stdout);
    std::fflush(stdout);

    FaultLog faults;
    (void)call_guarded("demo.wild", &faults, [] {
        // Not a throw: the barrier has nothing to catch, and no in-process construct would help.
        volatile int* wild = nullptr;
        *wild = 1;
        return qp::diag::Result<int>{0};
    });

    // Reached only if the fault was survivable, which is exactly the claim this case exists to deny.
    std::fputs("plugin guard: SURVIVED the hard crash\n", stdout);
    std::fflush(stdout);
    std::_Exit(0);
}
