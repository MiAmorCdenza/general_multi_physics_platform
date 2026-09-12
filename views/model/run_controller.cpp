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

/// @brief Whether any binder handles `type_name` with `layout`.
///
/// Asks `can_bind`, not `bind`. The difference is the whole point: a binder that owns a type but cannot
/// honour a particular instance still answers true here, which is what lets the caller report "this node
/// asks for something no operator provides" instead of "nothing in this graph can run".
[[nodiscard]] bool any_binder_handles(const std::vector<execution::IOperatorBinder*>& binders,
                                      std::string_view type_name,
                                      const execution::StateView& layout) {
    for (const execution::IOperatorBinder* binder : binders) {
        if (binder == nullptr) continue;
        if (binder->can_bind(type_name, layout)) return true;
    }
    return false;
}

}  // namespace

RunController::RunController(const qp::authoring::Session& session,
                             std::vector<execution::IOperatorBinder*> binders)
    : session_(&session), binders_(std::move(binders)) {}

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

    // -- 1. find a node to run -------------------------------------------------
    //
    // The first node any binder claims, in graph order. Not "the selected node": a run whose subject
    // depends on a selection the user may have changed for unrelated reasons would sometimes run the
    // wrong thing, and the trace would look plausible either way.
    const qp::graph::Graph& graph = session_->graph();
    const qp::graph::Node* candidate = nullptr;
    // The layout the run will actually use, so a binder's refusal here is the same refusal `prepare`
    // would give rather than an artefact of a probe with the wrong shape.
    const execution::StateView probe = execution::StateView::zeroed(1);

    for (const qp::graph::NodeSlot& slot : graph.slots()) {
        if (!slot.node.id.valid()) continue;
        if (!any_binder_handles(binders_, slot.node.type_name, probe)) continue;
        candidate = &slot.node;
        break;
    }

    if (candidate == nullptr) {
        // The distinction matters to the reader: an empty graph is a different problem from a graph full
        // of nodes no binder knows.
        out.report.message = graph.node_count() == 0
                                 ? "nothing to run: the graph is empty"
                                 : "nothing to run: no node in this graph has an operator yet";
        return out;
    }
    out.report.node_type = candidate->type_name;

    // -- 2. bind, before anything is written down ------------------------------
    //
    // Binding comes first because a refused bind is not an event. Opening the ledger entry first would
    // record a run that never executed, and a ledger whose gaps cite it as present is worse than one
    // that admits the gap -- the whole point of the record is that a missing entry means something.
    execution::GraphRun loop;
    const auto prepared = loop.prepare(*candidate, binders_, rt::RunId{});
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
    return out;
}

}  // namespace qp::views::model
