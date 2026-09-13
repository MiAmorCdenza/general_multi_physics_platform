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
#include <qp/views/model/run_providers.hpp>

#include <qp/views/model/demo_library.hpp>
#include <qp/graph/ir/node_type_registry.hpp>

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

    /// @brief The catalog and port registry the framework pre-flight validates against.
    ///
    /// The built-in demonstrator library, because it is the same catalog the window gives the Run action --
    /// a test that invented one would validate a graph against types the real application does not have.
    [[nodiscard]] qp::graph::ResolveContext resolve() const noexcept {
        return qp::graph::ResolveContext{&catalog_, &qp::ports::builtin_registry()};
    }

    void set(qp::graph::PortNumber port, qp::ports::Value value) {
        const auto applied = session_.apply(qp::graph::SetParam{node_, port, std::move(value)});
        REQUIRE(applied.has_value());
    }

private:
    qp::authoring::Session session_{};
    qp::graph::NodeTypeRegistry catalog_{};
    qp::graph::NodeId node_{};
    /// Registers the demonstrator types, so `graph/validate` has something to check the fixture's node against
    /// rather than reporting an unknown type for every case.
    struct Register {
        explicit Register(qp::graph::NodeTypeRegistry& c) { (void)qp::views::register_demo_library(c); }
    };
    Register registered_{catalog_};
};

}  // namespace

TEST_CASE("run.controller.refuses_a_graph_with_nothing_to_run", "[run]") {
    // An empty graph and a graph nothing can run are **different** problems, and the sentences say which
    // one the user has. "Nothing to run" alone would leave someone staring at three nodes wondering why.
    {
        Fixture fixture{Shape::empty};
        RunController controller{fixture.session(), binders(), fixture.resolve()};
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
        RunController controller{fixture.session(), binders(), fixture.resolve()};
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
        RunController controller{fixture.session(), {}, fixture.resolve()};
        const RunResult result = controller.run();
        REQUIRE_FALSE(result.report.ok);
        REQUIRE(result.trace.empty());
    }

    // The ledger gains nothing from a refused run. A ledger full of entries that produced nothing is a
    // report nobody can read, and the count a supervisor looks at would be wrong.
    {
        Fixture fixture{Shape::empty};
        RunController controller{fixture.session(), binders(), fixture.resolve()};
        for (int i = 0; i < 3; ++i) (void)controller.run();
        REQUIRE(controller.ledger().size() == 0);
    }
}

TEST_CASE("run.controller.damping_is_reported_not_hidden", "[run]") {
    // The kernel integrates `x'' = -omega^2 x` and has no damping term. A node saying `c = 0.5` cannot be
    // honoured, and the message names the node type and the two settings that caused it rather than
    // reporting an error code -- the useful thing for a user to know is *which node* and *what to change*.
    Fixture fixture{Shape::ready, 200.0, 0.5, /*c=*/0.5};
    RunController controller{fixture.session(), binders(), fixture.resolve()};
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
    RunController other{verlet.session(), binders(), verlet.resolve()};
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
    RunController controller{fixture.session(), binders(), fixture.resolve()};
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

TEST_CASE("run.controller.reports_validation_without_refusing", "[run]") {
    // The pre-flight validates as well as searches, and its findings travel with a **successful** run. A graph
    // whose types the catalog does not know still runs -- the kernels do not check dimensions at run time --
    // and the numbers it produces look exactly like correct ones. A status line that reported only "ran ...
    // 4097 samples" would be telling the user everything is fine about a graph the framework already knows is
    // inconsistent.
    Fixture fixture{Shape::ready};

    // A catalog that knows nothing: every node in the graph is an unknown type, which is a validation error and
    // is still runnable, because the binder claims the type regardless of what the catalog says.
    qp::graph::NodeTypeRegistry empty_catalog{};
    const qp::graph::ResolveContext blind{&empty_catalog, &qp::ports::builtin_registry()};

    RunController controller{fixture.session(), binders(), blind};
    const RunResult result = controller.run();

    REQUIRE(result.report.ok);
    REQUIRE(result.report.samples == RunController::kSteps + 1);
    REQUIRE(result.report.validation_errors > 0);
    REQUIRE_FALSE(result.report.first_problem.empty());
    // The message says so, in the same sentence the status line shows: the run happened, and here is what is
    // wrong with the graph it happened on.
    REQUIRE(result.report.message.find("validation problem") != std::string::npos);
    REQUIRE(result.report.message.find("ran rk4_oscillator") != std::string::npos);

    // And with a catalog that knows the demonstrator types, the same graph validates cleanly: the findings are
    // measurements of the graph, not a standing complaint about the fixture.
    RunController informed{fixture.session(), binders(), fixture.resolve()};
    const RunResult clean = informed.run();
    REQUIRE(clean.report.ok);
    REQUIRE(clean.report.validation_errors == 0);
    REQUIRE(clean.report.first_problem.empty());
    REQUIRE(clean.report.message.find("validation problem") == std::string::npos);
}
TEST_CASE("run.controller.a_second_run_is_a_second_entry", "[run]") {
    // Each press of Run is its own run: its own id, its own trace. A controller that reused an id would
    // make two experiments share a record, and the ledger could no longer say which numbers came from
    // which configuration.
    Fixture fixture{Shape::ready};
    RunController controller{fixture.session(), binders(), fixture.resolve()};

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
    RunController controller{fixture.session(), binders(), fixture.resolve()};
    const std::string text = controller.description();

    REQUIRE_FALSE(text.empty());
    REQUIRE(text.find(std::to_string(RunController::kSteps)) != std::string::npos);
    REQUIRE(text.find(std::to_string(RunController::kDt)) != std::string::npos);

    // The product of the two, so an edit to either constant shows up in the sentence.
    REQUIRE(text.find(std::to_string(RunController::kSteps * RunController::kDt)) !=
            std::string::npos);
}


namespace {

/// @brief A run that reports two particles and counts its steps, so the controller's second path is observable.
class CountingRun final : public qp::graph::execution::IGraphRun {
public:
    [[nodiscard]] qp::diag::Result<void> advance(std::size_t steps, double dt) override {
        if (!(dt > 0.0)) return qp::diag::ErrorCode::invalid_argument;
        steps_ += steps;
        return {};
    }
    [[nodiscard]] qp::graph::execution::GraphRunReport report() const override {
        qp::graph::execution::GraphRunReport out;
        out.steps = steps_;
        out.particles = 2;
        out.live = 2;
        out.note = "counting run";
        return out;
    }
    [[nodiscard]] std::vector<double> positions() const override { return {1.0, 2.0, 3.0, 4.0, 5.0, 6.0}; }

private:
    std::size_t steps_ = 0;
};

/// @brief A provider that claims any non-empty graph, and refuses on demand.
class StubProvider final : public qp::graph::execution::IGraphRunProvider {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "stub"; }
    [[nodiscard]] bool claims(const qp::graph::Graph& graph) const noexcept override {
        return graph.node_count() > 0;
    }
    [[nodiscard]] qp::graph::execution::RunBuildResult build(const qp::graph::Graph&,
                                                             const qp::graph::INodeCatalog&) override {
        qp::graph::execution::RunBuildResult out;
        if (refuse) {
            out.refusal = "nothing to bake";
            return out;
        }
        out.run = std::make_unique<CountingRun>();
        return out;
    }
    bool refuse = false;
};

}  // namespace

TEST_CASE("views.binders.a_provider_runs_a_graph_the_operators_declined", "[run]") {
    // **The second path, and the ordering that makes it safe.** The provider list is consulted only after the
    // operator loop has declined, so a graph both could run keeps the answer it had before providers existed.
    // The graph here is the fixture's spring-damper -- a type an operator *would* claim -- handed **no
    // binders**, which is the same shape the "nothing to run" case uses. The two cases therefore differ in
    // exactly the thing under test: one mounted provider.
    Fixture fixture{Shape::ready};
    StubProvider provider;

    clear_run_providers();
    RunController alone{fixture.session(), {}, fixture.resolve()};
    const RunResult refused = alone.run();
    REQUIRE_FALSE(refused.report.ok);
    REQUIRE(refused.report.message.find("no node") != std::string::npos);
    REQUIRE(refused.particle_positions.empty());

    // With one mounted: the report is filled from the provider's own report, and the snapshot -- the one thing
    // a canvas reads -- comes through `particle_positions`.
    mount_run_provider(&provider);
    RunController with_provider{fixture.session(), {}, fixture.resolve()};
    const RunResult ran = with_provider.run();
    REQUIRE(ran.report.ok);
    REQUIRE(ran.report.steps == RunController::kSteps);
    REQUIRE(ran.report.operator_name == std::string{"stub"});
    REQUIRE(ran.report.message == std::string{"counting run"});
    REQUIRE(ran.particle_positions.size() == 6);
    REQUIRE(ran.particle_positions.front() == 1.0);
    REQUIRE(ran.particle_positions.back() == 6.0);

    // A refusal passes the provider's own sentence through unchanged: this layer has no vocabulary for "a
    // magnetic socket nobody wired", and inventing one would be worse than repeating the kit's.
    provider.refuse = true;
    RunController declining{fixture.session(), {}, fixture.resolve()};
    const RunResult declined = declining.run();
    REQUIRE_FALSE(declined.report.ok);
    REQUIRE(declined.report.message == std::string{"nothing to bake"});
    REQUIRE(declined.particle_positions.empty());

    // The list is a process-wide static: a case that left an entry behind would change every later case's
    // answer, so the leak is cleaned up by the case that made it.
    clear_run_providers();
    REQUIRE(run_providers().empty());
}


namespace {

/// @brief A catalog holding a declaration-only type, so the render list has something to find.
class DeclarationCatalog final : public qp::graph::INodeCatalog {
public:
    DeclarationCatalog() {
        desc_.type_name = "render.stub_item";
        desc_.has_compute = false;
        desc_.allow_in_field_domain = false;
        desc_.allow_in_particle_domain = false;
        qp::graph::PortDesc out;
        out.number = 1;
        out.name = "item";
        out.type = qp::ports::kParticleBuffer;
        desc_.outputs.push_back(out);
    }
    [[nodiscard]] const qp::graph::NodeDesc* find(std::string_view name) const noexcept override {
        return name == desc_.type_name ? &desc_ : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept override { return 1; }

private:
    qp::graph::NodeDesc desc_{};
};

}  // namespace

TEST_CASE("run.controller.carries_what_the_graph_wants_drawn", "[run]") {
    // The render declarations need a producer and had none: `ViewRequest` is handed the render plan's own list,
    // and until now nothing in the view layer computed a plan -- the operator loop works from one node and the
    // run providers work from the graph themselves. So the controller computes it, and this case is what says
    // it does.
    Fixture fixture{Shape::ready};
    RunController plain{fixture.session(), binders(), fixture.resolve()};
    const RunResult without = plain.run();
    // The fixture's graph is a spring-damper: step content, not a declaration, so there is nothing to draw and
    // the list is empty rather than missing. **This assertion is what found the defect**: the first rule was
    // "no compute", and the demonstrator library's types leave that flag defaulted, so a graph with nothing to
    // draw reported one thing to draw.
    REQUIRE(without.render_declared.empty());

    // A graph holding a declaration-only node: it lands in the list, with the node's own output port as the
    // name of the thing being declared, and it is **not** something the operator loop will run -- the run still
    // refuses for the reason it did before.
    qp::authoring::Session session{};
    const auto reserved = session.reserve_node();
    REQUIRE(reserved.has_value());
    const auto added = session.apply(qp::graph::AddNode{reserved.value(), "render.stub_item", "item"});
    REQUIRE(added.has_value());
    DeclarationCatalog catalog;
    const qp::graph::ResolveContext resolve{&catalog, &qp::ports::builtin_registry()};
    RunController controller{session, {}, resolve};
    const RunResult result = controller.run();
    REQUIRE(result.render_declared.size() == 1);
    REQUIRE(result.render_declared.front().node == reserved.value());
    REQUIRE(result.render_declared.front().port == 1);
    // Declared and not run: the refusal is unchanged, because a render node is not an operator.
    REQUIRE_FALSE(result.report.ok);
}
