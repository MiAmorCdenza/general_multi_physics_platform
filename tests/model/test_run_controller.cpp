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
#include <qp/host/host.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/emitter.hpp>
#include <qp/plugins/magnetosphere/pusher.hpp>
#include <qp/plugins/magnetosphere/run.hpp>

#include <qp/views/model/run_controller.hpp>
#include <qp/views/model/run_providers.hpp>

#include <qp/runtime/run/run.hpp>

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

    /// @brief The ledger the runs are recorded in: the **fixture's**, not the controller's.
    ///
    /// Handing it over is what the cases below assert from the outside -- a run that landed in a ledger
    /// this fixture cannot see would be a run whose record no reporter of this session would find.
    [[nodiscard]] qp::runtime::RunLedger& ledger() noexcept { return ledger_; }

private:
    qp::authoring::Session session_{};
    qp::runtime::RunLedger ledger_{};
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

TEST_CASE("run.controller.a_content_graph_runs_through_the_provider", "[run]") {
    // **The application's second run path, asserted end to end.** `run_controller.cpp` asks the operator path first
    // and the mounted providers after it declines; the shell mounts `MagnetosphereRunProvider` and the kit's node
    // types, and pressing Run on the flagship graph is what a user does with the whole kit. Until this existed,
    // nothing tested that path through *this* layer: `test_magnetosphere_run.cpp` drives the provider directly, so a
    // controller that never asked it -- or a shell that never mounted it -- would have left every case green. That is
    // not hypothetical here: the analysis plugin was missing from the Qt build for many rounds behind a
    // configure-time guard, and three cases asserting a fit passed by asserting its absence.
    //
    // The graph is built through the command bus, the way the window's Demos menu builds one.
    qp::host::PluginHost host{qp::plugin::Capability::node_types | qp::plugin::Capability::field_domain |
                              qp::plugin::Capability::particle_domain};
    using qp::plugins::magnetosphere::FieldNodes;
    using qp::plugins::magnetosphere::PusherNodes;
    using qp::plugins::magnetosphere::EmitterNodes;
    REQUIRE(FieldNodes::mount(host) == 17);
    REQUIRE(EmitterNodes::mount(host) == 1);
    REQUIRE(PusherNodes::mount(host) == 3);

    qp::authoring::Session session;
    // The id is taken the way the fixture above takes it: checked, then read -- a `Result` is not dereferenced with
    // `operator*` in this codebase, and the value accessor is the one that reports the check.
    const auto add = [&session](const char* type, const char* name) {
        const auto reserved = session.reserve_node();
        REQUIRE(reserved.has_value());
        const qp::graph::NodeId id = reserved.value();
        REQUIRE(session.apply(qp::graph::AddNode{id, type, name}).has_value());
        return id;
    };
    const auto put = [&session](qp::graph::NodeId id, qp::graph::PortNumber port, qp::ports::Value value) {
        REQUIRE(session.apply(qp::graph::SetParam{id, port, std::move(value)}).has_value());
    };

    // A small but complete kit: a dipole to bake, an emitter launched against it, and a Boris push. The flagship
    // blueprint adds the tail, the envelope and the shielding; none of that changes *which path* runs, and a case
    // that says what it means is worth more than one that repeats the flagship.
    const double re = qp::plugins::magnetosphere::kEarthRadiusM;
    const qp::graph::NodeId dipole = add("field.dipole", "dipole");
    for (qp::graph::PortNumber axis = 0; axis < 3; ++axis) {
        put(dipole, static_cast<qp::graph::PortNumber>(FieldNodes::kPortOrigin0 + axis),
            qp::ports::Value{-8.0 * re});
        put(dipole, static_cast<qp::graph::PortNumber>(FieldNodes::kPortSpacing0 + axis),
            qp::ports::Value{0.25 * re});
        put(dipole, static_cast<qp::graph::PortNumber>(FieldNodes::kPortCount0 + axis),
            qp::ports::Value{65.0});
    }
    const qp::graph::NodeId emitter = add("particle.ring_emitter", "emitter");
    put(emitter, EmitterNodes::kPortCount, qp::ports::Value{std::int64_t{16}});
    const qp::graph::NodeId pusher = add("particle.boris", "push");
    // The same three wires the kit's own case draws: the field into both the emitter (so it can compute a pitch
    // angle) and the pusher, and the emitter's state into the pusher's state socket.
    put(pusher, PusherNodes::kPortMaxRangeRe, qp::ports::Value{20.0});

    const auto wire = [&session](qp::graph::NodeId from, qp::graph::PortNumber out, qp::graph::NodeId to,
                                 qp::graph::PortNumber in) {
        REQUIRE(session.apply(qp::graph::Connect{
                    qp::graph::PortRef{from, out, qp::graph::PortDirection::output},
                    qp::graph::PortRef{to, in, qp::graph::PortDirection::input}})
                    .has_value());
    };
    wire(dipole, FieldNodes::kPortField, emitter, EmitterNodes::kPortMagnetic);
    wire(dipole, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);
    wire(emitter, EmitterNodes::kPortState, pusher, PusherNodes::kPortStateIn);

    qp::runtime::RunLedger ledger;
    const qp::graph::ResolveContext resolve{&host.node_types(), &qp::ports::builtin_registry()};

    // ---- Without the mount the graph is *refused*: this is the failure the mount prevents ---------------------
    {
        RunController controller{session, ledger, binders(), resolve};
        const RunResult result = controller.run();
        REQUIRE_FALSE(result.report.ok);
        REQUIRE(result.trace.empty());
        // ... and the ledger stays empty, because a refused run is not an event.
        REQUIRE(ledger.size() == 0);
    }

    // ---- With it, the run happens and the record is where the panels read it ----------------------------------
    {
        qp::plugins::magnetosphere::MagnetosphereRunProvider provider;
        qp::views::model::mount_run_provider(&provider);

        RunController controller{session, ledger, binders(), resolve};
        const RunResult result = controller.run();
        // The provider's own name is what the status line shows, so a user can tell which path ran.
        REQUIRE(result.report.ok);
        REQUIRE(result.report.operator_name == std::string{"magnetosphere"});
        REQUIRE(result.report.run.valid());
        REQUIRE(result.report.samples > 0);
        // The particles are where the particle scene reads them: sixteen triples.
        REQUIRE(result.particle_positions.size() == 16 * 3);
        // The field is the snapshot the field-lines item traces, under the dipole's own key.
        REQUIRE(result.fields.contains(qp::graph::field::FieldKey{dipole.index, FieldNodes::kPortField}));

        // **The trace's channels are this layer's contract with the rest of the window**: the measurement panel
        // reads one of them, and their names are what a report quotes. `radius` is the channel a length measurement
        // reads, and it is in metres because the session's dimension is SI.
        const qp::runtime::Trace& trace = result.trace;
        REQUIRE(trace.channel_count() == 4);
        bool has_radius = false;
        for (const qp::runtime::Channel& channel : trace.channels()) {
            if (channel.name == "radius") {
                has_radius = true;
                REQUIRE(channel.dim == qp::units::dims::length);
            }
        }
        REQUIRE(has_radius);
        REQUIRE(trace.size() > 0);

        // The ledger entry exists and carries the id the trace's samples point back into -- the chain a reading's
        // provenance follows.
        REQUIRE(ledger.size() == 1);
        REQUIRE(ledger.find(result.report.run) != nullptr);

        qp::views::model::clear_run_providers();
        REQUIRE(qp::views::model::run_providers().empty());
    }
}

TEST_CASE("run.controller.refuses_a_graph_with_nothing_to_run", "[run]") {
    // An empty graph and a graph nothing can run are **different** problems, and the sentences say which
    // one the user has. "Nothing to run" alone would leave someone staring at three nodes wondering why.
    {
        Fixture fixture{Shape::empty};
        RunController controller{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
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
        RunController controller{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
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
        RunController controller{fixture.session(), fixture.ledger(), {}, fixture.resolve()};
        const RunResult result = controller.run();
        REQUIRE_FALSE(result.report.ok);
        REQUIRE(result.trace.empty());
    }

    // The ledger gains nothing from a refused run. A ledger full of entries that produced nothing is a
    // report nobody can read, and the count a supervisor looks at would be wrong.
    {
        Fixture fixture{Shape::empty};
        RunController controller{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
        for (int i = 0; i < 3; ++i) (void)controller.run();
        REQUIRE(controller.ledger().size() == 0);
    }
}

TEST_CASE("run.controller.damping_is_reported_not_hidden", "[run]") {
    // The kernel integrates `x'' = -omega^2 x` and has no damping term. A node saying `c = 0.5` cannot be
    // honoured, and the message names the node type and the two settings that caused it rather than
    // reporting an error code -- the useful thing for a user to know is *which node* and *what to change*.
    Fixture fixture{Shape::ready, 200.0, 0.5, /*c=*/0.5};
    RunController controller{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
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
    RunController other{verlet.session(), verlet.ledger(), binders(), verlet.resolve()};
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
    RunController controller{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
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

    RunController controller{fixture.session(), fixture.ledger(), binders(), blind};
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
    RunController informed{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
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
    RunController controller{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};

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
    RunController controller{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
    const std::string text = controller.description();

    REQUIRE_FALSE(text.empty());
    REQUIRE(text.find(std::to_string(RunController::kSteps)) != std::string::npos);
    REQUIRE(text.find(std::to_string(RunController::kDt)) != std::string::npos);

    // The product of the two, so an edit to either constant shows up in the sentence.
    REQUIRE(text.find(std::to_string(RunController::kSteps * RunController::kDt)) !=
            std::string::npos);
}


namespace {

/// @brief A run that reports two particles, counts its steps, and records a one-channel trace.
class CountingRun final : public qp::graph::execution::IGraphRun {
public:
    [[nodiscard]] qp::diag::Result<void> advance(std::size_t steps, double dt) override {
        if (!(dt > 0.0)) return qp::diag::ErrorCode::invalid_argument;
        // **One sample per step, not one per call.** `advance` takes a step *count* and a caller passes the whole
        // run's worth of them, so a run that recorded once per call would hand back a one-sample "time series" --
        // which is what the first version of this stub did, and the case that asserts `size() == kSteps` is what
        // said so. A real run's `advance` loops, and so does the stub that stands in for one.
        for (std::size_t step = 0; step < steps; ++step) {
            ++steps_;
            if (recording_) {
                (void)trace_.append(static_cast<double>(steps_) * dt,
                                    {qp::runtime::UncertainValue::measured(1.5, 0.0, qp::units::dims::length)});
            }
        }
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
    void set_run(qp::runtime::RunId run) noexcept override {
        trace_ = qp::runtime::Trace{run};
        recording_ = trace_.add_channel(
                         qp::runtime::Channel{"radius", qp::units::dims::length, {}})
                         .has_value();
    }
    [[nodiscard]] const qp::runtime::Trace& trace() const noexcept override { return trace_; }

private:
    std::size_t steps_ = 0;
    qp::runtime::Trace trace_{qp::runtime::RunId{}};
    bool recording_ = false;
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
    // Three ledgers rather than one, so that a run landing in the wrong one is visible: each controller is
    // handed a different ledger and the counts below say which of them each run reached.
    qp::runtime::RunLedger alone_ledger{};
    qp::runtime::RunLedger provided{};
    qp::runtime::RunLedger refused_ledger{};

    clear_run_providers();
    RunController alone{fixture.session(), alone_ledger, {}, fixture.resolve()};
    const RunResult refused = alone.run();
    REQUIRE_FALSE(refused.report.ok);
    REQUIRE(refused.report.message.find("no node") != std::string::npos);
    REQUIRE(refused.particle_positions.empty());
    REQUIRE(alone_ledger.size() == 0);

    // With one mounted: the report is filled from the provider's own report, and the snapshot -- the one thing
    // a canvas reads -- comes through `particle_positions`.
    mount_run_provider(&provider);
    RunController with_provider{fixture.session(), provided, {}, fixture.resolve()};
    const RunResult ran = with_provider.run();
    REQUIRE(ran.report.ok);
    REQUIRE(ran.report.steps == RunController::kSteps);
    REQUIRE(ran.report.operator_name == std::string{"stub"});
    REQUIRE(ran.report.message == std::string{"counting run"});
    REQUIRE(ran.particle_positions.size() == 6);
    REQUIRE(ran.particle_positions.front() == 1.0);
    REQUIRE(ran.particle_positions.back() == 6.0);
    REQUIRE(provided.size() == 1);
    REQUIRE(provided.find(ran.report.run) != nullptr);

    // A refusal passes the provider's own sentence through unchanged: this layer has no vocabulary for "a
    // magnetic socket nobody wired", and inventing one would be worse than repeating the kit's.
    provider.refuse = true;
    RunController declining{fixture.session(), refused_ledger, {}, fixture.resolve()};
    const RunResult declined = declining.run();
    REQUIRE_FALSE(declined.report.ok);
    REQUIRE(declined.report.message == std::string{"nothing to bake"});
    REQUIRE(declined.particle_positions.empty());
    REQUIRE(refused_ledger.size() == 0);

    // The list is a process-wide static: a case that left an entry behind would change every later case's
    // answer, so the leak is cleaned up by the case that made it.
    clear_run_providers();
    REQUIRE(run_providers().empty());
}

TEST_CASE("run.controller.a_provider_run_records_under_the_ledgers_identity", "[run]") {
    // **The chain the whole platform is for, asserted at the seam that was missing.** A provider's run records a
    // trace; the controller opens the ledger entry, hands the identity over *before* the first step, and copies
    // the record out. Without this, a run of the flagship kit produced a report and a picture and nothing a
    // measurement could be taken from -- which is what the window showed: "no readings yet", and correctly so.
    Fixture fixture{Shape::ready};
    StubProvider provider;
    clear_run_providers();
    mount_run_provider(&provider);

    RunController controller{fixture.session(), fixture.ledger(), {}, fixture.resolve()};
    const RunResult ran = controller.run();
    REQUIRE(ran.report.ok);

    // The identity is the ledger's, it is **valid**, and it is the same one the record carries: a reading taken
    // from this trace names the run that produced it, and the ledger is where that name can be looked up. An id
    // nobody issued would make the trace a record that cannot be found.
    REQUIRE(ran.report.run.valid());
    REQUIRE(ran.trace.run() == ran.report.run);
    REQUIRE(controller.ledger().find(ran.report.run) != nullptr);
    REQUIRE(ran.report.samples == ran.trace.size());
    REQUIRE(ran.trace.size() == RunController::kSteps);

    // The channels and the samples are **copied out**, not borrowed: the run is destroyed when `run()` returns,
    // and the view layer reads this afterwards -- the same rule positions and fields follow.
    REQUIRE(ran.trace.channels().size() == 1);
    REQUIRE(ran.trace.channels().front().name == std::string{"radius"});
    REQUIRE(ran.trace.channels().front().dim == qp::units::dims::length);
    // The first sample is one step in, at the controller's own step size: the time axis starts where the run
    // started rather than at a wall clock, which is what makes a trace comparable between two runs.
    REQUIRE(ran.trace.samples().front().t == RunController::kDt);
    const auto first = ran.trace.value_at(0, 0);
    REQUIRE(first.has_value());
    REQUIRE(first->value == 1.5);

    // And the ordering is observable, which is why it is asserted: `set_run` **rebuilds** the trace, so a
    // controller that named the run after advancing it would hand back an empty record. A non-empty one here is
    // the proof that the identity arrived before the first sample did -- the same order the operator path
    // documents for the ledger, where binding comes before anything is written down.
    REQUIRE_FALSE(ran.trace.empty());

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
    RunController plain{fixture.session(), fixture.ledger(), binders(), fixture.resolve()};
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
    qp::runtime::RunLedger ledger{};
    RunController controller{session, ledger, {}, resolve};
    const RunResult result = controller.run();
    REQUIRE(result.render_declared.size() == 1);
    REQUIRE(result.render_declared.front().node == reserved.value());
    REQUIRE(result.render_declared.front().port == 1);
    // Declared and not run: the refusal is unchanged, because a render node is not an operator.
    REQUIRE_FALSE(result.report.ok);
}

TEST_CASE("run.controller.one_ledger_for_the_caller", "[run]") {
    // **The defect this pins was read off a screenshot of the running window.** A magnetosphere run drew
    // twenty-four particles and nested field lines on the panels while the status line beside them said
    // `runs 0 | reproducibility gaps: no run yet`. Both statements were true about their own object: the
    // controller kept a ledger of its own and the window counted the one it holds. A run ledger is the record
    // of what happened in a session, so two of them means the session has two histories -- the same
    // two-sources-of-truth failure the authoring layer exists to prevent, one layer up.
    //
    // The assertion is on the **identity** of the ledger. Equality of contents would also pass for a copy that
    // happened to match, and a copy is the bug.
    Fixture fixture{Shape::ready};
    qp::runtime::RunLedger mine{};
    RunController controller{fixture.session(), mine, binders(), fixture.resolve()};
    REQUIRE(&controller.ledger() == &mine);

    const RunResult first = controller.run();
    REQUIRE(first.report.ok);
    // The run is in the caller's ledger, and it is the run the report names -- not merely one of the same size.
    REQUIRE(mine.size() == 1);
    REQUIRE(mine.find(first.report.run) != nullptr);

    // A second controller over the same ledger continues the same numbering rather than starting again: ids are
    // issued by the ledger, so two controllers sharing one is what makes "which run was that" have one answer.
    RunController second{fixture.session(), mine, binders(), fixture.resolve()};
    const RunResult after = second.run();
    REQUIRE(after.report.ok);
    REQUIRE(mine.size() == 2);
    REQUIRE(after.report.run != first.report.run);
    REQUIRE(after.report.run.value > first.report.run.value);

    // And a controller handed a **different** ledger records there instead, which is what makes the identity
    // above a distinction rather than two objects that happen to be empty together.
    qp::runtime::RunLedger other{};
    RunController elsewhere{fixture.session(), other, binders(), fixture.resolve()};
    REQUIRE(elsewhere.run().report.ok);
    REQUIRE(other.size() == 1);
    REQUIRE(mine.size() == 2);
}
