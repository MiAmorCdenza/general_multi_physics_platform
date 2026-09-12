/**
 * @file test_execution.cpp
 * @brief Tests for the run loop itself: stepping, recording, and argument handling.
 *
 * ## Why the loop's cases and the binding's cases live in different files
 *
 * They were one file, driven through a real plugin node, and the contract gate failed in a way worth
 * recording because the failure was the design talking. The plugin headers claim binding-related ids;
 * the gate attributes cases **per invocation**; and a case under `tests/unit/plugins/` is invisible to
 * the core invocation. Two modules cannot share one test file when ownership is judged globally.
 *
 * The split that resolved it is not administrative. These cases are about the **loop** -- does it
 * record the initial condition, does a second run start clean, is a bad `dt` refused -- and none of
 * that depends on which physics ran. They drive a stub operator, so they test the loop rather than a
 * plugin, and they belong to the loop's module.
 *
 * `tests/unit/plugins/test_execution_binding.cpp` holds the other half: which node becomes which
 * operator, and what is refused. That half genuinely needs the plugin.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/execution/execution.hpp>

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace execution = qp::graph::execution;
namespace rt = qp::runtime;
using qp::graph::Node;

/// @brief An operator that advances a harmonic state by an exact rotation.
///
/// A stub rather than a real kernel, so these cases test the loop and nothing else. Integrating
/// `x'' = -omega^2 x` as a rotation makes the trajectory predictable without depending on any
/// integrator's error: if the loop mis-records a sample, the value is wrong by a rotation step rather
/// than by a rounding difference, which is the difference between a test that catches a defect and one
/// that catches nothing.
class RotationOperator final : public execution::IStateOperator {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test.rotation"; }

    [[nodiscard]] qp::diag::Result<void> step(execution::StateView& state, double dt) override {
        if (!state.is_consistent()) return qp::diag::ErrorCode::invalid_argument;
        for (std::size_t i = 0; i < state.count; ++i) {
            const double x = state.at(i, 0);
            const double v = state.at(i, 1);
            const double omega = state.at(i, 2);
            // A non-finite or zero frequency is refused, which is how this stub produces a failing
            // step without needing any physics to diverge.
            if (!std::isfinite(omega) || omega == 0.0) return qp::diag::ErrorCode::invalid_argument;
            const double c = std::cos(omega * dt);
            const double s = std::sin(omega * dt);
            state.set(i, 0, x * c + (v / omega) * s);
            state.set(i, 1, -x * omega * s + v * c);
        }
        return {};
    }
};

/// @brief A binder that answers for any type, so the loop can be exercised without a plugin.
class AnyTypeBinder final : public execution::IOperatorBinder {
public:
    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view, const Node&, const execution::StateView&) override {
        return std::make_unique<RotationOperator>();
    }
};

/// @brief A binder that refuses everything, for the "nothing can run this" case.
class NoTypeBinder final : public execution::IOperatorBinder {
public:
    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view, const Node&, const execution::StateView&) override {
        return nullptr;
    }
};

/// @brief A binder that answers for exactly one type name, so ordering can be exercised.
class NamedTypeBinder final : public execution::IOperatorBinder {
public:
    explicit NamedTypeBinder(std::string_view wanted) : wanted_(wanted) {}
    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view type_name, const Node&, const execution::StateView&) override {
        if (type_name != wanted_) return nullptr;
        return std::make_unique<RotationOperator>();
    }

private:
    std::string_view wanted_;
};

[[nodiscard]] Node any_node(std::string_view type_name) {
    Node node;
    node.id = qp::graph::NodeId{1, 1};
    node.type_name = std::string{type_name};
    return node;
}

}  // namespace

TEST_CASE("execution.loop.records_the_initial_condition", "[execution]") {
    // The first sample is taken **before** the first step, so a trace of `n` steps holds `n + 1`
    // samples. Without it the initial state is unrecoverable from the record, and the confidence panel
    // measures its energy drift against exactly that state -- a trace that began after step one would
    // have nothing to measure against.
    AnyTypeBinder binder;
    execution::GraphRun run;
    const Node node = any_node("anything");

    REQUIRE(run.prepare(node, {&binder}, rt::RunId{7}).has_value());
    REQUIRE(run.is_ready());
    REQUIRE(run.operator_name() == "test.rotation");
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(2.0);

    const std::size_t steps = 100;
    const double dt = 1.0e-3;
    const execution::RunOutcome outcome = run.run(steps, dt);
    REQUIRE(outcome.ok());
    REQUIRE(outcome.completed);
    REQUIRE(outcome.steps == steps);
    REQUIRE(run.trace().size() == steps + 1);

    // The channels are named for what they carry, and the trace belongs to the run it was prepared
    // with -- a trace with no run identity cannot be traced back to a configuration.
    REQUIRE(run.trace().channel_count() == 2);
    REQUIRE(run.trace().channels()[0].name == execution::GraphRun::kPositionChannel);
    REQUIRE(run.trace().channels()[1].name == execution::GraphRun::kVelocityChannel);
    REQUIRE(run.trace().run() == rt::RunId{7});

    // The first sample is the initial condition, exactly, at `t = 0`.
    REQUIRE(run.trace().samples().front().t == 0.0);
    REQUIRE(run.trace().samples().front().values[0].value == 1.0);
    REQUIRE(run.trace().samples().front().values[1].value == 0.0);

    // The last sample is at `steps * dt`, and the state agrees with it -- which is what lets a second
    // run continue from where this one stopped.
    const double t_end = dt * static_cast<double>(steps);
    REQUIRE(run.trace().samples().back().t == t_end);
    REQUIRE(run.trace().samples().back().values[0].value == run.state().at(0, 0));
    REQUIRE(run.trace().samples().back().values[1].value == run.state().at(0, 1));

    // Times strictly increase, which the trace requires and a caller relies on to fit.
    const std::vector<rt::Sample>& samples = run.trace().samples();
    for (std::size_t i = 1; i < samples.size(); ++i) {
        REQUIRE(samples[i].t > samples[i - 1].t);
    }
}

TEST_CASE("execution.loop.second_run_replaces_the_trace", "[execution]") {
    // A second run must not append to the first one's samples. Two runs in one trace would make the
    // time axis go backwards at the seam, and the trace refuses out-of-order times -- so the failure
    // would surface as a mystifying append error rather than as "you ran it twice".
    AnyTypeBinder binder;
    execution::GraphRun run;
    const Node node = any_node("anything");

    REQUIRE(run.prepare(node, {&binder}, rt::RunId{1}).has_value());
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(2.0);
    REQUIRE(run.run(100, 1.0e-3).ok());
    REQUIRE(run.trace().size() == 101);

    REQUIRE(run.prepare(node, {&binder}, rt::RunId{2}).has_value());
    REQUIRE(run.trace().size() == 0);
    REQUIRE(run.trace().run() == rt::RunId{2});

    // `prepare` resets the state, so the omega component and the initial condition have to be set
    // again. This assertion is why the case is worth having: the first version re-ran without
    // re-initialising, the stub refused a zero frequency, and the run failed -- which is exactly what
    // would happen to a caller who assumed `prepare` kept the previous state.
    REQUIRE(run.state().at(0, 2) == 0.0);
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(2.0);

    REQUIRE(run.run(50, 1.0e-3).ok());
    REQUIRE(run.trace().size() == 51);
}

TEST_CASE("execution.loop.rejects_unusable_arguments", "[execution]") {
    AnyTypeBinder binder;
    const Node node = any_node("anything");

    // An invalid run identity is refused at the point the trace would be created, and zero particles
    // is not a run.
    {
        execution::GraphRun run;
        REQUIRE_FALSE(run.prepare(node, {&binder}, rt::RunId{}).has_value());
        REQUIRE_FALSE(run.prepare(node, {&binder}, rt::RunId{1}, 0).has_value());
    }

    // No binder claims the type: `not_implemented`, so the caller can name what it could not run
    // rather than reporting a generic failure.
    {
        execution::GraphRun run;
        NoTypeBinder none;
        const auto prepared = run.prepare(node, {&none}, rt::RunId{1});
        REQUIRE_FALSE(prepared.has_value());
        REQUIRE(prepared.error() == qp::diag::ErrorCode::not_implemented);
        REQUIRE_FALSE(run.is_ready());
    }

    // A null entry is skipped rather than dereferenced: a caller assembling binders from optional
    // plugins will have holes, and a later binder still gets its turn.
    {
        execution::GraphRun run;
        NamedTypeBinder specific{"wanted.type"};
        REQUIRE(run.prepare(any_node("wanted.type"), {nullptr, &specific}, rt::RunId{1}).has_value());
        REQUIRE(run.is_ready());
    }

    // A binder answers for one name and declines another.
    {
        NamedTypeBinder specific{"wanted.type"};
        execution::GraphRun declined;
        REQUIRE_FALSE(declined.prepare(any_node("other.type"), {&specific}, rt::RunId{1}).has_value());

        execution::GraphRun accepted;
        REQUIRE(accepted.prepare(any_node("wanted.type"), {&specific}, rt::RunId{1}).has_value());
    }

    execution::GraphRun run;
    REQUIRE(run.prepare(node, {&binder}, rt::RunId{1}).has_value());

    // An out-of-range particle is reported rather than silently dropped: a run that ignored it would
    // show fewer particles than the user thought they configured.
    REQUIRE_FALSE(run.set_initial(5, 1.0, 0.0).has_value());
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(2.0);

    // Zero steps is legal and records the initial condition alone -- "show me the starting state" is a
    // reasonable thing to ask.
    const execution::RunOutcome none = run.run(0, 0.01);
    REQUIRE(none.ok());
    REQUIRE(none.steps == 0);
    REQUIRE(run.trace().size() == 1);

    // A non-positive or non-finite step is refused before any state is touched.
    for (const double bad : {0.0, -0.01, std::nan("")}) {
        execution::GraphRun other;
        REQUIRE(other.prepare(node, {&binder}, rt::RunId{2}).has_value());
        const execution::RunOutcome refused = other.run(10, bad);
        REQUIRE_FALSE(refused.ok());
        REQUIRE(refused.error == qp::diag::ErrorCode::invalid_argument);
        REQUIRE(refused.steps == 0);
        REQUIRE(other.trace().empty());
    }

    // An unprepared run reports rather than dereferencing a null operator.
    {
        execution::GraphRun fresh;
        const execution::RunOutcome refused = fresh.run(10, 0.01);
        REQUIRE_FALSE(refused.ok());
        REQUIRE(refused.error == qp::diag::ErrorCode::invalid_argument);
    }
}

TEST_CASE("execution.loop.failure_keeps_earlier_samples", "[execution]") {
    // A failing step stops the run and **keeps** the samples already taken. A trace that stops at step
    // 900 is more informative than an empty one: the confidence panel can show where it went wrong,
    // and discarding the evidence leaves the user with nothing to look at.
    //
    // The stub refuses a non-finite frequency, which reaches the same code path as a diverging kernel
    // without needing any physics to diverge.
    AnyTypeBinder binder;
    execution::GraphRun run;
    REQUIRE(run.prepare(any_node("anything"), {&binder}, rt::RunId{1}).has_value());
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(std::nan(""));

    const execution::RunOutcome outcome = run.run(10, 1.0e-3);
    REQUIRE_FALSE(outcome.ok());
    REQUIRE(outcome.error != qp::diag::ErrorCode::ok);
    // The initial condition survived, which is the sample that makes the partial record useful.
    REQUIRE(run.trace().size() == 1);
    REQUIRE(outcome.steps == 0);
}

TEST_CASE("execution.loop.state_view_is_total", "[execution]") {
    // `StateView`'s accessors are total: an out-of-range read is zero and an out-of-range write does
    // nothing. That makes a layout mismatch a **wrong answer rather than a fault**, which is why the
    // adapter checks the layout instead of trusting it -- a fault would at least be noticed.
    execution::StateView state = execution::StateView::zeroed(2, 3);
    REQUIRE(state.count == 2);
    REQUIRE(state.components_per_particle == 3);
    REQUIRE(state.values.size() == 6);
    REQUIRE(state.is_consistent());

    state.set(1, 2, 5.0);
    REQUIRE(state.at(1, 2) == 5.0);

    REQUIRE(state.at(9, 0) == 0.0);
    state.set(9, 0, 1.0);
    REQUIRE(state.values.size() == 6);
    REQUIRE(state.at(0, 0) == 0.0);

    REQUIRE(state.at(0, 9) == 0.0);
    state.set(0, 9, 1.0);
    REQUIRE(state.at(0, 0) == 0.0);

    // A zero component count is normalised to one rather than producing a zero-length layout that
    // every index would miss.
    const execution::StateView degenerate = execution::StateView::zeroed(3, 0);
    REQUIRE(degenerate.components_per_particle == 1);
    REQUIRE(degenerate.values.size() == 3);
    REQUIRE(degenerate.is_consistent());

    // A hand-built inconsistent layout reports itself rather than being stepped.
    execution::StateView broken;
    broken.count = 2;
    broken.components_per_particle = 3;
    broken.values = {1.0, 2.0};  // too few for two particles
    REQUIRE_FALSE(broken.is_consistent());
}
