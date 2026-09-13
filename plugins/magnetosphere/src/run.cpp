/**
 * @file run.cpp
 * @brief Five steps, in the order the header argues for, and one walking-backwards loop.
 *
 * The interesting part is the emitter lookup. A run has to answer "which particles does this pusher advance",
 * and there are three ways it could have been answered: by looking for a node of the emitter's type in the plan
 * (which silently picks one when there are two), by taking the first particle-domain node in the order (which
 * depends on the topological tie-break), or by following the **state wire** backwards from each pusher. Only the
 * third is a fact about the graph rather than about the order the plan happened to be built in, and it is also
 * the only one that survives a graph with two independent emitters -- which is a graph a battery of two
 * experiments in one document produces. Two emitters feeding two different chains is refused by name rather than
 * resolved arbitrarily.
 *
 * The chain walk is a loop rather than one hop because a plan may hold several pushers in sequence: the second
 * one's state comes from the first one's output, so the emitter is two or more wires upstream. The set of
 * emitters is collected rather than the first one found, because "there is exactly one" is a property the run
 * needs and not one it can assume.
 */
#include <qp/plugins/magnetosphere/run.hpp>

#include <qp/graph/eval/evaluator.hpp>
#include <qp/graph/structure.hpp>

#include <qp/plugins/magnetosphere/boris.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace qp::plugins::magnetosphere {
namespace {

namespace graph = qp::graph;
namespace gfield = qp::graph::field;
namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;

/// @brief Whether `id` names a node of `type_name`.
[[nodiscard]] bool is_type(const graph::Graph& g, graph::NodeId id, const char* type_name) noexcept {
    const graph::Node* node = g.find_node(id);
    return node != nullptr && node->type_name == type_name;
}

/// @brief The node feeding `socket`, or an invalid id when nothing is wired.
[[nodiscard]] graph::NodeId upstream(const graph::Graph& g, graph::NodeId consumer,
                                     graph::PortNumber socket) noexcept {
    const graph::Edge* edge = g.incoming(graph::PortRef{consumer, socket, graph::PortDirection::input});
    return edge == nullptr ? graph::NodeId{} : edge->from.node;
}

/// @brief The emitter a pusher's state channel leads back to, or an invalid id.
///
/// Walks the chain: a pusher fed by a pusher is fed by whatever feeds that one. Bounded by the number of nodes,
/// so a graph whose state channel somehow formed a cycle (which `graph/structure` refuses at connect time, and
/// which this loop refuses to depend on) terminates rather than hanging.
[[nodiscard]] graph::NodeId emitter_behind(const graph::Graph& g, graph::NodeId pusher) noexcept {
    graph::NodeId current = upstream(g, pusher, PusherNodes::kPortStateIn);
    for (std::size_t hop = 0; current.valid() && hop <= g.slots().size(); ++hop) {
        if (is_type(g, current, EmitterNodes::kRingType)) return current;
        if (!is_type(g, current, PusherNodes::kBorisType)) return graph::NodeId{};
        current = upstream(g, current, PusherNodes::kPortStateIn);
    }
    return graph::NodeId{};
}

/// @brief Whether `seen` already holds `id`.
[[nodiscard]] bool contains(const std::vector<graph::NodeId>& seen, graph::NodeId id) noexcept {
    for (const graph::NodeId other : seen) {
        if (other == id) return true;
    }
    return false;
}

/// @brief The empty report a run that was never built answers with.
///
/// A static rather than a member, because the alternative is a second copy of the counters that could drift from
/// the executor's. `report()` is then total: a caller that asks an empty run what it did gets zeros, which is the
/// true answer.
[[nodiscard]] const pp::AdvanceReport& empty_report() noexcept {
    static const pp::AdvanceReport kEmpty{};
    return kEmpty;
}

}  // namespace

const char* to_string(RunRefusal refusal) noexcept {
    switch (refusal) {
        case RunRefusal::ok: return "ok";
        case RunRefusal::empty_plan: return "empty_plan";
        case RunRefusal::no_pusher: return "no_pusher";
        case RunRefusal::no_emitter: return "no_emitter";
        case RunRefusal::several_emitters: return "several_emitters";
        case RunRefusal::field_not_baked: return "field_not_baked";
        case RunRefusal::emitter_unusable: return "emitter_unusable";
        case RunRefusal::grid_unknown: return "grid_unknown";
        case RunRefusal::plan_rejected: return "plan_rejected";
        case RunRefusal::executor_rejected: return "executor_rejected";
    }
    return "unknown";
}

void MagnetosphereRun::reset() noexcept {
    executor_.reset();
    plan_.clear();
    state_.resize(0);
    emitter_ = EmitterSpec{};
    plan_refusal_ = PlanBuildRefusal::ok;
    executor_refusal_ = pp::PlanRefusal::ok;
}

const pp::AdvanceReport& MagnetosphereRun::advance_report() const noexcept {
    return executor_ == nullptr ? empty_report() : executor_->report();
}

qp::graph::execution::GraphRunReport MagnetosphereRun::report() const {
    qp::graph::execution::GraphRunReport out;
    if (executor_ == nullptr) return out;
    const pp::AdvanceReport& advanced = executor_->report();
    out.steps = advanced.steps;
    out.clamped = advanced.clamped;
    out.particles = state_.count();
    out.live = state_.live_count();
    out.absorbed = state_.count_with(pp::Status::absorbed);
    out.escaped = state_.count_with(pp::Status::escaped);
    // The kernels this kit built are `BorisAdvancer`s, and the name check is what makes the downcast a checked
    // claim rather than an assumption: a kernel this kit did not build would be skipped rather than read as one.
    for (const std::unique_ptr<pk::IBatchAdvancer>& kernel : plan_.kernels) {
        if (kernel != nullptr && kernel->name() == std::string_view{BorisAdvancer::kName}) {
            out.speed_clamps += static_cast<const BorisAdvancer*>(kernel.get())->speed_clamps();
        }
    }
    out.note = std::string{BorisAdvancer::kName} + ": " + std::to_string(out.steps) + " steps, " +
               std::to_string(out.live) + " of " + std::to_string(out.particles) + " live";
    return out;
}

std::vector<double> MagnetosphereRun::positions() const {
    std::vector<double> out;
    if (state_.count() == 0) return out;
    out.reserve(state_.count() * 3);
    for (std::size_t i = 0; i < state_.count(); ++i) {
        out.push_back(state_.at(i, 0, pp::ParticleState::Slot::position) * kNormalizedPerMetre);
        out.push_back(state_.at(i, 1, pp::ParticleState::Slot::position) * kNormalizedPerMetre);
        out.push_back(state_.at(i, 2, pp::ParticleState::Slot::position) * kNormalizedPerMetre);
    }
    return out;
}

const qp::graph::field::FieldSet& MagnetosphereRun::fields() const noexcept {
    // The base's shared empty set when there is no store, rather than a member: see the declaration's comment.
    return fields_ != nullptr ? *fields_ : IGraphRun::fields();
}

RunRefusal MagnetosphereRun::build_with_own_fields(const graph::Graph& g, const graph::Declarations& declared,
                                                   const graph::INodeCatalog& catalog) {
    reset();
    owned_fields_ = std::make_unique<gfield::FieldSet>();

    // The bake, with this kit's own evaluator and no cache: pressing Run means "bake what the graph says now".
    DipoleEvaluator evaluator{*owned_fields_};
    // The graph, because a node with field inputs can only reach its inputs' **data** through the wiring: see
    // `set_graph`'s own comment for why the value it is handed cannot carry the key.
    evaluator.set_graph(g);
    graph::EvalResult result;
    const graph::EvalContext ctx{&catalog, &qp::ports::builtin_registry(), &evaluator, nullptr};
    const auto baked = graph::evaluate_graph(g, ctx, result);
    if (!baked.has_value()) {
        // The bake refused. For this graph that means a field node whose parameters the evaluator could not
        // honour, and `field_not_baked` is the code this kit already uses for "the field is not there".
        reset();
        return RunRefusal::field_not_baked;
    }
    return build(g, declared, catalog, *owned_fields_);
}

bool MagnetosphereRunProvider::claims(const graph::Graph& graph) const noexcept {
    for (const graph::NodeSlot& slot : graph.slots()) {
        if (!slot.occupied) continue;
        const std::string& type = slot.node.type_name;
        if (type == FieldNodes::kDipoleType || type == EmitterNodes::kRingType || type == PusherNodes::kBorisType) {
            return true;
        }
    }
    return false;
}

qp::graph::execution::RunBuildResult MagnetosphereRunProvider::build(const graph::Graph& graph,
                                                                     const graph::INodeCatalog& catalog) {
    // Every pusher in the graph, declared as what the caller wants: pressing Run on a particle graph means "run
    // the pushers that are in it". A declared-output editor, when one exists, is what will replace this.
    graph::Declarations declared;
    for (const graph::NodeSlot& slot : graph.slots()) {
        if (!slot.occupied) continue;
        if (slot.node.type_name == PusherNodes::kBorisType) {
            declared.add(graph::DeclaredOutput{slot.node.id, PusherNodes::kPortStateOut});
        }
    }

    auto run = std::make_unique<MagnetosphereRun>();
    const RunRefusal refusal = run->build_with_own_fields(graph, declared, catalog);
    qp::graph::execution::RunBuildResult out;
    if (refusal != RunRefusal::ok) {
        out.refusal = std::string{to_string(refusal)};
        return out;
    }
    out.run = std::move(run);
    return out;
}

RunRefusal MagnetosphereRun::build(const graph::Graph& g, const graph::Declarations& declared,
                                   const graph::INodeCatalog& catalog, gfield::FieldSet& fields) {
    reset();
    fields_ = &fields;

    // -- 1. The plan: the domain layer decides what belongs where, from what was asked for ------------------
    const graph::ExecutionPlan whole = graph::build_plan(g, graph::PlanContext{&catalog}, declared);
    if (whole.particle.empty()) return RunRefusal::empty_plan;

    // -- 2. The emitter: backwards along the state channel from every pusher ---------------------------------
    std::vector<graph::NodeId> emitters;
    for (const graph::NodeId id : whole.particle.order()) {
        if (!is_type(g, id, PusherNodes::kBorisType)) continue;
        const graph::NodeId emitter = emitter_behind(g, id);
        if (!emitter.valid()) return RunRefusal::no_emitter;
        if (!contains(emitters, emitter)) emitters.push_back(emitter);
    }
    if (emitters.empty()) return RunRefusal::no_pusher;
    if (emitters.size() > 1) return RunRefusal::several_emitters;

    // -- 3. Launch: the emitter's spec against the field it is wired to --------------------------------------
    const graph::Node* emitter_node = g.find_node(emitters.front());
    if (emitter_node == nullptr) return RunRefusal::no_emitter;
    gfield::FieldValue magnetic;
    GridSpec grid;
    switch (resolve_field(g, emitters.front(), EmitterNodes::kPortMagnetic, fields, magnetic, grid)) {
        case PlanBuildRefusal::ok: break;
        case PlanBuildRefusal::field_not_baked: return RunRefusal::field_not_baked;
        case PlanBuildRefusal::grid_unknown: return RunRefusal::grid_unknown;
        case PlanBuildRefusal::stale_order: return RunRefusal::field_not_baked;
    }
    // Nothing wired: the emitter's magnetic socket is `required`, so this is the case the graph validator also
    // reports -- and the run cannot launch without it, because a pitch angle needs a field to be measured
    // against.
    if (!gfield::is_readable(magnetic)) return RunRefusal::field_not_baked;

    emitter_ = EmitterNodes::read_from(*emitter_node);
    if (!emit_ring(emitter_, magnetic, grid, state_)) return RunRefusal::emitter_unusable;

    // -- 4. Bind: the composition root's half, over the particle domain's order -----------------------------
    plan_refusal_ = build_particle_plan(g, whole.particle.order(), fields, plan_);
    if (plan_refusal_ != PlanBuildRefusal::ok) return RunRefusal::plan_rejected;
    if (plan_.steps.empty()) return RunRefusal::no_pusher;

    // -- 5. Prepare: once, before any step, which is where a kernel validates and where a step that requires a
    //       field nobody bound is refused by name --------------------------------------------------------------
    executor_ = std::make_unique<pp::ParticleExecutor>(state_, plan_.steps);
    executor_refusal_ = executor_->prepare();
    if (executor_refusal_ != pp::PlanRefusal::ok) {
        executor_.reset();
        return RunRefusal::executor_rejected;
    }
    return RunRefusal::ok;
}

qp::diag::Result<void> MagnetosphereRun::advance(std::size_t steps, double dt) {
    if (executor_ == nullptr) return qp::diag::ErrorCode::not_implemented;
    pk::AdvanceContext ctx;
    ctx.dt = dt;
    for (std::size_t step = 0; step < steps; ++step) {
        const auto advanced = executor_->advance(ctx);
        // The first refusal stops the loop and is returned: a run whose kernel refused a step did not silently
        // take the rest, and the counters say how far it got.
        if (!advanced.has_value()) return advanced.error();
    }
    return {};
}

}  // namespace qp::plugins::magnetosphere
