/**
 * @file test_run_controller.cpp
 * @brief Tests for the window's Run action.
 *
 * ## What is worth testing here
 *
 * The controller is a thin layer -- find a node, bind it, step it, report -- so the interesting
 * properties are the **decisions** it makes around the loop:
 *
 *   - a graph with nothing runnable is refused with a sentence that distinguishes "empty" from "nothing
 *     can run this", because those are different problems for the user;
 *   - the ledger gets an entry **only** when there is something to run, so it does not fill with runs
 *     that produced nothing -- a ledger nobody can read is worse than none;
 *   - the run id in the report matches the trace's, so the numbers can be looked up;
 *   - damping is reported as "cannot run", not approximated.
 *
 * The controller needs the mechanics binder, so this is the one file in this directory that includes a
 * plugin header. It lives here all the same, and the reason is the contract gate rather than taste: the
 * gate judges test ownership **per invocation**, and the invocation that can see these contracts is the
 * one reading `views/model` with `--only model`. A file under `unit/plugins/` is invisible to it, so
 * every `@tests` id in `run_controller.hpp` dangled while these cases were reported as orphans by the
 * plugin partition -- both correct, and both pointing at the file's path rather than at its content.
 *
 * ## Why the fixture is constructed in place
 *
 * `authoring::Session` is non-copyable and non-movable -- by design, since it owns the one graph and the
 * one undo stack and a second copy is the defect it exists to prevent. So the fixture cannot be built by
 * a factory that returns one by value, and the shape here is a constructor that takes what varies. The
 * compiler rejected the first version, which is the type doing its job one layer out from the code that
 * matters.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/run_controller.hpp>

#include <qp/plugins/mechanics/mechanics_binder.hpp>

#include <qp/graph/execution/execution.hpp>
#include <qp/graph/mutate/command.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace qp::views::model;

namespace {

namespace rt = qp::runtime;

/// @brief The binder list every case uses. A function so no case leaves state behind.
[[nodiscard]] std::vector<qp::graph::execution::IOperatorBinder*> binders() {
    static qp::plugins::mechanics::MechanicsBinder binder;
    return {&binder};
}

/// @brief What the fixture session should contain.
enum class Shape {
    /// No nodes at all.
    empty,
    /// One spring-damper node with no parameters: what the demo graph looked like before its seed set them.
    unparameterised,
    /// One spring-damper node with the four parameters a run needs.
    ready,
};

/// @brief A session holding a graph, built through the command bus.
///
/// Through the bus rather than by writing the graph directly, so the fixture takes the same path the
/// window's seed does. One that assembled `Node` objects by hand could produce a shape the real code
/// never creates -- and a run against such a node would prove nothing about the real one.
class Fixture final {
public:
    /// @brief `omega = sqrt(k/m) = 20 rad/s` for the defaults, matching `RunController::kOmega`.
    Fixture(Shape shape, double k = 200.0, double m = 0.5, double c = 0.0,
            std::int64_t integrator = 1) {
        if (shape == Shape::empty) return;
        const auto reserved = session_.reserve_node();
        REQUIRE(reserved.has_value());
        node_ = reserved.value();
        const auto added = session_.apply(qp::graph::AddNode{node_, "demo.spring_damper", "n1"});
        REQUIRE(added.has_value());
        if (shape == Shape::unparameterised) return;
        set(2, qp::ports::Value{k});
        set(3, qp::ports::Value{c});
        set(4, qp::ports::Value{m});
        set(5, qp::ports::Value{integrator});
    }

    [[nodiscard]] const qp::authoring::Session& session() const noexcept { return session_; }

    void set(qp::graph::PortNumber port, qp::ports::Value value) {
        const auto applied = session_.apply(qp::graph::SetParam{node_, port, std::move(value)});
        REQUIRE(applied.has_value());
    }

private:
    qp::authoring::Session session_{};
    qp::graph::NodeId node_{};
};

}  // namespace

TEST_CASE("run.controller.refuses_a_graph_with_nothing_to_run", "[run]") {
    // An empty graph and a graph nothing can run are **different** problems, and the sentences say which
    // one the user has. "Nothing to run" alone would leave someone staring at three nodes wondering why.
    {
        Fixture fixture{Shape::empty};
        RunController controller{fixture.session(), binders()};
        const RunResult result = controller.run();
        REQUIRE_FALSE(result.report.ok);
        REQUIRE(result.report.message.find("empty") != std::string::npos);
        REQUIRE(result.trace.empty());
    }
    {
        // A node exists but has no parameters: the binder requires `k`, `m`, `c` and `integrator`, and a
        // node nobody has filled in is a node it cannot run. This is what the Run button reported on its
        // first press, and it was the right answer to an incomplete demo.
        //
        // Note which sentence this is. The binder *recognises the type* (`can_bind` is true, the layout
        // is the one it wants), so this is not the "no node in this graph has an operator" case: it is
        // "the operator exists and cannot honour this node". Reporting the first would tell the user to
        // install a plugin they already have.
        Fixture fixture{Shape::unparameterised};
        RunController controller{fixture.session(), binders()};
        const RunResult result = controller.run();
        REQUIRE_FALSE(result.report.ok);
        REQUIRE(result.report.message.find("cannot run demo.spring_damper") != std::string::npos);
        REQUIRE(result.report.message.find("no node") == std::string::npos);
        REQUIRE(result.trace.empty());
        // Refused before the ledger was touched, so nothing is recorded as having run.
        REQUIRE(controller.ledger().size() == 0);
    }
    {
        // No binders at all: a build with no plugins. Still a sentence rather than a crash.
        Fixture fixture{Shape::ready};
        RunController controller{fixture.session(), {}};
        const RunResult result = controller.run();
        REQUIRE_FALSE(result.report.ok);
        REQUIRE(result.trace.empty());
    }

    // The ledger gains nothing from a refused run. A ledger full of entries that produced nothing is a
    // report nobody can read, and the count a supervisor looks at would be wrong.
    {
        Fixture fixture{Shape::empty};
        RunController controller{fixture.session(), binders()};
        for (int i = 0; i < 3; ++i) (void)controller.run();
        REQUIRE(controller.ledger().size() == 0);
    }
}

TEST_CASE("run.controller.damping_is_reported_not_hidden", "[run]") {
    // The kernel integrates `x'' = -omega^2 x` and has no damping term. A node saying `c = 0.5` cannot be
    // honoured, and the message names the node type and the two settings that caused it rather than
    // reporting an error code -- the useful thing for a user to know is *which node* and *what to change*.
    Fixture fixture{Shape::ready, 200.0, 0.5, /*c=*/0.5};
    RunController controller{fixture.session(), binders()};
    const RunResult result = controller.run();

    REQUIRE_FALSE(result.report.ok);
    REQUIRE(result.report.message.find("demo.spring_damper") != std::string::npos);
    REQUIRE(result.report.message.find("damping") != std::string::npos);
    REQUIRE(result.trace.empty());
    // Nothing was run, so nothing is recorded as having run.
    REQUIRE(controller.ledger().size() == 0);

    // An unimplemented integrator is reported the same way, for the same reason: the user chose `verlet`
    // to get different behaviour, and running RK4 would contradict the choice silently.
    Fixture verlet{Shape::ready, 200.0, 0.5, 0.0, /*integrator=*/2};
    RunController other{verlet.session(), binders()};
    REQUIRE_FALSE(other.run().report.ok);
}

TEST_CASE("run.controller.runs_a_node_and_records_its_trace", "[run]") {
    // The end-to-end case: a parameterised node produces a trace with an identity the ledger issued, and
    // the frequency the node's `k` and `m` imply is the one the trajectory shows.
    //
    // `k = 200`, `m = 0.5` gives `omega = 20 rad/s`, which is `RunController::kOmega` -- so the state's
    // frequency component and the kernel's parameter block agree, and a disagreement would show up here
    // as a trajectory at the wrong period.
    Fixture fixture{Shape::ready};
    RunController controller{fixture.session(), binders()};
    const RunResult result = controller.run();

    REQUIRE(result.report.ok);
    REQUIRE(result.report.operator_name == "rk4_oscillator");
    REQUIRE(result.report.node_type == "demo.spring_damper");
    REQUIRE(result.report.steps == RunController::kSteps);
    REQUIRE(result.report.samples == RunController::kSteps + 1);
    REQUIRE(result.trace.size() == RunController::kSteps + 1);

    // The trace carries an identity the ledger issued. A trace whose `RunId` nobody issued is a record
    // that cannot be looked up, which defeats the point of recording it.
    REQUIRE(result.report.run.valid());
    REQUIRE(result.trace.run() == result.report.run);
    REQUIRE(controller.ledger().size() == 1);
    REQUIRE(controller.ledger().find(result.report.run) != nullptr);

    // The channels are the ones the panels look for by name.
    REQUIRE(result.trace.channel_count() == 2);
    REQUIRE(result.trace.channels()[0].name == qp::graph::execution::GraphRun::kPositionChannel);
    REQUIRE(result.trace.channels()[1].name == qp::graph::execution::GraphRun::kVelocityChannel);

    // Energy is conserved to RK4's accuracy at this step size, which is the property the confidence panel
    // reports. A drift here would mean the run loop or the kernel had changed.
    const double omega = RunController::kOmega;
    const auto energy_at = [omega](const rt::Sample& sample) {
        const double x = sample.values[0].value;
        const double v = sample.values[1].value;
        return 0.5 * v * v + 0.5 * omega * omega * x * x;
    };
    const double e0 = energy_at(result.trace.samples().front());
    const double e1 = energy_at(result.trace.samples().back());
    REQUIRE(e0 > 0.0);
    REQUIRE(std::abs((e1 - e0) / e0) < 1.0e-9);

    // And the initial condition is in the record, which is what makes the drift measurable at all.
    REQUIRE(result.trace.samples().front().t == 0.0);
    REQUIRE(result.trace.samples().front().values[0].value ==
            RunController::kInitialDisplacement);
}

TEST_CASE("run.controller.a_second_run_is_a_second_entry", "[run]") {
    // Each press of Run is its own run: its own id, its own trace. A controller that reused an id would
    // make two experiments share a record, and the ledger could no longer say which numbers came from
    // which configuration.
    Fixture fixture{Shape::ready};
    RunController controller{fixture.session(), binders()};

    const RunResult first = controller.run();
    REQUIRE(first.report.ok);
    const RunResult second = controller.run();
    REQUIRE(second.report.ok);

    REQUIRE(first.report.run != second.report.run);
    REQUIRE(controller.ledger().size() == 2);
    REQUIRE(first.trace.run() != second.trace.run());

    // Same graph, same seed, same numbers -- charter R2's claim made checkable at this layer.
    REQUIRE(first.trace.size() == second.trace.size());
    REQUIRE(first.trace.samples().back().values[0].value ==
            second.trace.samples().back().values[0].value);
    REQUIRE(first.trace.samples().back().values[1].value ==
            second.trace.samples().back().values[1].value);
}

TEST_CASE("run.controller.description_states_what_it_will_do", "[run]") {
    // A button that does not say what it will do is a button that surprises. The description names the
    // step count and size, and those are the two numbers that decide whether the answer is right.
    Fixture fixture{Shape::ready};
    RunController controller{fixture.session(), binders()};
    const std::string text = controller.description();

    REQUIRE_FALSE(text.empty());
    REQUIRE(text.find(std::to_string(RunController::kSteps)) != std::string::npos);
    REQUIRE(text.find(std::to_string(RunController::kDt)) != std::string::npos);

    // The product of the two, so an edit to either constant shows up in the sentence.
    REQUIRE(text.find(std::to_string(RunController::kSteps * RunController::kDt)) !=
            std::string::npos);
}
