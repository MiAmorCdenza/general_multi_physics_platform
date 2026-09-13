/**
 * @file run_controller.cpp
 * @brief Finds a runnable node, runs it, and reports what happened in words.
 */
#include <qp/views/model/run_controller.hpp>

#include <cmath>
#include <qp/views/model/run_providers.hpp>
#include <qp/graph/domain/build.hpp>
#include <cstdint>
#include <string>
#include <utility>

namespace qp::views::model {
namespace {

namespace execution = qp::graph::execution;
namespace rt = qp::runtime;

/// @brief The seed every run in this window uses.
///
/// A fixed value rather than the clock, because charter R2 makes reproduction mean *the same seed*: a
/// run seeded from a clock is an anecdote, and the window would then produce a different trace each
/// time the button is pressed with nothing in the graph having changed. When the UI grows a field for
/// it, this becomes the default.
constexpr std::uint64_t kSeed = 20240517;


}  // namespace

RunController::RunController(const qp::authoring::Session& session,
                             std::vector<execution::IOperatorBinder*> binders,
                             qp::graph::ResolveContext resolve)
    : session_(&session), binders_(std::move(binders)), resolve_(resolve) {}

std::string RunController::description() const {
    // Stated in the same sentence the status line shows, so the numbers are never a surprise after the
    // fact. `kSteps * kDt` rather than a computed duration, so an edit to either constant shows up here.
    return "Run: " + std::to_string(kSteps) + " steps of " + std::to_string(kDt) + " s (" +
           std::to_string(kSteps * kDt) + " s simulated)";
}

rt::RunSpec RunController::spec_for() const {
    rt::RunSpec spec;
    spec.seed = kSeed;
    spec.graph_version = session_->graph().version();
    spec.toolchain = rt::toolchain_id();
    spec.optimisation = rt::optimisation_id();
    spec.started_at = rt::now_unix_seconds();
    // The same four fields the window's seeded run pins, and no more. A run from this button cannot
    // vouch for its plugin versions because this build loads no plugins at run time, and it records no
    // parameter set because none has been chosen -- so those stay absent and the ledger's own gap
    // report says so rather than the controller pretending otherwise.
    spec.fields = rt::ReproField::seed | rt::ReproField::graph_version |
                  rt::ReproField::toolchain | rt::ReproField::optimisation;
    return spec;
}

RunResult RunController::run() const {
    RunResult out;

    // -- 1. ask before doing ---------------------------------------------------
    //
    // The search and the refusal both belong to `graph/execution::check_run`, which is where the loop that
    // would do the work lives. This used to be a hand-rolled search here -- the framework function written
    // once in the view layer and not at all in the framework -- and the difference is not tidiness: the
    // framework version also asks `graph/validate`, so a caller can see the graph's problems *before* waiting
    // for a run rather than after.
    // The layout the run will use, and it is **not** a constant any more. `GraphRun::prepare` and `check_run`
    // used to build a mechanics-shaped layout of three components per particle, so a model whose state is two
    // components or four could never be prepared: every binder was asked about a shape it does not accept and
    // declined. That was latent while the only operator in the tree was the three-component oscillator, and
    // `plugins/models`' pendulum (two) and projectile (four) are what surfaced it.
    //
    // The count is derived exactly the way `check_run` derives its own: ask each binder which layout it will
    // take for the node, since the model owns its state shape and the caller cannot know it. `4` is the search
    // ceiling -- the widest layout anything in this repository declares -- and a model needing more would raise
    // it in one place.
    const auto layout_for = [this](const qp::graph::Node& node) {
        for (std::size_t components = 1; components <= 4; ++components) {
            const execution::StateView probe = execution::StateView::zeroed(1, components);
            for (graph::execution::IOperatorBinder* binder : binders_) {
                if (binder != nullptr && binder->can_bind(node.type_name, probe)) return probe;
            }
        }
        return execution::StateView::zeroed(1);
    };

    const qp::graph::Graph& graph = session_->graph();

    // -- 0. What the graph wants drawn ---------------------------------------------------------------
    //
    // A **declaration** is a node with no compute: `graph/domain`'s render domain is exactly that, and
    // `build_plan` puts such a node into `plan.render.declared` instead of into an evaluation plan. The wanted
    // set is therefore "every declaration-only node, on its first output port" -- the port is the node's own
    // name for the thing it declares, and a node that declares nothing cannot be asked for.
    //
    // Computed before the run rather than after it, and left in place even when the run is refused: what a
    // graph declares is a property of the graph, not of this run.
    {
        qp::graph::Declarations wanted;
        for (const qp::graph::NodeSlot& slot : graph.slots()) {
            if (!slot.occupied) continue;
            const qp::graph::NodeDesc* desc =
                resolve_.catalog == nullptr ? nullptr : resolve_.catalog->find(slot.node.type_name);
            // Three conditions, and the case for this feature is what separated them. A node is a drawing
            // declaration when it **has no compute** (nothing evaluates it), is **not bake content** and is
            // **not step content** -- that is `graph/domain`'s render domain, spelled out. The first version
            // asked only for "no compute", which swept in every node type whose descriptor left the flag
            // defaulted: the demonstrator library's spring-damper among them, so a graph with nothing to draw
            // reported one thing to draw.
            if (desc == nullptr || desc->has_compute || desc->outputs.empty()) continue;
            if (desc->allow_in_field_domain || desc->allow_in_particle_domain) continue;
            wanted.add(qp::graph::DeclaredOutput{slot.node.id, desc->outputs.front().number});
        }
        const qp::graph::ExecutionPlan declared_plan =
            qp::graph::build_plan(graph, qp::graph::PlanContext{resolve_.catalog}, wanted);
        out.render_declared = declared_plan.render.declared();
    }
    const execution::RunReadiness ready = execution::check_run(graph, resolve_, binders_,
                                                               execution::StateView::zeroed(1));
    if (!ready.ok()) {
        // **The second kind of run.** `GraphRun` drives one operator over a state of a few doubles per particle;
        // a graph whose *setup* is a computation -- a field to bake, particles to launch -- is run by **content**
        // instead, through a provider the application mounted. Asked only here, after the operator path has
        // declined, so a graph both could run keeps the answer it had before providers existed.
        for (execution::IGraphRunProvider* provider : run_providers()) {
            if (provider == nullptr || !provider->claims(graph)) continue;
            execution::RunBuildResult built = provider->build(graph, *resolve_.catalog);
            if (!built.ok()) {
                // The provider's own sentence, passed through unchanged: it names what is missing, and this
                // layer has no vocabulary for "a magnetic socket nobody wired".
                out.report.message = built.refusal;
                return out;
            }
            // **The record, and the identity that makes it findable.** The ledger entry is opened *here*, after the
            // build succeeded and before the first step, which is the order the operator path argues for: a
            // refused build is not an event, so an entry that exists always has a run that really executed. The id
            // is handed to the run before it steps, because a trace's samples are only lookups if somebody issued
            // the identity they carry -- and for this kit that chain is the platform's whole point: the trace is
            // what the measurement session reads, the ledger is what a reading's provenance points back into, and
            // the uncertainty follows from there.
            const qp::runtime::RunId id = ledger_.begin(spec_for());
            built.run->set_run(id);
            // **The run's own cadence, when it has one.** The two constants on this class were measured against a
            // laboratory oscillator, and applying them to a magnetosphere makes a run 8.7 milliseconds long --
            // one seventy-third of a proton's gyration, which is a run about nothing. A run that knows its own
            // fastest time scale states it; one with no opinion keeps this class's numbers, which is why the
            // operator path is unchanged.
            const std::optional<execution::RunCadence> cadence = built.run->preferred_cadence();
            const std::size_t steps = cadence.has_value() ? cadence->steps : kSteps;
            const double dt = cadence.has_value() ? cadence->dt : kDt;
            const auto advanced = built.run->advance(steps, dt);
            if (!advanced.has_value()) {
                out.report.message = std::string{provider->name()} + " refused a step";
                return out;
            }
            const execution::GraphRunReport summary = built.run->report();
            out.report.ok = true;
            out.report.steps = summary.steps;
            out.report.samples = built.run->trace().size();
            out.report.operator_name = std::string{provider->name()};
            out.report.run = id;
            out.report.message = summary.note;
            out.particle_positions = built.run->positions();
            // **Copied, not borrowed, and by the rule already applied one line above.** The run that owns the
            // samples is destroyed when this function returns, so a view item holding a pointer into it would be
            // reading freed memory on the next repaint. Positions are copied for exactly that reason; the field
            // is the same snapshot for the field half of a drawing, and the price is one memcpy of what the bake
            // already allocated -- measured at 6.6 MB for this kit's default 65^3 dipole grid, against the 6.6 MB
            // the bake itself had just allocated. Keeping the run alive in the controller instead would trade
            // that one copy per Run press for the same bytes resident for the life of the window, and it would
            // make `run()` return a value that borrows from the controller.
            out.fields = built.run->fields();
            // The record, copied by the same rule and for the same reason. Unlike the fields this one is small --
            // four doubles a step for four thousand steps is a hundred and thirty kilobytes -- and unlike the
            // positions it is what the **measurement chain** reads: the window hands this trace to the measurement
            // session, so a run of this kit finally produces readings, an uncertainty and a report. Before this
            // line the platform's flagship experiment produced no trace at all, which meant no measurements, no
            // confidence report and no provenance for either.
            out.trace = built.run->trace();
            return out;
        }
        out.report.message = ready.detail;
        out.report.node_type = ready.type_name;
        return out;
    }
    out.report.node_type = ready.type_name;
    // Recorded on the success path only. `RunReadiness::node` is documented as invalid for `empty_graph` and
    // `no_operator`, so copying it before the refusal check would publish an id that means "none" as though it
    // meant "this one" -- and a caller stamping provenance from it would attribute a reading to node 0.
    out.report.node = ready.node;

    const qp::graph::Node* candidate = graph.find_node(ready.node);
    if (candidate == nullptr) {
        // The graph changed between the answer and its use. Refused rather than run: a node that is gone is not
        // a node to run, and continuing would be running whatever now occupies that slot.
        out.report.message = "the graph changed while the run was being prepared";
        return out;
    }

    // -- 2. bind, before anything is written down ------------------------------
    //
    // Binding comes first because a refused bind is not an event. Opening the ledger entry first would
    // record a run that never executed, and a ledger whose gaps cite it as present is worse than one
    // that admits the gap -- the whole point of the record is that a missing entry means something.
    execution::GraphRun loop;
    const auto prepared = loop.prepare(*candidate, binders_, layout_for(*candidate), rt::RunId{});
    if (!prepared.has_value()) {
        // The type is supported -- `can_bind` said so, which is why this node was chosen -- but no
        // operator can honour *this instance*: a parameter is missing, damping the kernel has no term
        // for, or an integrator with no implementation. Named as the node type rather than as an error
        // code, because the useful thing for the user to know is which node and which settings, and the
        // three causes are listed rather than guessed at: the binder does not report which one it was.
        out.report.message = "cannot run " + candidate->type_name +
                             ": no operator can honour this node as configured (check its parameters, "
                             "damping and integrator settings)";
        return out;
    }
    out.report.operator_name = loop.operator_name();

    // -- 3. open the run, now that there will be a trace to put in it ----------
    //
    // The id has to exist before the trace does, or the trace would carry an identity nobody issued and
    // could not be looked up -- which defeats the point of recording it. `set_run` is what starts the
    // trace, and it is called only here.
    const rt::RunId run_id = ledger_.begin(spec_for());
    if (!run_id.valid()) {
        out.report.message = "could not open a run in the ledger";
        return out;
    }
    out.report.run = run_id;
    loop.set_run(run_id);

    // -- 4. seed and step -----------------------------------------------------

    // Initial condition: displaced, at rest. That is the condition a lab oscillator is released from,
    // and it makes the closed form `x(t) = x0 cos(omega t)` -- so a student comparing the trace against
    // a prediction does not have to solve for the phase first.
    if (!loop.set_initial(0, kInitialDisplacement, 0.0).has_value()) {
        out.report.message = "internal: could not set the initial state";
        return out;
    }
    loop.set_omega(kOmega);

    const execution::RunOutcome stepped = loop.run(kSteps, kDt);
    out.report.steps = stepped.steps;
    out.report.samples = loop.trace().size();
    out.trace = loop.trace();
    out.report.ok = stepped.ok();

    if (!stepped.ok()) {
        // A partial trace is returned rather than discarded. The confidence panel exists to make a
        // diverging run visible, and it cannot do that from an empty trace.
        out.report.message = "the run stopped after " + std::to_string(stepped.steps) +
                             " steps: the operator refused a step (see the log for the code)";
        return out;
    }

    out.report.message = "ran " + out.report.operator_name + " on " + candidate->type_name + " for " +
                         std::to_string(kSteps) + " steps, recorded " +
                         std::to_string(out.report.samples) + " samples";

    // The pre-flight's validation findings travel with the outcome even on success, and the message says so.
    // The run produced numbers; these are the reasons to distrust them, and a status line that reported only
    // "ran ... 4097 samples" would be telling the user everything is fine about a graph the framework already
    // knows is inconsistent. Reported rather than enforced, because refusing a dimension mismatch would make
    // the button stricter than the engine -- see `graph/execution::check_run`.
    out.report.validation_errors = ready.validation_errors;
    out.report.first_problem = ready.first_problem;
    if (ready.validation_errors > 0) {
        out.report.message += " -- but the graph has " + std::to_string(ready.validation_errors) +
                              " validation problem";
        out.report.message += ready.validation_errors == 1 ? "" : "s";
        out.report.message += " (" + ready.first_problem + ")";
    }
    return out;
}

}  // namespace qp::views::model
