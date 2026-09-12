/**
 * @file run_controller.cpp
 * @brief Finds a runnable node, runs it, and reports what happened in words.
 */
#include <qp/views/model/run_controller.hpp>

#include <cmath>
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
    const execution::RunReadiness ready = execution::check_run(graph, resolve_, binders_,
                                                               execution::StateView::zeroed(1));
    if (!ready.ok()) {
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
