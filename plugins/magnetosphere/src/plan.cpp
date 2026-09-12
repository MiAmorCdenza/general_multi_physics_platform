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
    boris.has_compute = false;

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

    return {std::move(boris)};
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
        if (node->type_name != PusherNodes::kBorisType) {
            ++out.skipped;
            continue;
        }

        pk::ParamBlock params = PusherNodes::param_block_of(*node);
        pp::StepPlan step;

        for (const SocketBinding& binding : kSockets) {
            const graph::Edge* edge = graph.incoming(graph::PortRef{id, binding.port, graph::PortDirection::input});
            if (edge == nullptr) continue;

            // **By publisher, not by shape.** See this file's header: a descriptor-keyed lookup would hand the
            // pusher whichever of two identically-shaped fields was published last.
            const gfield::FieldKey key{edge->from.node.index, edge->from.port};
            const gfield::FieldValue bound = fields.view(key);
            if (!gfield::is_readable(bound)) {
                out.clear();
                return PlanBuildRefusal::field_not_baked;
            }
            step.fields[static_cast<std::size_t>(binding.slot)] = bound;

            if (binding.slot != pp::SlotName::magnetic) continue;
            // The grid comes from the node the magnetic socket is wired to, and only from a type this build
            // knows how to ask. `FieldNodes::read_from` is the baker's own reader, so the box the kernel samples
            // is the box the samples were evaluated in.
            const graph::Node* source = graph.find_node(edge->from.node);
            if (source == nullptr || source->type_name != FieldNodes::kDipoleType) {
                out.clear();
                return PlanBuildRefusal::grid_unknown;
            }
            const GridSpec grid = FieldNodes::read_from(*source);
            params.set_real(BorisAdvancer::kIndexGridOrigin0, grid.origin_m.x);
            params.set_real(BorisAdvancer::kIndexGridOrigin1, grid.origin_m.y);
            params.set_real(BorisAdvancer::kIndexGridOrigin2, grid.origin_m.z);
            params.set_real(BorisAdvancer::kIndexGridSpacing0, grid.spacing_m.x);
            params.set_real(BorisAdvancer::kIndexGridSpacing1, grid.spacing_m.y);
            params.set_real(BorisAdvancer::kIndexGridSpacing2, grid.spacing_m.z);
        }

        step.param = params;
        // The step declares what it cannot run without, and this is the line that makes
        // `ParticleExecutor::prepare` refuse an unwired pusher by name instead of running it in a zero field.
        step.required_slots = BorisAdvancer::kRequiredFields;

        auto kernel = std::make_unique<BorisAdvancer>();
        step.kernel = kernel.get();
        out.kernels.push_back(std::move(kernel));
        out.steps.push_back(step);
        ++out.pushers;
    }

    return PlanBuildRefusal::ok;
}

}  // namespace qp::plugins::magnetosphere
