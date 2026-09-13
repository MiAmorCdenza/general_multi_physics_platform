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

/// @brief Whether the graph holds a pusher, which is what decides if there is anything to step.
[[nodiscard]] bool has_pusher(const graph::Graph& g) noexcept {
    for (const graph::NodeSlot& slot : g.slots()) {
        if (slot.occupied && slot.node.type_name == PusherNodes::kBorisType) return true;
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
    field_only_ = false;
    // The record and what it is computed against: a rebuild is a different experiment, so it starts with an empty
    // trace and no channels. The identity is *not* cleared -- `set_run` rebuilds the trace around it, and a run
    // that kept recording into the previous experiment's channels would append samples of a second setup to the
    // first setup's series.
    trace_ = qp::runtime::Trace{qp::runtime::RunId{}};
    recording_ = false;
    recorded_field_ = qp::graph::field::FieldValue{};
    recorded_grid_ = GridSpec{};
    recorded_mass_kg_ = 0.0;
    recorded_source_ = qp::graph::NodeId{};
}

const pp::AdvanceReport& MagnetosphereRun::advance_report() const noexcept {
    return executor_ == nullptr ? empty_report() : executor_->report();
}

qp::graph::execution::GraphRunReport MagnetosphereRun::report() const {
    qp::graph::execution::GraphRunReport out;
    if (executor_ == nullptr) {
        // A field-only run reports zeroes -- which is the true census -- and says so in the one place a status
        // line reads. An empty note would be indistinguishable from a run that failed before doing anything.
        if (field_only_) out.note = "field baked, no particles declared";
        return out;
    }
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

const qp::runtime::Trace& MagnetosphereRun::trace() const noexcept { return trace_; }

std::optional<qp::graph::execution::RunCadence> MagnetosphereRun::preferred_cadence() const noexcept {
    // No run, no opinion: the caller's default is the only honest answer for an object that launched nothing.
    if (!built() || field_only_) return std::nullopt;
    if (!gfield::is_readable(recorded_field_)) return std::nullopt;
    if (!(std::abs(emitter_.charge_mass_si) > 0.0)) return std::nullopt;

    // The field where the ring was launched, which is the field the gyration happens in: the launch point is on the
    // equator at the emitter's L shell, and the same sampler the kernel reads through is used here so that "the
    // field at the launch radius" means one thing in this file.
    const Vec3 launch{emitter_.l_shell_re * kEarthRadiusM, 0.0, 0.0};
    const Vec3 b = sample_baked(recorded_field_, recorded_grid_.origin_m, recorded_grid_.spacing_m, launch);
    const double b_magnitude = norm(b);
    if (!(b_magnitude > 0.0)) return std::nullopt;

    // `omega_c = |q/m| B`, so the gyro-period is `2 pi / omega_c` -- in **seconds**, because that is the unit the
    // physics is written in, and then converted once into the normalized time the kernel's `dt` is expressed in.
    // One conversion, at this boundary, like every other unit in this kit.
    const double omega_c = std::abs(emitter_.charge_mass_si) * b_magnitude;
    const double gyro_period_s = 2.0 * 3.14159265358979323846 / omega_c;
    const double dt_s = gyro_period_s / static_cast<double>(kStepsPerGyration);

    qp::graph::execution::RunCadence cadence;
    // Both halves, and the count is a **budget this kit can justify rather than a sentinel for "unchanged"**: 4096
    // steps of one thirty-second of a gyro-period is 128 gyrations, which is long enough for the ring to be a ring
    // and short enough that the window's Run button stays a button. A `steps = 0` meaning "use yours" would be the
    // kind of field whose zero means something other than zero, which is the shape this repository refuses.
    cadence.steps = kDefaultRunSteps;
    cadence.dt = dt_s * kNormalizedPerSecond;
    if (!(cadence.dt > 0.0) || !std::isfinite(cadence.dt)) return std::nullopt;
    return cadence;
}

void MagnetosphereRun::set_run(qp::runtime::RunId run) noexcept {
    // A fresh trace rather than a cleared one, because `Trace` takes its identity at construction and exposes no
    // way to change it afterwards -- deliberately, so the id a trace carries is always one a ledger issued.
    trace_ = qp::runtime::Trace{run};
    recording_ = false;
    const qp::runtime::Channel::NodeRef source{recorded_source_.index, recorded_source_.generation};
    const auto declare = [this, source](const char* name, qp::units::Dim dim) {
        return trace_.add_channel(qp::runtime::Channel{name, dim, source}).has_value();
    };
    if (!declare(kSpeedChannel, qp::units::dims::velocity)) return;
    if (!declare(kEnergyChannel, qp::units::dims::energy)) return;
    if (!declare(kMuChannel, qp::units::dims::magnetic_dipole)) return;
    if (!declare(kRadiusChannel, qp::units::dims::length)) return;
    recording_ = true;
}

/// @brief One sample of the recorded particle, in SI.
///
/// A free function in the anonymous namespace rather than a member, because it is arithmetic over a state and a
/// field and holds nothing: the run's job is to know *when* to call it.
[[nodiscard]] bool append_sample(qp::runtime::Trace& trace, const pp::ParticleState& state,
                                 const gfield::FieldValue& field, const GridSpec& grid, double mass_kg,
                                 double t) {
    if (state.count() == 0) return false;
    const Vec3 position{state.at(0, 0, pp::ParticleState::Slot::position),
                        state.at(0, 1, pp::ParticleState::Slot::position),
                        state.at(0, 2, pp::ParticleState::Slot::position)};
    const Vec3 velocity{state.at(0, 0, pp::ParticleState::Slot::velocity),
                        state.at(0, 1, pp::ParticleState::Slot::velocity),
                        state.at(0, 2, pp::ParticleState::Slot::velocity)};
    // **The state is already SI**, and the conversion is therefore nothing at all -- which is worth stating because
    // the first version of this function converted anyway and produced a speed of 8.99e14 m/s and a radius of
    // 2.68e14 m. The measurement that settled it is one line: an L shell of 6.6 came out as 4.2e7, which is 6.6
    // earth radii **in metres**. `positions()` multiplies by `kNormalizedPerMetre` to *undo* this, so the two
    // callers now agree about which unit the state holds, and the disagreement was a second conversion in a kit
    // whose whole unit story is "the conversion happens once, at the boundary".
    const double speed_si = norm(velocity);
    const double radius_si = norm(position);
    const double energy_j = 0.5 * mass_kg * speed_si * speed_si;

    // `v_perp` needs the **local** field direction, and `B` its magnitude: `mu = m v_perp^2 / 2B`. Sampling the
    // same table the pusher reads, through the same sampler, is what makes this the invariant *of this run*
    // rather than of a second reading of the graph.
    const Vec3 magnetic = sample_baked(field, grid.origin_m, grid.spacing_m, position);
    const double b_magnitude = norm(magnetic);
    double mu = 0.0;
    if (b_magnitude > 0.0) {
        const Vec3 b_hat = magnetic * (1.0 / b_magnitude);
        const double along = dot(velocity, b_hat);
        const Vec3 perpendicular = velocity - b_hat * along;
        const double v_perp_si = norm(perpendicular);
        // Joules per tesla: `m v^2 / 2B` in SI, which is the same expression the label promises.
        mu = 0.5 * mass_kg * v_perp_si * v_perp_si / b_magnitude;
    }

    return trace
        .append(t, {qp::runtime::UncertainValue::measured(speed_si, 0.0, qp::units::dims::velocity),
                    qp::runtime::UncertainValue::measured(energy_j, 0.0, qp::units::dims::energy),
                    qp::runtime::UncertainValue::measured(mu, 0.0, qp::units::dims::magnetic_dipole),
                    qp::runtime::UncertainValue::measured(radius_si, 0.0, qp::units::dims::length)})
        .has_value();
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

    // **A graph that wants only a field drawn.** The bake above evaluates the whole graph, so a dipole wired to a
    // `render.field_lines` item has already produced its table by the time the particle half would be asked for
    // one -- and that half has nothing to do: no emitter, no pusher, no state. Refusing with `no_pusher` there
    // would make the simplest possible picture ("draw this field") unbuildable, and the reference implementation
    // does not have that problem because it keeps baking and running as two pipelines.
    //
    // It is one pipeline here, and deliberately: **the run is the only thing in this platform that bakes**, so the
    // choice is between a field-only run and a second mechanism that bakes without running. The run wins on the
    // argument the whole kit is built on -- one bake, one store, one place that says what a run is -- and the cost
    // is paid in three places, each of which says what it means: `advance` does nothing and says so, `report`
    // reports a zero census with a note rather than an empty one, and `built()` is true.
    if (!has_pusher(g)) {
        fields_ = owned_fields_.get();
        field_only_ = true;
        return RunRefusal::ok;
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

    // -- 3b. What the record will be computed against ---------------------------------------------------------
    //
    // The **pusher's** magnetic socket, not the emitter's, even though the two are usually the same wire: `mu` is
    // a property of the field the particle moves in, and a graph that launched a ring against one field and pushed
    // it through another would otherwise get a diagnostic about the wrong one -- a number that is plausible, wrong
    // and impossible to notice. Resolved with the same `resolve_field`, so "which field is on this socket" has one
    // answer in this file rather than two.
    //
    // The mass comes from the emitter's **species index**, read from the node rather than from the spec: the spec
    // carries `q/m` because that is what the pusher divides by, and the trace needs `m` (see `mass_of`).
    recorded_source_ = emitters.front();
    for (const graph::NodeId id : whole.particle.order()) {
        if (is_type(g, id, PusherNodes::kBorisType)) {
            recorded_source_ = id;
            break;
        }
    }
    recorded_field_ = magnetic;
    recorded_grid_ = grid;
    if (const graph::Node* pusher = g.find_node(recorded_source_); pusher != nullptr) {
        gfield::FieldValue pushed;
        GridSpec pushed_grid;
        if (resolve_field(g, recorded_source_, PusherNodes::kPortMagnetic, fields, pushed, pushed_grid) ==
                PlanBuildRefusal::ok &&
            gfield::is_readable(pushed)) {
            recorded_field_ = pushed;
            recorded_grid_ = pushed_grid;
        }
    }
    recorded_mass_kg_ = EmitterNodes::mass_of(emitter_node->param(EmitterNodes::kPortSpecies).as_i64());
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
    // **A run with nothing to step is not a failure.** A graph that wants only a field drawn -- a dipole wired to
    // a `render.field_lines` item, no emitter and no pusher anywhere -- bakes and has no particles, and the
    // controller asks every run to advance. Answering `not_implemented` there would turn "there is nothing to
    // integrate" into "the run is broken", which is a sentence about the wrong thing.
    if (executor_ == nullptr) {
        return field_only_ ? qp::diag::Result<void>{} : qp::diag::Result<void>{qp::diag::ErrorCode::not_implemented};
    }
    pk::AdvanceContext ctx;
    ctx.dt = dt;
    // The initial condition is recorded **before** the first step, so a trace of `steps` steps holds `steps + 1`
    // samples -- the same shape `GraphRun` produces, and the shape a reader assumes: the first number in a column
    // is where the experiment started, not where it had already got to.
    double t = 0.0;
    if (recording_) {
        append_sample(trace_, state_, recorded_field_, recorded_grid_, recorded_mass_kg_, t);
    }
    for (std::size_t step = 0; step < steps; ++step) {
        const auto advanced = executor_->advance(ctx);
        // The first refusal stops the loop and is returned: a run whose kernel refused a step did not silently
        // take the rest, and the counters say how far it got.
        if (!advanced.has_value()) return advanced.error();
        t += dt;
        // Recorded **after** the step and with the step's own time, so sample `n` is the state at `n * dt`. A
        // refusal above leaves the trace one sample short of the counter, which is the honest record: the last
        // sample is the last state that was actually computed.
        if (recording_) {
            append_sample(trace_, state_, recorded_field_, recorded_grid_, recorded_mass_kg_, t);
        }
    }
    return {};
}

}  // namespace qp::plugins::magnetosphere
