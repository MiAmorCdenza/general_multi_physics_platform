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
#include <qp/graph/execution/run_provider.hpp>

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>

#include <cmath>
#include <limits>
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

    /// @brief What this stub declares: an exact rotation, and nothing it has not been asked to be.
    ///
    /// The rotation is a pure function of `(state, dt)`, its inverse is itself with `-dt` to within rounding,
    /// it acts on one state and touches no shared buffer. It is **not** declared dimensionally consistent:
    /// it deliberately mixes `x` and `v/omega` in one expression, so the claim would be false. The timid
    /// answers in `SimModelDesc` are what make saying "no" possible.
    [[nodiscard]] execution::SimModelDesc describe() const noexcept override {
        execution::SimModelDesc desc;
        desc.is_pure = true;
        desc.time_reversible = true;
        // Zero: the rotation's inverse is itself with `-dt`, so the round trip lands bit-identically. Claiming
        // a tolerance here would weaken a claim this stub can actually keep -- and the test that checks it
        // asserts the observed error is within whatever is declared, so a wrong number fails the case.
        desc.round_trip_tolerance = 0.0;
        desc.is_independent_of_other_instances = true;
        return desc;
    }

    [[nodiscard]] qp::diag::Result<void> step(execution::StateView& state, double dt) override {
        if (!state.is_consistent()) return qp::diag::ErrorCode::invalid_argument;
        // Non-zero and finite, which is the step contract rather than a convenience: a zero step would record a
        // duplicate sample for no reason, and a non-finite one is not a step. **Negative is legal** -- that is
        // what makes the round trip in `execution.model.a_time_reversible_operator_round_trips` a check rather
        // than a claim, and the first version of this stub accepted zero while the case asserted it refused.
        if (!std::isfinite(dt) || dt == 0.0) return qp::diag::ErrorCode::invalid_argument;
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

    /// @brief No claim at all: this stub exists to raise, and a model that raises declares nothing.
    [[nodiscard]] execution::SimModelDesc describe() const noexcept override { return {}; }

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

/// @brief A binder for a model whose state is **not** the mechanics family's three components.
///
/// It exists because that is what the loop could not express. `GraphRun::prepare` and `check_run` used to build
/// the layout themselves -- `StateView::zeroed(particles)`, whose component count is three -- so every binder was
/// asked about a shape this one does not accept and declined, and a two- or four-component model was reported as
/// having nothing to run and could not be prepared at all. The defect was latent while the only operator in the
/// tree was the three-component oscillator.
class TwoComponentBinder final : public execution::IOperatorBinder {
public:
    static constexpr std::size_t kComponents = 2;

    [[nodiscard]] bool can_bind(std::string_view type_name,
                                const execution::StateView& layout) const noexcept override {
        // The type **and** the layout, which is the whole point: a binder is asked with the layout it will be
        // handed, so it can say no instead of reading a component nothing writes.
        return type_name == kType && layout.components_per_particle == kComponents;
    }

    [[nodiscard]] std::unique_ptr<execution::IStateOperator> bind(
        std::string_view type_name, const Node&, const execution::StateView& layout) override {
        if (!can_bind(type_name, layout)) return nullptr;
        return std::make_unique<CounterOperator>();
    }

    static constexpr const char* kType = "test.two_component";

private:
    /// @brief Advances component 0 by `dt` and leaves component 1 alone: enough to see that it ran.
    class CounterOperator final : public execution::IStateOperator {
    public:
        [[nodiscard]] std::string_view name() const noexcept override { return "counter"; }
        [[nodiscard]] execution::SimModelDesc describe() const noexcept override { return {}; }
        [[nodiscard]] qp::diag::Result<void> step(execution::StateView& state, double dt) override {
            for (std::size_t i = 0; i < state.count; ++i) {
                state.set(i, 0, state.at(i, 0) + dt);
            }
            return {};
        }
    };
};

}  // namespace

TEST_CASE("execution.loop.records_the_initial_condition", "[execution]") {
    // The first sample is taken **before** the first step, so a trace of `n` steps holds `n + 1`
    // samples. Without it the initial state is unrecoverable from the record, and the confidence panel
    // measures its energy drift against exactly that state -- a trace that began after step one would
    // have nothing to measure against.
    AnyTypeBinder binder;
    execution::GraphRun run;
    const Node node = any_node("anything");

    REQUIRE(run.prepare(node, {&binder}, execution::StateView::zeroed(1), rt::RunId{7}).has_value());
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

    REQUIRE(run.prepare(node, {&binder}, execution::StateView::zeroed(1), rt::RunId{1}).has_value());
    REQUIRE(run.set_initial(0, 1.0, 0.0).has_value());
    run.set_omega(2.0);
    REQUIRE(run.run(100, 1.0e-3).ok());
    REQUIRE(run.trace().size() == 101);

    REQUIRE(run.prepare(node, {&binder}, execution::StateView::zeroed(1), rt::RunId{2}).has_value());
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
    REQUIRE(run.prepare(node, {&binder}, execution::StateView::zeroed(1), rt::RunId{}).has_value());
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
    const auto prepared = refused.prepare(any_node("mine.but.unusable"), {&unusable}, execution::StateView::zeroed(1), rt::RunId{1});
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
    RunReadiness empty = check_run(graph, ctx, {&binder}, execution::StateView::zeroed(1));
    REQUIRE_FALSE(empty.ok());
    REQUIRE(empty.refusal == RunRefusal::empty_graph);
    REQUIRE_FALSE(empty.detail.empty());

    const auto added = graph.add_node("anything");
    REQUIRE(added.has_value());

    // A node whose type no binder claims: the graph is not empty, and this build cannot run it. The two
    // answers are different sentences because they are different problems.
    RunReadiness none = check_run(graph, ctx, {&refuses}, execution::StateView::zeroed(1));
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
    RunReadiness declined = check_run(unusable, unusable_ctx, {&declines}, execution::StateView::zeroed(1));
    REQUIRE_FALSE(declined.ok());
    REQUIRE(declined.refusal == RunRefusal::node_cannot_be_honoured);
    REQUIRE(declined.type_name == "mine.but.unusable");
    REQUIRE(declined.node == unusable_node.value());
    REQUIRE(declined.detail.find("mine.but.unusable") != std::string::npos);

    // And the answer a caller acts on: which node, which operator, and that nothing was stepped.
    RunReadiness ready = check_run(graph, ctx, {&binder}, execution::StateView::zeroed(1));
    REQUIRE(ready.ok());
    REQUIRE(ready.node == added.value());
    REQUIRE(ready.type_name == "anything");
    REQUIRE(ready.operator_name == "test.rotation");
    REQUIRE(ready.detail.find("test.rotation") != std::string::npos);
    // Nothing stepped: the graph is exactly as it was, with no trace anywhere.
    REQUIRE(graph.node_count() == 1);
}

TEST_CASE("execution.check_run.asks_each_binder_with_the_layout_it_will_use", "[execution]") {
    // **The defect this case exists for.** Both `check_run` and `GraphRun::prepare` used to build the layout
    // themselves, at three components per particle -- the mechanics family's shape, which was the only shape in
    // the tree when the loop was written. So a model whose state is two components or four was asked about a
    // layout it does not accept, declined, and was reported as "nothing to run" even though a binder in the list
    // claims it. `plugins/models`' pendulum is two components and its projectile is four, which is what surfaced
    // it.
    //
    // The case asserts the property rather than the fix: the layout a run will use reaches the binder, and the
    // answer changes with it. A test that only checked "a two-component model now runs" would pass for an
    // implementation that special-cased two.
    TwoComponentBinder binder;
    const Node node = any_node(TwoComponentBinder::kType);
    const std::vector<execution::IOperatorBinder*> binders{&binder};

    qp::graph::Graph graph;
    REQUIRE(graph.add_node_named(TwoComponentBinder::kType, "counter").has_value());

    const ResolveContext ctx{nullptr, nullptr};

    // With the layout the model declares, the graph is runnable and prepares.
    const execution::StateView matching = execution::StateView::zeroed(1, TwoComponentBinder::kComponents);
    const execution::RunReadiness ready = check_run(graph, ctx, binders, matching);
    REQUIRE(ready.ok());
    REQUIRE(ready.node.valid());

    execution::GraphRun loop;
    REQUIRE(loop.prepare(node, binders, matching, rt::RunId{1}).has_value());
    REQUIRE(loop.operator_name() == "counter");

    // With any other layout the binder declines, and both functions say so. Three is the case that used to be
    // hardcoded, so it is the one worth naming.
    for (const std::size_t components : {std::size_t{1}, std::size_t{3}, std::size_t{4}}) {
        const execution::StateView wrong = execution::StateView::zeroed(1, components);
        INFO("components " << components);
        const execution::RunReadiness refused = check_run(graph, ctx, binders, wrong);
        REQUIRE_FALSE(refused.ok());
        REQUIRE(refused.refusal == execution::RunRefusal::no_operator);

        execution::GraphRun other;
        const auto prepared = other.prepare(node, binders, wrong, rt::RunId{1});
        REQUIRE_FALSE(prepared.has_value());
        REQUIRE(prepared.error() == qp::diag::ErrorCode::not_implemented);
    }

    // And the operator really runs at its own shape: one step of 0.5 advances the first component by 0.5 and
    // leaves the second alone, which is only possible if the state handed over has two of them.
    execution::GraphRun stepped;
    REQUIRE(stepped.prepare(node, binders, matching, rt::RunId{2}).has_value());
    REQUIRE(stepped.set_initial(0, 0.0, 0.0).has_value());
    REQUIRE(stepped.run(1, 0.5).ok());
    REQUIRE(stepped.state().components_per_particle == TwoComponentBinder::kComponents);
    REQUIRE(std::abs(stepped.state().at(0, 0) - 0.5) < 1.0e-12);
    REQUIRE(stepped.state().at(0, 1) == 0.0);

    // An **empty** layout is refused rather than defaulted, and that is a correction: there used to be a
    // `particles` argument beside the layout, so a caller could pass `StateView{}` and have the loop fill in a
    // mechanics-shaped state of that many particles. Two numbers for one thing, with the layout silently winning
    // whenever it named a count -- and the losing number was the one a test asserted about, which is how a
    // parameter becomes impossible to argue with. There is one number now and it is the caller's.
    AnyTypeBinder accepts_anything;
    execution::GraphRun defaulted;
    const auto nothing = defaulted.prepare(any_node("anything"), {&accepts_anything}, execution::StateView{},
                                           rt::RunId{1});
    REQUIRE_FALSE(nothing.has_value());
    REQUIRE(nothing.error() == qp::diag::ErrorCode::invalid_argument);
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

    const RunReadiness readiness = check_run(graph, ctx, {&binder}, execution::StateView::zeroed(1));
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
    REQUIRE(run.prepare(any_node("demo.raising"), {&raising}, execution::StateView::zeroed(1), rt::RunId{7}).has_value());
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
        // A layout with no particles is a caller mistake rather than a default to be filled in: StateView{}
        // is an empty state, and "run this over nothing" is not a request. This used to be expressed by passing
        // particles = 0 beside a non-empty layout -- and that stopped being expressible when the separate
        // count was removed, which is exactly why it was removed: two numbers for one thing, one of them
        // silently winning, and a test that could no longer fail.
        REQUIRE_FALSE(run.prepare(node, {&binder}, execution::StateView{}, rt::RunId{1}).has_value());
        REQUIRE_FALSE(run.prepare(node, {&binder}, execution::StateView{}, rt::RunId{}).has_value());
        // And a layout that claims a size its own values do not have is refused for the same reason.
        execution::StateView ragged = execution::StateView::zeroed(2, 3);
        ragged.values.pop_back();
        REQUIRE_FALSE(ragged.is_consistent());
        REQUIRE_FALSE(run.prepare(node, {&binder}, ragged, rt::RunId{1}).has_value());
    }

    // An unknown identity is accepted -- `execution.loop.run_can_be_named_after_binding` is where that is
    // tested -- but `set_run` refuses one, so a caller cannot erase a named run's trace by passing a
    // default-constructed id.
    {
        execution::GraphRun run;
        REQUIRE(run.prepare(node, {&binder}, execution::StateView::zeroed(1), rt::RunId{1}).has_value());
        run.set_run(rt::RunId{});
        REQUIRE(run.trace().run() == rt::RunId{1});
    }

    // No binder claims the type: `not_implemented`, so the caller can name what it could not run
    // rather than reporting a generic failure.
    {
        execution::GraphRun run;
        NoTypeBinder none;
        const auto prepared = run.prepare(node, {&none}, execution::StateView::zeroed(1), rt::RunId{1});
        REQUIRE_FALSE(prepared.has_value());
        REQUIRE(prepared.error() == qp::diag::ErrorCode::not_implemented);
        REQUIRE_FALSE(run.is_ready());
    }

    // A null entry is skipped rather than dereferenced: a caller assembling binders from optional
    // plugins will have holes, and a later binder still gets its turn.
    {
        execution::GraphRun run;
        NamedTypeBinder specific{"wanted.type"};
        REQUIRE(run.prepare(any_node("wanted.type"), {nullptr, &specific}, execution::StateView::zeroed(1), rt::RunId{1}).has_value());
        REQUIRE(run.is_ready());
    }

    // A binder answers for one name and declines another.
    {
        NamedTypeBinder specific{"wanted.type"};
        execution::GraphRun declined;
        REQUIRE_FALSE(declined.prepare(any_node("other.type"), {&specific}, execution::StateView::zeroed(1), rt::RunId{1}).has_value());

        execution::GraphRun accepted;
        REQUIRE(accepted.prepare(any_node("wanted.type"), {&specific}, execution::StateView::zeroed(1), rt::RunId{1}).has_value());
    }

    execution::GraphRun run;
    REQUIRE(run.prepare(node, {&binder}, execution::StateView::zeroed(1), rt::RunId{1}).has_value());

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
        REQUIRE(other.prepare(node, {&binder}, execution::StateView::zeroed(1), rt::RunId{2}).has_value());
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
    REQUIRE(run.prepare(any_node("anything"), {&binder}, execution::StateView::zeroed(1), rt::RunId{1}).has_value());
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

// ===========================================================================
// The simulation-model contract
// ===========================================================================
//
// `SimModelDesc` is four claims about a model. A claim nothing can falsify is a comment, so each case here
// makes one of them **checkable** -- and the case that matters most is the one where the claim is "no":
// `execution.model.a_time_reversible_operator_round_trips` is green only because the stub that declares a
// round trip takes one, and RK4's own refusal to declare it is asserted in the plugin partition.

TEST_CASE("execution.model.a_pure_step_is_replayable", "[execution]") {
    // A pure step is a function of `(state, dt)`: replaying it from the same input gives the same output, bit
    // for bit. That is what makes a recorded run reproducible, and it is falsifiable -- an operator that
    // advanced its own state instead of the argument's would fail here on the second call.
    execution::StateView first = execution::StateView::zeroed(2, 3);
    execution::StateView second = first;
    for (std::size_t i = 0; i < first.count; ++i) {
        first.set(i, 0, 1.0 + static_cast<double>(i));
        first.set(i, 2, 2.0);
    }
    second = first;

    RotationOperator op;
    REQUIRE(op.describe().is_pure);
    REQUIRE(op.step(first, 1.0e-3).has_value());
    REQUIRE(op.step(second, 1.0e-3).has_value());
    REQUIRE(first.values == second.values);

    // And again from the same input, which is the replay: a third state stepped from the *original* values
    // must equal the first one's result exactly. The comparison is on the whole buffer rather than on a
    // tolerance, because "reproducible" that holds only to within rounding is a different and weaker claim.
    execution::StateView replay = execution::StateView::zeroed(2, 3);
    for (std::size_t i = 0; i < replay.count; ++i) {
        replay.set(i, 0, 1.0 + static_cast<double>(i));
        replay.set(i, 2, 2.0);
    }
    REQUIRE(op.step(replay, 1.0e-3).has_value());
    REQUIRE(replay.values == first.values);

    // The stub that declares nothing is the other half: `RaisingOperator` returns a default-constructed
    // description, and every field of it is the timid answer. A report built from such a model claims
    // nothing, which is the point of not defaulting to "yes".
    RaisingOperator unknown;
    const execution::SimModelDesc none = unknown.describe();
    REQUIRE_FALSE(none.is_pure);
    REQUIRE_FALSE(none.time_reversible);
    REQUIRE_FALSE(none.is_dimensionally_consistent);
    REQUIRE_FALSE(none.is_independent_of_other_instances);
}

TEST_CASE("execution.model.a_time_reversible_operator_round_trips", "[execution]") {
    // The claim is exactly this: one `+dt` step followed by one `-dt` step returns to where it started. The
    // rotation stub is a good subject because it is genuinely self-inverse -- the same trigonometry with the
    // sign flipped -- so the round trip lands bit-identically and a zero tolerance is honest.
    RotationOperator op;
    const execution::SimModelDesc desc = op.describe();
    REQUIRE(desc.time_reversible);
    REQUIRE(desc.round_trip_tolerance == 0.0);

    execution::StateView state = execution::StateView::zeroed(3, 3);
    for (std::size_t i = 0; i < state.count; ++i) {
        state.set(i, 0, 0.5 * static_cast<double>(i + 1));
        state.set(i, 1, -0.25);
        state.set(i, 2, 1.5);
    }
    const std::vector<double> start = state.values;

    constexpr double kDt = 1.0e-3;
    REQUIRE(op.step(state, kDt).has_value());
    // Moved: a round trip that never left is not evidence of anything.
    REQUIRE(state.values != start);
    REQUIRE(op.step(state, -kDt).has_value());

    // The tolerance is relative to the state's own magnitude, and this stub declares **zero** because its step
    // is self-inverse -- the same trigonometry with the sign flipped.
    //
    // The comparison is against a floor rather than against literal zero, and the first version of this case
    // is why: it asserted `worst <= 0.0` and failed while Catch2 printed `0.0 <= 0.0`. The value was a
    // **subnormal**, printed as `0.0` because that is what six digits of a `1e-320` looks like. A declared
    // tolerance of zero therefore cannot be demanded bit-exactly -- `cos`/`sin` are not exactly invertible --
    // and the honest floor is a few units in the last place.
    const double floor_value = std::numeric_limits<double>::epsilon();
    double worst = 0.0;
    for (std::size_t i = 0; i < start.size(); ++i) {
        const double magnitude = std::max(std::abs(start[i]), 1.0);
        worst = std::max(worst, std::abs(state.values[i] - start[i]) / magnitude);
    }
    INFO("round-trip relative error " << worst);
    REQUIRE(worst <= desc.round_trip_tolerance + floor_value);

    // A scheme that is not reversible says so, and the declaration is the claim rather than a constant: the
    // other stub answers the other way, so the field carries information.
    RaisingOperator unknown;
    REQUIRE_FALSE(unknown.describe().time_reversible);

    // A zero step is not a step, and an operator has to refuse it rather than record a duplicate. Negative is
    // legal -- that is the whole point above -- so the guard cannot be `dt > 0`.
    REQUIRE_FALSE(op.step(state, 0.0).has_value());
}

TEST_CASE("execution.model.a_dimensionally_wrong_step_leaves_the_bound", "[execution]") {
    // "Dimensionally consistent" is not a property this platform can check by reading an expression. What it
    // can check is the consequence: a model whose arithmetic respects the units it declared also respects the
    // invariants those units imply, and one that does not drifts out of them.
    //
    // The subject is a harmonic oscillator's conserved energy, `E = (x^2 + (v/omega)^2) / 2`, for the
    // **velocity-Verlet** update `v += -omega^2*x*dt/2`, `x += v*dt`, `v += -omega^2*x*dt/2`. Every term is an
    // acceleration times a time or a velocity times a time, so the units multiply out and the energy stays.
    constexpr double kOmega = 2.0;
    constexpr double kDt = 5.0e-3;
    constexpr int kSteps = 200;
    const auto energy = [](const execution::StateView& s) {
        return 0.5 * (s.at(0, 0) * s.at(0, 0) + (s.at(0, 1) / kOmega) * (s.at(0, 1) / kOmega));
    };
    const auto seed = [] {
        execution::StateView s = execution::StateView::zeroed(1, 3);
        s.set(0, 0, 1.0);
        // A real velocity, so the state carries kinetic energy that a wrong update can lose: with
        // `|x| = 1` and `|v/omega| = 1` the invariant is exactly 1.0.
        s.set(0, 1, 2.0);
        s.set(0, 2, kOmega);
        return s;
    };

    execution::StateView correct = seed();
    const double initial = energy(correct);
    REQUIRE(initial > 0.0);
    for (int i = 0; i < kSteps; ++i) {
        correct.set(0, 1, correct.at(0, 1) - kOmega * kOmega * correct.at(0, 0) * (kDt / 2.0));
        correct.set(0, 0, correct.at(0, 0) + correct.at(0, 1) * kDt);
        correct.set(0, 1, correct.at(0, 1) - kOmega * kOmega * correct.at(0, 0) * (kDt / 2.0));
    }
    // The invariant is **bounded**, not exact, and the bound is the method's own: velocity-Verlet conserves a
    // nearby quantity and its energy error oscillates at second order, so `O(dt^2 * steps)` relative is the
    // honest expectation. A tight tolerance here would be asserting that a symplectic integrator is exact,
    // which is the kind of claim this whole descriptor exists to avoid making.
    REQUIRE(std::abs(energy(correct) - initial) <= 1.0e-4 * initial);

    // The wrong one: `x += v*dt` and nothing else, so a velocity is used as a displacement rate and never
    // updated. It is still a **pure, replayable, deterministic** function of `(state, dt)` -- which is why
    // dimension correctness has to be its own declaration instead of being inferred from purity -- and it
    // climbs straight out of the bound.
    execution::StateView wrong = seed();
    for (int i = 0; i < kSteps; ++i) {
        wrong.set(0, 0, wrong.at(0, 0) + wrong.at(0, 1) * kDt);
    }
    REQUIRE(std::abs(energy(wrong) - initial) > 1.0e-3 * initial);
    // And the two really did compute different things rather than the same thing twice.
    REQUIRE(wrong.at(0, 0) != correct.at(0, 0));
    // The wrong one is also **past the bound the oscillator's energy implies**, which is the sentence a report
    // can carry: a harmonic oscillator cannot leave `|x| <= sqrt(2E)/omega`, so a displacement beyond it is a
    // symptom rather than a state.
    REQUIRE(correct.at(0, 0) <= std::sqrt(2.0 * initial * 2.0) / kOmega + 1.0e-6);
}

TEST_CASE("execution.model.interleaved_instances_do_not_disturb_each_other", "[execution]") {
    // The property a file-scope cache, a `static` scratch buffer or a shared RNG would break, and the reason
    // the declaration exists at all: a run in one window must not change what a run in another computes.
    //
    // Interleaving is the test rather than two sequential runs, because two sequential runs both start from a
    // clean process state and a shared buffer that is overwritten on entry would pass. Alternating steps means
    // each operator's next call happens after the other one has run.
    RotationOperator first_op;
    RotationOperator second_op;
    REQUIRE(first_op.describe().is_independent_of_other_instances);
    REQUIRE(second_op.describe().is_independent_of_other_instances);

    const auto seeded = [](double x, double v, double omega) {
        execution::StateView s = execution::StateView::zeroed(1, 3);
        s.set(0, 0, x);
        s.set(0, 1, v);
        s.set(0, 2, omega);
        return s;
    };

    execution::StateView alone = seeded(1.0, 0.0, 2.0);
    for (int i = 0; i < 50; ++i) REQUIRE(first_op.step(alone, 1.0e-3).has_value());

    execution::StateView interleaved = seeded(1.0, 0.0, 2.0);
    execution::StateView other = seeded(-3.0, 0.5, 0.75);
    for (int i = 0; i < 50; ++i) {
        REQUIRE(first_op.step(interleaved, 1.0e-3).has_value());
        REQUIRE(second_op.step(other, 7.0e-3).has_value());
    }

    // Bit for bit, not to a tolerance: a shared buffer that happened to hold the same values would pass a
    // tolerance check, and the property is about isolation rather than about accuracy.
    REQUIRE(interleaved.values == alone.values);
}


namespace {

/// @brief A catalog holding one descriptor of a caller's choosing, for the domain-message case.
///
/// A local fixture rather than a new one in the shared scene, because the case is about what the *message* says
/// about a node of another domain and the scene's catalog has no domains to speak of.
class OneDescCatalog final : public qp::graph::INodeCatalog {
public:
    explicit OneDescCatalog(qp::graph::NodeDesc desc) : desc_(std::move(desc)) {}

    [[nodiscard]] const qp::graph::NodeDesc* find(std::string_view name) const noexcept override {
        return name == desc_.type_name ? &desc_ : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept override { return 1; }

private:
    qp::graph::NodeDesc desc_;
};

}  // namespace

TEST_CASE("execution.check_run.names_a_node_of_another_domain", "[execution]") {
    // **"No node has an operator" is true and unhelpful.** A graph holding a particle kit's nodes has registered
    // types, drawn wires, and a reason nothing runs that has nothing to do with a missing plugin: those nodes
    // belong to a domain whose state is a particle batch, driven by its own loop rather than by this one's
    // `StateView`. The sentence names the node and the domain, so the reader's next question is "what drives
    // that" rather than "which plugin is missing" -- and the two domains are named separately because a field
    // node has to be baked and a particle node has to be launched first, which are different next steps.
    NoTypeBinder refuses;

    qp::graph::NodeDesc pusher;
    pusher.type_name = "kit.pusher";
    pusher.has_compute = true;
    pusher.allow_in_field_domain = false;
    pusher.allow_in_particle_domain = true;
    OneDescCatalog particle_catalog{pusher};
    qp::graph::Graph particle_graph;
    REQUIRE(particle_graph.add_node("kit.pusher").has_value());
    const ResolveContext particle_ctx{&particle_catalog, &qp::ports::builtin_registry()};

    const RunReadiness particle = check_run(particle_graph, particle_ctx, {&refuses},
                                            execution::StateView::zeroed(1));
    REQUIRE_FALSE(particle.ok());
    REQUIRE(particle.refusal == RunRefusal::no_operator);
    REQUIRE(particle.detail.find("kit.pusher") != std::string::npos);
    REQUIRE(particle.detail.find("particle domain") != std::string::npos);

    qp::graph::NodeDesc field = pusher;
    field.type_name = "kit.field";
    field.allow_in_field_domain = true;
    field.allow_in_particle_domain = false;
    OneDescCatalog field_catalog{field};
    qp::graph::Graph field_graph;
    REQUIRE(field_graph.add_node("kit.field").has_value());
    const ResolveContext field_ctx{&field_catalog, &qp::ports::builtin_registry()};

    const RunReadiness baked = check_run(field_graph, field_ctx, {&refuses}, execution::StateView::zeroed(1));
    REQUIRE(baked.refusal == RunRefusal::no_operator);
    REQUIRE(baked.detail.find("field domain") != std::string::npos);

    // A description that is **not** computed keeps the general sentence: a node with no implementation is a
    // different problem (an unfinished palette entry, or a declaration-only render node), and naming a domain
    // for it would send the reader looking for a loop that was never the issue.
    qp::graph::NodeDesc inert = pusher;
    inert.type_name = "kit.inert";
    inert.has_compute = false;
    OneDescCatalog inert_catalog{inert};
    qp::graph::Graph inert_graph;
    REQUIRE(inert_graph.add_node("kit.inert").has_value());
    const ResolveContext inert_ctx{&inert_catalog, &qp::ports::builtin_registry()};
    const RunReadiness nothing = check_run(inert_graph, inert_ctx, {&refuses}, execution::StateView::zeroed(1));
    REQUIRE(nothing.refusal == RunRefusal::no_operator);
    REQUIRE(nothing.detail.find("no node") != std::string::npos);
}


namespace {

/// @brief A run that counts its steps, so the interface's own contract can be checked without a kit.
class StubRun final : public execution::IGraphRun {
public:
    [[nodiscard]] qp::diag::Result<void> advance(std::size_t steps, double dt) override {
        if (!(dt > 0.0)) return qp::diag::ErrorCode::invalid_argument;
        steps_ += steps;
        return {};
    }

    [[nodiscard]] execution::GraphRunReport report() const override {
        execution::GraphRunReport out;
        out.steps = steps_;
        out.particles = 2;
        out.live = 1;
        out.absorbed = 1;
        out.note = "stub";
        return out;
    }

    [[nodiscard]] std::vector<double> positions() const override { return {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}; }

private:
    std::size_t steps_ = 0;
};

/// @brief A provider that claims any non-empty graph and answers with `StubRun`, or with a sentence.
class StubProvider final : public execution::IGraphRunProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "stub"; }

    [[nodiscard]] bool claims(const qp::graph::Graph& graph) const noexcept override {
        return graph.node_count() > 0;
    }

    [[nodiscard]] execution::RunBuildResult build(const qp::graph::Graph&,
                                                  const qp::graph::INodeCatalog&) override {
        execution::RunBuildResult out;
        if (refuse) {
            out.refusal = "nothing to bake";
            return out;
        }
        out.run = std::make_unique<StubRun>();
        return out;
    }

    bool refuse = false;
};

/// @brief A run that baked something, so the *other* answer is checked too: "here are my tables".
class BakingRun final : public execution::IGraphRun {
public:
    BakingRun() {
        qp::abi::FieldDim tesla;
        tesla.M = 1;
        tesla.T = -2;
        tesla.I = -1;
        const qp::abi::LatticeDesc desc =
            qp::abi::make_lattice(qp::abi::LatticeKind::volume, qp::abi::ComponentKind::vector,
                                  qp::abi::ElementType::f64, tesla, 2, 2, 2);
        published = fields_.publish(qp::graph::field::FieldKey{5, 1}, desc, std::vector<double>(24, 0.25));
    }

    [[nodiscard]] qp::diag::Result<void> advance(std::size_t, double dt) override {
        if (!(dt > 0.0)) return qp::diag::ErrorCode::invalid_argument;
        return {};
    }
    [[nodiscard]] execution::GraphRunReport report() const override { return {}; }
    [[nodiscard]] std::vector<double> positions() const override { return {}; }
    [[nodiscard]] const qp::graph::field::FieldSet& fields() const noexcept override { return fields_; }

    /// Whether the bake went in, so a case can fail on *that* rather than on a later assertion.
    bool published = false;

private:
    qp::graph::field::FieldSet fields_{};
};

}  // namespace

TEST_CASE("execution.run_provider.a_run_with_no_field_answers_with_an_empty_set", "[execution]") {
    // `IGraphRun::fields()` exists because a render domain now **declares** "draw the field on this wire", and
    // the thing that reads a declaration is a view item, which has nowhere else to get samples from: the run owns
    // the store. The old shape's cost was written in the header together with the condition that would pay it,
    // so this case pins the two answers that condition created.
    //
    // The default is the interesting half. Most runs in this repository bake nothing -- the operator loop binds
    // one operator to one node and never enters the field domain -- and those runs must keep compiling, which is
    // why the member is virtual with a definition rather than pure. `StubRun` above inherits that default, so
    // what it answers here is what every such run answers.
    StubRun plain;
    const qp::graph::field::FieldSet& none = plain.fields();
    REQUIRE(none.size() == 0);
    REQUIRE(none.keys().empty());
    REQUIRE(none.bytes() == 0);

    // A run with no field and a run with the *wrong* field give the same answer, and that sameness is the point:
    // an item asks the store rather than checking for null, gets an unreadable value either way, and has one
    // branch for "the field is not there" instead of two that would have to agree.
    REQUIRE_FALSE(qp::graph::field::is_readable(none.view(qp::graph::field::FieldKey{5, 1})));
    REQUIRE_FALSE(qp::graph::field::is_readable(none.view(qp::graph::field::kNoField)));

    // **The default shares one object**, and this assertion is what makes that a decision rather than a
    // coincidence: a run that baked nothing answers with the same reference as any other such run, so the default
    // costs no allocation per run. Pinned here so that giving each run its own empty vector -- a defensible
    // change, since an empty `FieldSet` is a value -- has to be made deliberately and this line updated.
    StubRun another;
    REQUIRE(&plain.fields() == &another.fields());

    // And the other answer: a run that baked reports its own tables, not the shared empty one. The reference is
    // the run's member, so it is valid exactly as long as the run is -- which is why the view layer **copies** it
    // into its result rather than keeping the pointer.
    BakingRun baking;
    REQUIRE(baking.published);
    const qp::graph::field::FieldSet& baked = baking.fields();
    REQUIRE(&baked != &plain.fields());
    REQUIRE(baked.size() == 1);
    REQUIRE(baked.bytes() == 24 * sizeof(double));
    const qp::graph::field::FieldValue view = baked.view(qp::graph::field::FieldKey{5, 1});
    REQUIRE(qp::graph::field::is_readable(view));
    REQUIRE(view.point_count() == 8);
    REQUIRE(qp::graph::field::get_component(view, 3, 1) == 0.25);
    // The same store answers a key it does not hold with the unreadable value, which is the single branch above.
    REQUIRE_FALSE(qp::graph::field::is_readable(baked.view(qp::graph::field::FieldKey{9, 9})));
}

TEST_CASE("execution.run_provider.a_provider_names_its_own_refusal", "[execution]") {
    // The interface's own contract, checked with a stub rather than with a kit: the point is the **shape** --
    // a question asked before the work ("is this graph yours"), a build that may answer with a sentence, and a
    // run that reports and hands out a snapshot. A kit that got this wrong would still pass its own physics
    // tests, which is why the contract is pinned here.
    StubProvider provider;
    REQUIRE(provider.name() == std::string_view{"stub"});

    qp::graph::Graph graph;
    REQUIRE_FALSE(provider.claims(graph));

    const auto added = graph.add_node("anything");
    REQUIRE(added.has_value());
    REQUIRE(provider.claims(graph));

    execution::RunBuildResult built = provider.build(graph, OneTypeCatalog{"anything"});
    REQUIRE(built.ok());
    REQUIRE(built.refusal.empty());
    REQUIRE(built.run != nullptr);

    // The report is consistent before a step and stays consistent after one: `live + absorbed + escaped` equals
    // `particles` is the invariant a status line depends on, and it is the one thing a provider could plausibly
    // get wrong by counting a retirement twice.
    execution::GraphRunReport report = built.run->report();
    REQUIRE(report.is_consistent());
    REQUIRE(report.steps == 0);
    REQUIRE(report.note == std::string{"stub"});
    REQUIRE(built.run->positions().size() % 3 == 0);

    REQUIRE(built.run->advance(7, 0.01).has_value());
    REQUIRE(built.run->advance(3, 0.01).has_value());
    report = built.run->report();
    REQUIRE(report.steps == 10);
    REQUIRE(report.is_consistent());

    // A step that is not a step is refused, and the counters are untouched by the refusal: a run that counted a
    // step it did not take would report a duration it never simulated.
    REQUIRE_FALSE(built.run->advance(1, 0.0).has_value());
    REQUIRE(built.run->report().steps == 10);

    // A refusal is the provider's **own sentence**, passed through unchanged, and a refused build has no run.
    StubProvider refusing;
    refusing.refuse = true;
    const execution::RunBuildResult refused = refusing.build(graph, OneTypeCatalog{"anything"});
    REQUIRE_FALSE(refused.ok());
    REQUIRE(refused.run == nullptr);
    REQUIRE(refused.refusal == std::string{"nothing to bake"});
}
