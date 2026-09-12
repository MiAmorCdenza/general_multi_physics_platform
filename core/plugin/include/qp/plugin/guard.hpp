/**
 * @file guard.hpp
 * @brief Calling into a plugin without letting it take the process down.
 *
 * ## The failure this exists for
 *
 * Charter C4 says a plugin crash must not take the host with it, and the qualifier is the honest part of the
 * requirement: "schema domain". There are two kinds of plugin misbehaviour and only one of them is
 * survivable in process:
 *
 *   - a plugin that **raises** (a `std::bad_alloc`, an out-of-range `at()`, a `throw` of its own) or that
 *     returns something which does not match what it declared. A `catch (...)` around the call converts this
 *     into a diagnosable failure, and every caller of that plugin afterwards is protected by the
 *     quarantine. **This file makes that a property rather than a hope.**
 *   - a plugin that **segfaults**, corrupts memory, or deadlocks. No in-process construct survives this --
 *     not a `try` block, not a signal handler that is expected to keep running. The only real answers are a
 *     separate process or a thread that can be abandoned, and both are large enough to be their own piece
 *     of work. Until then this limit is **stated** in the contract rather than implied away: a host that
 *     claims to survive a segfault and does not is worse than one that says where its boundary is.
 *
 * ## Why the barrier is a call wrapper and not a `noexcept` on the interface
 *
 * `INodeEvaluator::evaluate` returns `Result<...>` and is deliberately **not** `noexcept`. Marking it
 * `noexcept` would not isolate anything: a `throw` inside a `noexcept` function calls `std::terminate`, so
 * an interface that promised not to throw would kill the process precisely when a plugin misbehaved.
 * Catching at the call site is the only form of this that works, and it has to be at **every** call site --
 * which is why the wrapper is a template here rather than a convention each module reimplements.
 *
 * ## Why a fault is not a refusal
 *
 * A plugin that returns "I cannot do this" is working: that is a limitation with a name, and the user can
 * act on it. A plugin that raises is broken. Folding the two into one code would make a report say the same
 * thing about a spring-damper with damping it cannot integrate and about a plugin with a memory bug.
 *
 * @ownership   observes (the callable, the fault log)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A fault never propagates out of `call_guarded`
 * @errors      Reports through `diag::Result` and `FaultLog`
 * @frozen      no
 * @tests       plugin.guard.a_throw_becomes_a_failure,
 *              plugin.guard.a_hard_crash_is_out_of_scope,
 *              plugin.guard.a_refusal_is_not_a_fault
 */
#pragma once

#include <qp/diag/error.hpp>
#include <qp/diag/result.hpp>

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace qp::plugin {

/**
 * @brief How many faults one plugin may produce before the host stops calling it.
 *
 * Three, and the number is a decision rather than a round figure. Zero would quarantine on the first fault,
 * which throws away a plugin that has one unlucky edge case and would make the host's answer for "a plugin
 * failed once" the same as for "a plugin is broken" -- the first deserves a diagnostic, the second deserves
 * a refusal. A large number would let a broken plugin fill the log and slow every run, because the host
 * would keep paying for it. Three is enough to distinguish "it happened" from "it keeps happening", and the
 * counter is reported so the user can see which of the two they have.
 */
inline constexpr int kFaultLimit = 3;

/**
 * @brief One plugin fault, as the log and the report need it.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `label` names the code that misbehaved, not the caller
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.guard.a_throw_becomes_a_failure
 */
struct Fault final {
    /// What misbehaved, in the caller's terms: a node type, an operator name, a format name. The host
    /// chooses the label because only it knows what the user was looking at.
    std::string label{};
    /// The failure code the caller will see.
    diag::ErrorCode code = diag::ErrorCode::ok;
    /// What the plugin said when it raised, when it said anything. Empty for a non-`std::exception` throw,
    /// which is itself worth recording: an empty message means "it threw something with no words".
    std::string what{};
    /// How many faults this label has produced, including this one.
    int count = 0;
    /// Whether this fault crossed the limit and put the label out of service.
    bool quarantined = false;
};

/**
 * @brief Remembers which plugins have misbehaved, and refuses to keep calling them.
 *
 * The state is an **object** the caller owns rather than a process-wide registry. A global would be a
 * hidden dependency between unrelated runs -- a test that made one plugin fail would change what the next
 * test sees -- and the host already has an object per session to hang it on.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A label's count only ever grows, and a quarantined label stays quarantined until a new log
 *              exists -- see `plugin.guard.healing_is_not_automatic`, which asserts that a success does not
 *              clear the strikes
 * @errors      noexcept
 * @frozen      no
 * @tests       plugin.guard.the_host_stops_calling_a_broken_plugin,
 *              plugin.guard.healing_is_not_automatic
 */
class FaultLog final {
public:
    FaultLog() = default;
    FaultLog(const FaultLog&) = delete;
    FaultLog& operator=(const FaultLog&) = delete;

    /// @brief Records a fault against `label` and returns the entry, filled in.
    ///
    /// @ownership   owns the returned row
    /// @thread      main
    /// @pre         none
    /// @post        `label`'s count is one higher, and `quarantined` is true once it reaches `kFaultLimit`
    /// @invariant   A quarantined label stays quarantined: nothing here clears a strike, because a plugin
    ///              that misbehaved three times has earned a restart rather than a retry
    /// @errors      May allocate; allocation failure terminates
    /// @complexity  O(labels)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.guard.the_host_stops_calling_a_broken_plugin
    [[nodiscard]] Fault record(std::string label, diag::ErrorCode code, std::string what);

    /// @brief Whether `label` has crossed the limit.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        true exactly when `label` has `kFaultLimit` or more faults
    /// @invariant   Monotonic per label
    /// @errors      noexcept
    /// @complexity  O(labels)
    /// @nondet      none
    /// @frozen      no
    /// @tests       plugin.guard.the_host_stops_calling_a_broken_plugin
    [[nodiscard]] bool is_quarantined(std::string_view label) const noexcept;

    /// @brief The faults recorded so far, in the order they happened.
    [[nodiscard]] const std::vector<Fault>& faults() const noexcept { return faults_; }

    /// @brief How many distinct labels have been quarantined.
    [[nodiscard]] std::size_t quarantined_count() const noexcept;

private:
    std::vector<Fault> faults_{};
};

/**
 * @brief Calls plugin code, converting a raised exception into a failed `Result`.
 *
 * `call` must return a `diag::Result<T>`; the wrapper returns the same type, so a call site changes from
 * `plugin->work(...)` to `call_guarded([&] { return plugin->work(...); })` and nothing else moves.
 *
 * Everything is caught, including things that are not `std::exception`: a plugin may throw an `int`, and a
 * barrier that only catches `std::exception` would let that through while looking like it was protecting
 * the host.
 *
 * @param label Where to attribute a fault. Passed through to the log rather than derived here, because only
 *              the caller knows whether the user is looking at a node type, an operator, or a format.
 * @param log   Where faults are recorded, or null for a caller that only wants the barrier. A null log means
 *              faults are converted and reported but **not** counted, so no quarantine happens -- stated
 *              rather than silently doing nothing.
 * @param call  The call to make. Invoked at most once; not invoked at all when `label` is quarantined.
 *
 * @ownership   observes `call`
 * @thread      main
 * @pre         none
 * @post        A `Result` of `call`'s type comes back; a fault becomes `plugin_fault` (or
 *              `plugin_quarantined` when `label` is already out of service) with `call` never having run
 * @invariant   Never throws, whatever the plugin does
 * @errors      noexcept (allocation failure inside the returned `Result` terminates)
 * @complexity  O(1) plus the call
 * @nondet      none
 * @frozen      no
 * @tests       plugin.guard.a_throw_becomes_a_failure,
 *              plugin.guard.the_host_stops_calling_a_broken_plugin,
 *              plugin.guard.an_exception_with_no_words_is_still_recorded
 */
template <typename F>
[[nodiscard]] auto call_guarded(std::string_view label, FaultLog* log, F&& call) noexcept
    -> std::invoke_result_t<F> {
    using ResultType = std::invoke_result_t<F>;

    // Out of service: refused without calling. The report says *why* the call did not happen rather than
    // pretending the plugin failed again, because "we stopped asking" and "it just failed" are different
    // stories and the second one keeps a user waiting for a fix that will not come.
    if (log != nullptr && log->is_quarantined(label)) {
        return ResultType{diag::ErrorCode::plugin_quarantined};
    }

    try {
        return call();
    } catch (const std::exception& e) {
        if (log != nullptr) {
            (void)log->record(std::string{label}, diag::ErrorCode::plugin_fault, std::string{e.what()});
        }
        return ResultType{diag::ErrorCode::plugin_fault};
    } catch (...) {
        if (log != nullptr) {
            (void)log->record(std::string{label}, diag::ErrorCode::plugin_fault, std::string{});
        }
        return ResultType{diag::ErrorCode::plugin_fault};
    }
}

}  // namespace qp::plugin
