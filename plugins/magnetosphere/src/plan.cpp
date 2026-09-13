/**
 * @file plan.cpp
 * @brief The binding, in one loop: for each pusher node, read its parameters, follow its sockets, bind the slots.
 *
 * Three things here are load-bearing and each is a way the binding could be quietly wrong:
 *
 *   1. **the wired field is looked up by the node the wire comes from**, not by its lattice description. Two
 *      field nodes in one graph can bake the same grid shape, and a lookup by shape would hand the pusher
 *      whichever was published last -- a run that integrated the right physics from the wrong node;
 *   2. **the grid metadata comes from the same node**, through the same reader the baker used. The pusher samples
 *      a box, and a box that is not where the samples are produces a plausible field from the wrong place;
 *   3. **a node the order names but the graph does not hold is a refusal**, not a skip. A plan that silently
 *      dropped a node would run a graph one node smaller than the one the user drew.
 */
#include <qp/plugins/magnetosphere/plan.hpp>

#include <qp/plugins/magnetosphere/boris.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/rk4.hpp>
#include <qp/plugins/magnetosphere/verlet.hpp>

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace qp::plugins::magnetosphere {
namespace {

namespace graph = qp::graph;
namespace gfield = qp::graph::field;
namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;

/// @brief Every socket a pusher has that carries a field, and the slot each one means.
///
/// One table rather than three copies of the same four lines, because "which port is which slot" is a single
/// fact that `PusherNodes::node_types` also states -- and a table is the only shape in which it can be compared
/// against the declaration by reading it.
struct SocketBinding final {
    graph::PortNumber port = 0;
    pp::SlotName slot = pp::SlotName::magnetic;
};

constexpr SocketBinding kSockets[] = {
    {PusherNodes::kPortMagnetic, pp::SlotName::magnetic},
    {PusherNodes::kPortElectric, pp::SlotName::electric},
    {PusherNodes::kPortDrag, pp::SlotName::drag},
};

/// @brief A parameter's value, or `fallback` when the node does not carry one or carries a broken one.
///
/// The finiteness test is not decoration: a parameter that arrived as a NaN would reach `BorisAdvancer::prepare`,
/// which refuses it, and the refusal would be reported as `kernel_refused` -- a plan whose kernel refused its
/// block -- rather than as "this node's speed limit is not a number". The default runs instead, which is what a
/// fresh node does anyway.
[[nodiscard]] double real_or(const graph::Node& node, graph::PortNumber port, double fallback) noexcept {
    const qp::ports::Value value = node.param(port);
    if (!value.valid()) return fallback;
    const double raw = value.to_double();
    return std::isfinite(raw) ? raw : fallback;
}

}  // namespace

const char* to_string(PlanBuildRefusal refusal) noexcept {
    switch (refusal) {
        case PlanBuildRefusal::ok: return "ok";
        case PlanBuildRefusal::stale_order: return "stale_order";
        case PlanBuildRefusal::field_not_baked: return "field_not_baked";
        case PlanBuildRefusal::grid_unknown: return "grid_unknown";
        case PlanBuildRefusal::grid_mismatch: return "grid_mismatch";
    }
    return "unknown";
}

double PusherNodes::max_range_of(const graph::Node& node) noexcept {
    const double range = real_or(node, kPortMaxRangeRe, kDefaultMaxRangeRe);
    // A non-positive radius would retire every particle on its first step, which a user would read as "the
    // simulation is broken". The default is the honest answer to a node whose parameter is unusable.
    return range > 0.0 ? range : kDefaultMaxRangeRe;
}

pk::ParamBlock PusherNodes::param_block_of(const graph::Node& node) noexcept {
    pk::ParamBlock params;
    params.set_real(BorisAdvancer::kIndexMaxRange, max_range_of(node));
    params.set_real(BorisAdvancer::kIndexGravity, real_or(node, kPortGravity, 0.0));
    params.set_real(BorisAdvancer::kIndexSubstepCap, real_or(node, kPortSubstepCap, kDefaultSubstepCap));
    params.set_real(BorisAdvancer::kIndexSpeedLimit,
                    real_or(node, kPortSpeedLimit, BorisAdvancer::kDefaultSpeedLimit));
    params.set_integer(BorisAdvancer::kIndexUseDrag, node.param(kPortUseDrag).as_bool() ? 1 : 0);
    // Gravity defaults to **zero**: at the energies this kit is about, the Earth's pull is a small correction,
    // and a term in the equations that the user did not ask for is a term their report will not mention. The
    // reference implementation has the same switch for the same reason.
    return params;
}

std::size_t PusherNodes::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (graph::NodeDesc& desc : node_types()) {
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

std::vector<graph::NodeDesc> PusherNodes::node_types() {
    graph::NodeDesc boris;
    boris.type_name = kBorisType;
    boris.label = "Boris push";
    boris.description = "Advances charged particles in the field wired into it: a relativistic Boris rotation "
                        "with adaptive sub-stepping, an optional electric field, an optional drag and an "
                        "optional radial gravity. Its product is the particle state, which is why it declares "
                        "no output port.";
    boris.category = "particles";
    boris.version = 1;
    // The particle domain: yes. This is the one node type in the kit that is allowed to run every frame, and it
    // is allowed there because it allocates nothing, blocks on nothing and calls nothing foreign -- the kernel it
    // binds to is the thing that keeps that promise, and `BorisAdvancer::capabilities` is where it says so.
    boris.allow_in_field_domain = false;
    boris.allow_in_particle_domain = true;
    // Computed, in the sense `build_plan` means: this node has an implementation, and the implementation is the
    // kernel the composition root builds for it. The bake produces no port value for it -- the state channel
    // carries topology rather than data -- and `has_compute` false would keep it out of the particle plan
    // altogether, which is the one thing that would make a correct graph report `empty_plan`.
    boris.has_compute = true;

    const auto socket = [](graph::PortNumber number, const char* name, const char* label,
                           qp::ports::PortTypeId type, const char* unit) {
        graph::PortDesc port;
        port.number = number;
        port.name = name;
        port.label = label;
        port.description = "A field socket. What is wired here decides which slot of the pusher's state the field "
                           "fills, and the port type is what stops a scalar field being wired into a vector one.";
        port.type = type;
        port.connectable = true;
        // Only the magnetic field is required, and that requirement is enforced where it can be named:
        // `BorisAdvancer::kRequiredFields` goes into the step's mask and `ParticleExecutor::prepare` refuses the
        // plan with `slot_unbound`. A `required` flag here would be a second answer to the same question, and the
        // graph validator's answer ("a socket nobody wired") is not the same as the run's ("a slot nobody bound").
        port.required = false;
        port.unit_symbol = unit;
        return port;
    };
    boris.inputs.push_back(socket(kPortMagnetic, "magnetic", "Magnetic field", qp::ports::kVectorField, "T"));
    boris.inputs.push_back(socket(kPortElectric, "electric", "Electric field", qp::ports::kVectorField, "V/m"));
    boris.inputs.push_back(socket(kPortDrag, "drag", "Drag", qp::ports::kScalarField, "1/s"));

    const auto knob = [](graph::PortNumber number, const char* name, const char* label, const char* unit,
                         double step) {
        graph::PortDesc port;
        port.number = number;
        port.name = name;
        port.label = label;
        port.description = "A pusher setting: a parameter rather than a socket, because it describes how the step "
                           "is taken rather than what it is taken in.";
        port.type = qp::ports::kScalarF64;
        port.connectable = false;
        port.required = true;
        port.unit_symbol = unit;
        port.step = step;
        return port;
    };
    graph::PortDesc range = knob(kPortMaxRangeRe, "max_range_re", "Retire beyond", "R_E", 0.5);
    range.description = "A particle further out than this is retired as having left the modelled region. Ten "
                        "earth radii is where the magnetopause stands, and beyond it a dipole is not the field "
                        "that is there.";
    graph::PortDesc gravity = knob(kPortGravity, "gravity", "Gravity", "x GM", 0.1);
    gravity.description = "Zero disables gravity and one is the Earth's own field. It is off by default because "
                          "at these energies it is a small correction, and a term in the equations the user did "
                          "not ask for is a term their report will not mention.";
    graph::PortDesc cap = knob(kPortSubstepCap, "substep_cap", "Sub-step cap", "", 64.0);
    cap.description = "The most sub-steps one step may take. A run that reaches this is being under-resolved, "
                      "which is a configuration finding rather than a numerical one.";
    graph::PortDesc limit = knob(kPortSpeedLimit, "speed_limit", "Speed limit", "c", 1.0e-6);
    limit.description = "The fraction of c a particle's speed is held below. Just below one by default; lowering "
                        "it is a legitimate experiment and the kernel counts every time it fires.";
    graph::PortDesc drag_switch = knob(kPortUseDrag, "use_drag", "Use drag", "", 1.0);
    drag_switch.type = qp::ports::kBool;
    drag_switch.description = "Whether the drag socket is read. A socket can be wired and switched off, which is "
                              "how a run compares two experiments without rewiring the graph.";
    boris.inputs.push_back(range);
    boris.inputs.push_back(gravity);
    boris.inputs.push_back(cap);
    boris.inputs.push_back(limit);
    boris.inputs.push_back(drag_switch);

    graph::PortDesc state_in;
    state_in.number = kPortStateIn;
    state_in.name = "state";
    state_in.label = "Particle state";
    state_in.description = "The particles to advance, wired from an emitter or from an earlier pusher. No value "
                           "crosses this wire; it is how the run knows which emitter feeds this step.";
    state_in.type = qp::ports::kParticleBuffer;
    state_in.connectable = true;
    // Required, and this is a **different case** from the magnetic socket's, which is deliberately not. A pusher
    // with no state has nothing to integrate: the run cannot start at all, and the graph validator saying "this
    // socket must be wired" is the finding, at the layer where the user is looking. A pusher with no magnetic
    // field is the silent failure -- the run starts, finishes, and every particle travels in a straight line --
    // and that one is refused by the step's own requirement mask instead.
    state_in.required = true;
    boris.inputs.push_back(state_in);

    graph::PortDesc state_out;
    state_out.number = kPortStateOut;
    state_out.name = "state";
    state_out.label = "Particle state";
    state_out.description = "The particles after this step, so a second pusher can be chained onto it and so a "
                            "run has something to declare as what it wants.";
    state_out.type = qp::ports::kParticleBuffer;
    state_out.connectable = true;
    state_out.required = false;
    boris.outputs.push_back(state_out);

    // **The family.** The reference implementation declares four integrators with identical sockets and identical
    // parameters, and a copy is how that fact is spelled here: a scheme that needed a different socket would be a
    // different family, and one that needed a different *default* would be a different node. What changes is the
    // name, the label and the sentence a user reads in the panel -- which is where the difference actually lives.
    graph::NodeDesc rk4 = boris;
    rk4.type_name = kRk4Type;
    rk4.label = "RK4 push";
    rk4.description = "Advances charged particles with the classical fourth-order Runge-Kutta method: the same "
                      "sockets, the same parameters and the same sub-step control as the Boris push, four field "
                      "samples a step, and no promise that |v| survives. The pair exists so a run can be asked "
                      "both questions: Boris is second order and conserves the speed exactly, this one is fourth "
                      "order and slowly spirals a particle in, and the case measures both.";

    // **The third member, and the same copy**: identical sockets, identical parameters, a different ordering.
    // What its sentence has to say is *where* the force is evaluated, because that is the whole difference.
    graph::NodeDesc verlet = boris;
    verlet.type_name = kVerletType;
    verlet.label = "Velocity-Verlet push";
    verlet.description = "Advances charged particles by half a drift, the force at the midpoint, the exact "
                         "rotation, and half a drift again -- the reference's `position first`. The same sockets, "
                         "the same parameters and the same sub-step control as the Boris push, and the same cost: "
                         "one field sample a sub-step. It differs in where the kick is taken, which is measurably "
                         "a smaller energy error than Boris on the same orbit, and its step is symmetric about its "
                         "own midpoint.";

    return {std::move(boris), std::move(rk4), std::move(verlet)};
}

bool PusherNodes::is_pusher(const std::string& type_name) noexcept {
    return type_name == kBorisType || type_name == kRk4Type || type_name == kVerletType;
}

PlanBuildRefusal resolve_field(const graph::Graph& graph, const graph::NodeId consumer,
                                const graph::PortNumber socket, const gfield::FieldSet& fields,
                                gfield::FieldValue& out, GridSpec& grid) noexcept {
    out = gfield::FieldValue{};
    grid = GridSpec{};
    const graph::Edge* edge = graph.incoming(graph::PortRef{consumer, socket, graph::PortDirection::input});
    if (edge == nullptr) return PlanBuildRefusal::ok;   // nothing wired: the caller decides if that is a refusal
    out = fields.view(gfield::FieldKey{edge->from.node.index, edge->from.port});
    if (!gfield::is_readable(out)) return PlanBuildRefusal::field_not_baked;
    // **The geometry comes from the node that baked it**, through the one resolver that knows how to ask every
    // type. This function used to do the walk itself and refuse anything that was not a dipole, which was honest
    // and also meant a pusher could only ever be wired to a dipole's field -- untrue since the kit gained a
    // uniform field, a sum, an electric field, a mask, a multiplier and a convection field. The refusal moved into
    // `resolve_field_origin`, which follows the combinators back to whoever declared a grid.
    if (!resolve_field_origin(graph, consumer, socket, grid)) return PlanBuildRefusal::grid_unknown;
    return PlanBuildRefusal::ok;
}

PlanBuildRefusal build_particle_plan(const graph::Graph& graph, const std::vector<graph::NodeId>& order,
                                     const gfield::FieldSet& fields, BuiltPlan& out) {
    out.clear();

    for (const graph::NodeId id : order) {
        const graph::Node* node = graph.find_node(id);
        if (node == nullptr) {
            out.clear();
            return PlanBuildRefusal::stale_order;
        }
        if (!PusherNodes::is_pusher(node->type_name)) {
            ++out.skipped;
            continue;
        }

        pk::ParamBlock params = PusherNodes::param_block_of(*node);
        pp::StepPlan step;

        // **Every bound slot must sit on one lattice, and until this check there was nothing that said so.** The
        // kernel has exactly one `GridMetadata` -- six numbers in its parameter block -- because that is what a
        // per-sub-step sampler can afford; the six are filled from the **magnetic** slot below, and every other
        // slot's table is then sampled at those coordinates. So a graph that wired a drag field baked on a
        // different lattice produced a particle dragged by *the wrong cell's* rate: a plausible number from the
        // wrong place, which is the failure `plan.hpp` warns about at length and which no case of either half
        // could catch.
        //
        // The check is new because the *possibility* is new: until this kit had a second kind of field to wire
        // into a second socket (`field.mask` publishing a scalar), a mismatch could not be built. Exact equality
        // rather than a tolerance, and that is the right comparison here: the two grids come from the same
        // `read_from` applied to the same parameters, so a difference in the last bit means the user described a
        // different lattice -- a tolerance would hide exactly the mistake this refuses.
        bool have_grid = false;
        GridSpec shared;
        for (const SocketBinding& binding : kSockets) {
            gfield::FieldValue probe;
            GridSpec grid;
            if (resolve_field(graph, id, binding.port, fields, probe, grid) != PlanBuildRefusal::ok) continue;
            if (!gfield::is_readable(probe)) continue;
            if (!have_grid) {
                have_grid = true;
                shared = grid;
                continue;
            }
            if (grid.origin_m.x != shared.origin_m.x || grid.origin_m.y != shared.origin_m.y ||
                grid.origin_m.z != shared.origin_m.z || grid.spacing_m.x != shared.spacing_m.x ||
                grid.spacing_m.y != shared.spacing_m.y || grid.spacing_m.z != shared.spacing_m.z ||
                grid.nx != shared.nx || grid.ny != shared.ny || grid.nz != shared.nz) {
                out.clear();
                return PlanBuildRefusal::grid_mismatch;
            }
        }

        for (const SocketBinding& binding : kSockets) {
            // **By publisher, not by shape.** See this file's header: a descriptor-keyed lookup would hand the
            // pusher whichever of two identically-shaped fields was published last.
            gfield::FieldValue bound;
            GridSpec grid;
            const PlanBuildRefusal refusal = resolve_field(graph, id, binding.port, fields, bound, grid);
            if (refusal != PlanBuildRefusal::ok) {
                out.clear();
                return refusal;
            }
            // Nothing wired: the slot stays absent, and whether that is a refusal is the **step's** declaration
            // -- `required_slots` below, which makes the executor answer `slot_unbound` by name.
            if (!gfield::is_readable(bound)) continue;
            step.fields[static_cast<std::size_t>(binding.slot)] = bound;

            if (binding.slot != pp::SlotName::magnetic) continue;
            params.set_real(BorisAdvancer::kIndexGridOrigin0, grid.origin_m.x);
            params.set_real(BorisAdvancer::kIndexGridOrigin1, grid.origin_m.y);
            params.set_real(BorisAdvancer::kIndexGridOrigin2, grid.origin_m.z);
            params.set_real(BorisAdvancer::kIndexGridSpacing0, grid.spacing_m.x);
            params.set_real(BorisAdvancer::kIndexGridSpacing1, grid.spacing_m.y);
            params.set_real(BorisAdvancer::kIndexGridSpacing2, grid.spacing_m.z);
        }

        step.param = params;
        // The step declares what it cannot run without, and this is the line that makes
        // `ParticleExecutor::prepare` refuse an unwired pusher by name instead of running it in a zero field. The
        // mask is the family's -- both schemes need the same socket -- so it is read from the shared declaration.
        step.required_slots = PusherParams::kRequiredFields;

        // **Which scheme, by type name**, and that is the only place in the plan where the two differ: everything
        // else about a step -- its sockets, its parameters, its required slots -- is the family's.
        if (node->type_name == PusherNodes::kRk4Type) {
            auto kernel = std::make_unique<Rk4Advancer>();
            step.kernel = kernel.get();
            out.kernels.push_back(std::move(kernel));
        } else if (node->type_name == PusherNodes::kVerletType) {
            auto kernel = std::make_unique<VerletAdvancer>();
            step.kernel = kernel.get();
            out.kernels.push_back(std::move(kernel));
        } else {
            auto kernel = std::make_unique<BorisAdvancer>();
            step.kernel = kernel.get();
            out.kernels.push_back(std::move(kernel));
        }
        out.steps.push_back(step);
        ++out.pushers;
    }

    return PlanBuildRefusal::ok;
}

}  // namespace qp::plugins::magnetosphere
