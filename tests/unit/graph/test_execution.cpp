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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace execution = qp::graph::execution;
using execution::RunReadiness;
using execution::RunRefusal;
using execution::check_run;
using qp::graph::ResolveContext;
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
    [[nodiscard]] bool can_bind(std::string_view type_name,
                                const execution::StateView&) const noexcept override {
        (void)type_name;
        return true;
    }

    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view, const Node&, const execution::StateView&) override {
        return std::make_unique<RotationOperator>();
    }
};

/// @brief A binder that refuses everything, for the "nothing can run this" case.
class NoTypeBinder final : public execution::IOperatorBinder {
public:
    [[nodiscard]] bool can_bind(std::string_view type_name,
                                const execution::StateView&) const noexcept override {
        (void)type_name;
        return false;
    }

    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view, const Node&, const execution::StateView&) override {
        return nullptr;
    }
};

/// @brief A catalog that knows exactly one type, so the pre-flight has something to validate against.
///
/// `validate_graph` needs a catalog and a port registry; a null catalog would leave it unable to say anything,
/// and the case would then be asserting the behaviour of a half-configured context rather than of the
/// pre-flight. One type with one input and one output is enough to describe the node the stub operator serves.
class OneTypeCatalog final : public qp::graph::INodeCatalog {
public:
    explicit OneTypeCatalog(std::string type) : type_(std::move(type)) {
        desc_.type_name = type_;
        qp::graph::PortDesc in;
        in.number = 1;
        in.name = "in";
        in.connectable = false;
        desc_.inputs.push_back(in);
        qp::graph::PortDesc out;
        out.number = 1;
        out.name = "out";
        desc_.outputs.push_back(out);
    }

    [[nodiscard]] const qp::graph::NodeDesc* find(std::string_view type_name) const noexcept override {
        return type_name == type_ ? &desc_ : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept override { return 1; }

private:
    std::string type_;
    qp::graph::NodeDesc desc_{};
};
/// @brief An operator that raises on its first step, for charter C4's in-process half.
///
/// A plugin that throws used to take the host with it: `GraphRun::run` called `IStateOperator::step`
/// directly, and the exception travelled out of `run_once` and out of the event loop. This stub is what makes
/// that a test rather than a hope -- and it is a stub rather than a real plugin because the property belongs
/// to the **loop**, not to any physics.
class RaisingOperator final : public execution::IStateOperator {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test.raising"; }

    [[nodiscard]] qp::diag::Result<void> step(execution::StateView&, double) override {
        throw std::runtime_error{"the operator has a memory bug"};
    }
};

/// @brief A binder that answers for one type and hands back the operator that raises.
///
/// The loop takes **binders**, not operators: which operator a node becomes is the plugin's business, and the
/// loop only asks. So the fault has to be reachable the way a real one is -- through a binder -- or the test
/// would be exercising a path no caller has.
class RaisingBinder final : public execution::IOperatorBinder {
public:
    [[nodiscard]] bool can_bind(std::string_view type_name,
                                const execution::StateView&) const noexcept override {
        return type_name == "demo.raising";
    }

    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view type_name, const Node&, const execution::StateView&) override {
        if (type_name != "demo.raising") return nullptr;
        return std::make_unique<RaisingOperator>();
    }
};
/// @brief A binder that owns a type but can honour no instance of it.
///
/// The stub that makes `can_bind` and `bind` two different questions rather than one asked twice. Every
/// instance of the type is declined, so `can_bind` is true -- the type is this binder's -- while `bind`
/// returns null. A design that collapsed the two could express only one of the two answers, and the
/// answer it would lose is the one that tells a user whether to look for a plugin or at the two numbers
/// on the node in front of them.
class ClaimsButRefusesBinder final : public execution::IOperatorBinder {
public:
    [[nodiscard]] bool can_bind(std::string_view type_name,
                                const execution::StateView&) const noexcept override {
        return type_name == "mine.but.unusable";
    }

    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view, const Node&, const execution::StateView&) override {
        return nullptr;
    }
};

/// @brief A binder that answers for exactly one type name, so ordering can be exercised.
class NamedTypeBinder final : public execution::IOperatorBinder {
public:
    [[nodiscard]] bool can_bind(std::string_view type_name,
                                const execution::StateView&) const noexcept override {
        (void)type_name;
        return type_name == wanted_;
    }

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

TEST_CASE("execution.loop.run_can_be_named_after_binding", "[execution]") {
    // A caller that must bind **before** it opens a ledger entry prepares with an unknown identity and
    // supplies the real one afterwards. That ordering is not a convenience: opening the run first means a
    // refused bind leaves an entry for a run that never executed, and a ledger whose gaps read like
    // events is worse than no ledger -- the gap is the information.
    //
    // So `prepare` accepting `RunId{}` is the contract that makes the honest ordering possible, and this
    // case is what says so.
    AnyTypeBinder binder;
    const Node node = any_node("anything");

    execution::GraphRun run;
    REQUIRE(run.prepare(node, {&binder}, rt::RunId{}).has_value());
    REQUIRE(run.is_ready());
    // Bound but unfiled: the loop knows what would run, can already record, and has recorded nothing.
    // The channels come with binding rather than with the identity, so a bound loop is never a loop whose
    // first sample would be refused for having the wrong width.
    REQUIRE(run.trace().empty());
    REQUIRE(run.trace().channel_count() == 2);
    REQUIRE_FALSE(run.trace().run().valid());

    // Samples can be taken without an identity, and they carry none. Nothing is wrong yet; it is a trace
    // that has not been filed.
    REQUIRE(run.set_initial(0, 3.0, 0.0).has_value());
    run.set_omega(2.0);
    REQUIRE(run.run(10, 1.0e-3).ok());
    REQUIRE(run.trace().size() == 11);
    REQUIRE_FALSE(run.trace().run().valid());

    // Naming it replaces the trace, and this is the assertion that keeps the record honest: samples and
    // identity are one record, so relabelling the previous numbers would attribute them to a run that did
    // not produce them.
    run.set_run(rt::RunId{42});
    REQUIRE(run.trace().run() == rt::RunId{42});
    REQUIRE(run.trace().empty());
    REQUIRE(run.trace().channel_count() == 2);

    // A caller can append immediately: the channels came with the identity.
    REQUIRE(run.run(4, 1.0e-3).ok());
    REQUIRE(run.trace().size() == 5);
    REQUIRE(run.trace().run() == rt::RunId{42});

    // An id nobody issued cannot be used to erase a named trace.
    run.set_run(rt::RunId{});
    REQUIRE(run.trace().run() == rt::RunId{42});
    REQUIRE(run.trace().size() == 5);
}

TEST_CASE("execution.loop.can_bind_answers_for_the_type_not_the_instance", "[execution]") {
    // `can_bind` and `bind` answer two different questions, and the interface keeps them apart on purpose.
    //
    //   - `can_bind(type, layout)` -- "is this type mine at all, in a state I could describe?"
    //   - `bind(type, node, layout)` -- "can I honour *this instance*?", where the answer may be no for a
    //     reason the instance carries: a parameter nobody filled in, damping the kernel has no term for,
    //     an integrator with no implementation.
    //
    // The distinction is what a caller needs in order to say the right sentence. Collapsed into one, the
    // only available message for every refusal is the same, and the one it loses is the one that tells a
    // user whether to install a plugin or to look at the numbers already on the node.
    const execution::StateView layout = execution::StateView::zeroed(1);

    // The type is mine, and no instance of it is usable. That is expressible, and this is the stub that
    // proves the interface can express it.
    ClaimsButRefusesBinder unusable;
    REQUIRE(unusable.can_bind("mine.but.unusable", layout));
    REQUIRE(unusable.can_bind("yours", layout) == false);

    // It still declines the run: answering true to the first question is not a promise to run anything.
    // This is the pair a search-then-run caller must handle, and the graph layer's job is only to report
    // it -- naming the node is the caller's business.
    execution::GraphRun refused;
    const auto prepared = refused.prepare(any_node("mine.but.unusable"), {&unusable}, rt::RunId{1});
    REQUIRE_FALSE(prepared.has_value());
    REQUIRE(prepared.error() == qp::diag::ErrorCode::not_implemented);
    REQUIRE_FALSE(refused.is_ready());

    // And the answer to the first question does not depend on the node instance, which is what makes it
    // askable **before** there is anything to run: the same type and layout give the same answer for a
    // node with parameters and for one without.
    Node bare = any_node("mine.but.unusable");
    Node filled = any_node("mine.but.unusable");
    filled.set_param(2, qp::ports::Value{200.0});
    REQUIRE(filled.param(2).valid());
    REQUIRE(unusable.can_bind(bare.type_name, layout) == unusable.can_bind(filled.type_name, layout));

    // The layout is part of the question, because a binder that cannot describe the state shape cannot
    // run anything regardless of which node it is handed.
    const execution::StateView other_shape = execution::StateView::zeroed(2, 1);
    REQUIRE(other_shape.is_consistent());
    REQUIRE(unusable.can_bind("mine.but.unusable", other_shape) ==
            unusable.can_bind("mine.but.unusable", layout));
}

TEST_CASE("execution.check_run.answers_before_anything_steps", "[execution]") {
    // The pre-flight, which is the same shape as `check_export` one layer over: an answer that arrives before
    // the work is one a window can act on -- grey the button out, name the node to look at -- while an answer
    // that arrives as a failure is one the user reads after waiting.
    AnyTypeBinder binder;
    NoTypeBinder refuses;
    ClaimsButRefusesBinder declines;
    qp::graph::Graph graph;
    OneTypeCatalog catalog{"anything"};
    const ResolveContext ctx{&catalog, &qp::ports::builtin_registry()};

    // Nothing in it.
    RunReadiness empty = check_run(graph, ctx, {&binder});
    REQUIRE_FALSE(empty.ok());
    REQUIRE(empty.refusal == RunRefusal::empty_graph);
    REQUIRE_FALSE(empty.detail.empty());

    const auto added = graph.add_node("anything");
    REQUIRE(added.has_value());

    // A node whose type no binder claims: the graph is not empty, and this build cannot run it. The two
    // answers are different sentences because they are different problems.
    RunReadiness none = check_run(graph, ctx, {&refuses});
    REQUIRE_FALSE(none.ok());
    REQUIRE(none.refusal == RunRefusal::no_operator);
    REQUIRE(none.detail.find("no node") != std::string::npos);

    // A binder that owns the type but cannot honour this instance: named as the node type, so the user is sent
    // to the node rather than to a plugin list. A second graph, because the type has to be one the binder
    // actually claims -- otherwise this would be the `no_operator` answer above wearing a different name.
    qp::graph::Graph unusable;
    const auto unusable_node = unusable.add_node("mine.but.unusable");
    REQUIRE(unusable_node.has_value());
    OneTypeCatalog unusable_catalog{"mine.but.unusable"};
    const ResolveContext unusable_ctx{&unusable_catalog, &qp::ports::builtin_registry()};
    RunReadiness declined = check_run(unusable, unusable_ctx, {&declines});
    REQUIRE_FALSE(declined.ok());
    REQUIRE(declined.refusal == RunRefusal::node_cannot_be_honoured);
    REQUIRE(declined.type_name == "mine.but.unusable");
    REQUIRE(declined.node == unusable_node.value());
    REQUIRE(declined.detail.find("mine.but.unusable") != std::string::npos);

    // And the answer a caller acts on: which node, which operator, and that nothing was stepped.
    RunReadiness ready = check_run(graph, ctx, {&binder});
    REQUIRE(ready.ok());
    REQUIRE(ready.node == added.value());
    REQUIRE(ready.type_name == "anything");
    REQUIRE(ready.operator_name == "test.rotation");
    REQUIRE(ready.detail.find("test.rotation") != std::string::npos);
    // Nothing stepped: the graph is exactly as it was, with no trace anywhere.
    REQUIRE(graph.node_count() == 1);
}

TEST_CASE("execution.check_run.reports_validation_without_refusing", "[execution]") {
    // `graph/validate` reports dimension mismatches and missing required parameters, and a graph with those
    // **can still run**: the kernels do not enforce dimensions at run time. Refusing would make this
    // pre-flight stricter than the engine, and the first thing it would refuse is the built-in demonstration.
    // So the problems are reported and the run is allowed -- "here is what is wrong with it" is a confidence
    // panel's job, not the Run button's.
    AnyTypeBinder binder;
    qp::graph::Graph graph;
    const auto added = graph.add_node("anything");
    REQUIRE(added.has_value());
    OneTypeCatalog catalog{"anything"};
    const ResolveContext ctx{&catalog, &qp::ports::builtin_registry()};

    const RunReadiness readiness = check_run(graph, ctx, {&binder});
    REQUIRE(readiness.ok());
    // Whatever the empty catalog's verdict is, the answer is consistent: problems are counted in the struct and
    // never turn into a refusal.
    REQUIRE(readiness.validation_errors == readiness.validation_errors);
    if (readiness.validation_errors > 0) {
        REQUIRE_FALSE(readiness.first_problem.empty());
        REQUIRE(readiness.detail.find("validation problem") != std::string::npos);
    } else {
        REQUIRE(readiness.first_problem.empty());
    }
}
TEST_CASE("execution.loop.a_raising_operator_is_a_fault_not_a_crash", "[execution]") {
    // Charter C4, at the call site that matters most for a physics plugin: the loop calls `step` once per
    // step, 4096 times a run, and that call goes into plugin code. Before the barrier existed, an operator
    // that raised took the exception out through the run, the window's slot and the event loop -- the
    // process died, and the user lost whatever they had not saved.
    //
    // Three properties, and each is load-bearing:
    //   - the failure comes back as a **code**, so the caller can report it;
    //   - the samples already taken are kept, because a run that faulted at step 900 is evidence and an
    //     empty trace is not;
    //   - the fault is attributed to the operator's name, so a report can say which one.
    RaisingBinder raising;

    execution::GraphRun run;
    REQUIRE(run.prepare(any_node("demo.raising"), {&raising}, rt::RunId{7}).has_value());
    REQUIRE(run.is_ready());
    REQUIRE(run.operator_name() == "test.raising");
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(2.0);

    const execution::RunOutcome outcome = run.run(10, 1.0e-3);
    REQUIRE_FALSE(outcome.ok());
    // Not `invalid_argument`, not `internal_error`: the code names the cause, so a report can distinguish a
    // plugin that misbehaved from a run that was asked to do something impossible.
    REQUIRE(outcome.error == qp::diag::ErrorCode::plugin_fault);
    REQUIRE(outcome.steps == 0);

    // The initial condition is in the record and nothing else is, because the very first step faulted. The
    // loop keeps what it recorded rather than discarding the run.
    REQUIRE(run.trace().size() == 1);
    REQUIRE(run.trace().samples().front().t == 0.0);
    REQUIRE(run.trace().samples().front().values[0].value == 1.0);

    // And the fault is on the record, with the operator's name and the plugin's own words.
    REQUIRE(run.faults().faults().size() == 1);
    REQUIRE(run.faults().faults().front().label == "test.raising");
    REQUIRE(run.faults().faults().front().what == "the operator has a memory bug");
    REQUIRE(run.faults().faults().front().count == 1);

    // A caller that runs the same broken operator again is protected by the same log only if it shares it,
    // which is the point of the log being the caller's object rather than a global: two runs are two runs.
    REQUIRE_FALSE(run.faults().is_quarantined("test.raising"));
}
TEST_CASE("execution.loop.rejects_unusable_arguments", "[execution]") {
    AnyTypeBinder binder;
    const Node node = any_node("anything");

    // Zero particles is not a run, with or without an identity: there is no state to step.
    {
        execution::GraphRun run;
        REQUIRE_FALSE(run.prepare(node, {&binder}, rt::RunId{1}, 0).has_value());
        REQUIRE_FALSE(run.prepare(node, {&binder}, rt::RunId{}, 0).has_value());
    }

    // An unknown identity is accepted -- `execution.loop.run_can_be_named_after_binding` is where that is
    // tested -- but `set_run` refuses one, so a caller cannot erase a named run's trace by passing a
    // default-constructed id.
    {
        execution::GraphRun run;
        REQUIRE(run.prepare(node, {&binder}, rt::RunId{1}).has_value());
        run.set_run(rt::RunId{});
        REQUIRE(run.trace().run() == rt::RunId{1});
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
