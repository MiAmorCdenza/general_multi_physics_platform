/**
 * @file sink.hpp
 * @brief Diagnostic sink: where a diagnostic goes after it is produced.
 *
 * Why the sink is an **interface**, not a global log object:
 *   1. "Mutable global state" is the most common shape of unclear ownership (enforcement.md
 *      §3's `qp-no-mutable-global`); a global singleton sink lets tests pollute each other.
 *   2. Hosts differ: the CLI writes to stderr, the GUI collects into a panel, tests into an
 *      assertion list. The three should not know about each other.
 *   3. Under multi-threaded evaluation, "who synchronizes this global object" has no answer.
 *
 * @ownership   observes (the Sink does not own the diagnostic; it may discard it after reading)
 * @thread      any (the implementer ensures thread safety; both the eval thread and the main thread may emit)
 * @pre         none
 * @post        none
 * @invariant   The implementer must not store a reference to the passed-in Diagnostic
 * @errors      emit must not throw (the host must be able to record even in the worst case)
 * @frozen      yes (the interface shape is frozen)
 * @tests       diag.sink.collecting_sink, diag.sink.null_sink
 */
#pragma once

#include <qp/diag/diagnostic.hpp>

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace qp::diag {

/**
 * @brief Diagnostic sink interface.
 *
 * Implementer contract:
 *   - `emit` must have noexcept semantics (it never throws); it swallows internal failures.
 *   - **Must not store** a `const Diagnostic&`; retaining one requires a copy.
 */
class ISink {
public:
    ISink() = default;
    virtual ~ISink() = default;
    ISink(const ISink&) = delete;
    ISink& operator=(const ISink&) = delete;
    ISink(ISink&&) = delete;
    ISink& operator=(ISink&&) = delete;

    /// @brief Receive one diagnostic. The implementer is responsible for thread safety.
    virtual void emit(const Diagnostic& d) noexcept = 0;

    /// @brief Stable short name of the sink; used to answer "where did the log go".
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

/// @brief Discards every diagnostic. For scenarios that need none (benchmarks, pure-computation tests).
class NullSink final : public ISink {
public:
    void emit(const Diagnostic&) noexcept override {}
    [[nodiscard]] const char* name() const noexcept override { return "null"; }
};

/**
 * @brief Collects diagnostics in memory. Used by tests and by hosts that display them at the end.
 *
 * Thread safety: both `emit` and `items` take the lock (a diagnostic may come from an eval thread).
 *
 * @ownership   owns (copies and stores every diagnostic)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The order of items() is the order in which emit was called
 * @errors      emit is noexcept (allocation failure terminates; a sink must not become a new failure source)
 * @frozen      no
 * @tests       diag.sink.collecting_sink, diag.sink.collecting_sink_thread_safe
 */
class CollectingSink final : public ISink {
public:
    void emit(const Diagnostic& d) noexcept override {
        // Deliberately no exception handling: allocation failure means terminate.
        // If the sink itself could fail, "error handling" would become a source of errors.
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(d);
    }

    [[nodiscard]] const char* name() const noexcept override { return "collecting"; }

    /// @brief Number of collected diagnostics.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.empty();
    }

    /// @brief A **copy** of the collected diagnostics. A copy, not a reference: a reference dies when the lock is released.
    [[nodiscard]] std::vector<Diagnostic> items() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_;
    }

    /// @brief Clear. Test cases must clear between cases, or they pollute each other.
    void clear() noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

    /// @brief Whether at least one diagnostic with the given error code was received.
    [[nodiscard]] bool contains(ErrorCode code) const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& d : items_) {
            if (d.code() == code) return true;
        }
        return false;
    }

    /// @brief The worst consequence level among the collected diagnostics.
    [[nodiscard]] Consequence worst_consequence() const noexcept {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto worst = Consequence::recoverable;
        for (const auto& d : items_) {
            if (d.consequence() > worst) worst = d.consequence();
        }
        return worst;
    }

private:
    mutable std::mutex mutex_;   ///< mutable: const queries must lock too
    std::vector<Diagnostic> items_;
};

}  // namespace qp::diag
