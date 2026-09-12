/**
 * @file models_binder.cpp
 * @brief What each model node declares, and how its parameters become an operator.
 *
 * The two halves are adjacent for the reason the header gives: a port number that `node_types()` declares and
 * `bind()` does not read is a control the user can turn with no effect, and a port number that `bind()` reads and
 * `node_types()` does not declare is a parameter no editor will offer. Both are silent from the outside, and the
 * case `models.a_node_binds_to_the_model_its_parameters_describe` is what catches them.
 */
#include <qp/plugins/models/models_binder.hpp>

#include <qp/plugins/models/models.hpp>

#include <qp/graph/ir/node_type_registry.hpp>
#include <qp/ports/port_type.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace qp::plugins::models {
namespace {

namespace ex = qp::graph::execution;
using qp::graph::NodeDesc;
using qp::graph::PortDesc;
using qp::graph::PortNumber;

/// @brief A connectable f64 output: the quantity the run traces.
[[nodiscard]] PortDesc output(PortNumber number, std::string name, std::string label) {
    PortDesc p;
    p.number = number;
    p.name = std::move(name);
    p.label = std::move(label);
    p.type = qp::ports::kScalarF64;
    p.connectable = true;
    return p;
}

/// @brief A parameter: a value the user edits, not something to connect.
///
/// `connectable = false` is what makes it a parameter rather than a socket, and it is the same distinction the
/// editor's demonstrator library makes. A model's stiffness cannot be driven by another node in this build, and a
/// socket that nothing can usefully connect to is a socket a user will try to connect.
[[nodiscard]] PortDesc parameter(PortNumber number, std::string name, std::string label,
                                 std::string unit = {}) {
    PortDesc p;
    p.number = number;
    p.name = std::move(name);
    p.label = std::move(label);
    p.type = qp::ports::kScalarF64;
    p.connectable = false;
    p.required = true;
    p.unit_symbol = std::move(unit);
    return p;
}

/// @brief Marks a parameter as bounded, which is what makes the property panel draw a slider.
///
/// The bounds are physical rather than arbitrary: a negative mass, a zero length and a negative frequency are not
/// values a user should be able to type, and a slider that stops at zero is a clearer statement of that than a
/// refusal message after the fact.
void bound(PortDesc& p, double low, double high, double step) {
    p.min_value = low;
    p.max_value = high;
    p.step = step;
}

/// @brief Reads a parameter as a double, or `fallback` when it is unset or not a number.
///
/// `to_double` rather than a kind-specific accessor: the property panel writes an `f64` for a numeric port, but a
/// document that round-tripped through a format could legitimately hold an `i64`, and refusing the integer form of
/// twelve because the editor happened to write a double would be a distinction nobody asked for.
[[nodiscard]] double number(const qp::graph::Node& node, PortNumber port, double fallback) noexcept {
    const qp::ports::Value v = node.param(port);
    if (!v.valid()) return fallback;
    const double d = v.to_double();
    return std::isfinite(d) ? d : fallback;
}

/// @brief The three port descriptions every model shares.
///
/// Shared by construction rather than by convention: three copies of "port 1 is the initial value" is three
/// chances for one model to number its ports differently, and a family whose members disagree about which port is
/// which cannot be laid out by one piece of palette code.
void add_common_ports(NodeDesc& desc, const char* initial_label, const char* rate_label,
                      const char* state_label) {
    desc.outputs.push_back(output(1, "state", state_label));
    desc.inputs.push_back(parameter(1, "initial", initial_label));
    desc.inputs.push_back(parameter(2, "rate", rate_label));
}

}  // namespace

std::vector<NodeDesc> ModelsBinder::node_types() {
    std::vector<NodeDesc> types;

    // ---- The damped harmonic oscillator ------------------------------------
    {
        NodeDesc d;
        d.type_name = kOscillatorType;
        d.label = "Damped oscillator";
        d.description =
            "x'' = -w^2 x - gamma x'. The frequency and the decay rate are both explicit, so the decay a "
            "student measures is the decay the model has.";
        d.category = "models";
        d.version = 1;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = false;  // a solve per step, not a per-frame update
        add_common_ports(d, "Initial displacement", "Initial velocity", "Displacement");
        PortDesc omega = parameter(3, "omega", "Angular frequency", "rad/s");
        bound(omega, 0.01, 1000.0, 0.1);
        d.inputs.push_back(std::move(omega));
        // Port 4 is the damping, and its **presence** is the difference from the demonstrator's spring-damper:
        // that node's binder refuses a non-zero `c` because its kernel has no damping term, so a user who wants
        // decay had nowhere to put the number. Here it is the point of the model.
        PortDesc gamma = parameter(kFirstPhysicalPort, "gamma", "Decay rate", "1/s");
        bound(gamma, 0.0, 100.0, 0.01);
        d.inputs.push_back(std::move(gamma));
        types.push_back(std::move(d));
    }

    // ---- The pendulum ------------------------------------------------------
    {
        NodeDesc d;
        d.type_name = kPendulumType;
        d.label = "Pendulum";
        d.description =
            "th'' = -(g/L) sin(th), without the small-angle approximation. The period grows with amplitude, so "
            "the model disagrees with a textbook that linearised -- on purpose.";
        d.category = "models";
        d.version = 1;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = false;
        add_common_ports(d, "Initial angle", "Initial angular velocity", "Angle");
        PortDesc g = parameter(3, "gravity", "Gravity", "m/s^2");
        bound(g, 0.001, 100.0, 0.001);
        d.inputs.push_back(std::move(g));
        PortDesc length = parameter(kFirstPhysicalPort, "length", "Length", "m");
        bound(length, 0.001, 100.0, 0.001);
        d.inputs.push_back(std::move(length));
        types.push_back(std::move(d));
    }

    // ---- The projectile ----------------------------------------------------
    {
        NodeDesc d;
        d.type_name = kProjectileType;
        d.label = "Projectile";
        d.description =
            "Gravity with linear drag, stopped at the ground. The floor is part of the model, so a landed "
            "projectile stays landed.";
        d.category = "models";
        d.version = 1;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = false;
        add_common_ports(d, "Initial height", "Initial vertical speed", "Height");
        PortDesc g = parameter(3, "gravity", "Gravity", "m/s^2");
        bound(g, 0.001, 100.0, 0.001);
        d.inputs.push_back(std::move(g));
        PortDesc drag = parameter(kFirstPhysicalPort, "drag", "Drag", "1/s");
        bound(drag, 0.0, 10.0, 0.001);
        d.inputs.push_back(std::move(drag));
        types.push_back(std::move(d));
    }

    return types;
}

bool ModelsBinder::can_bind(std::string_view type_name, const ex::StateView& layout) const noexcept {
    // The layout is checked **per type**, because the three models have different state dimensions and a binder
    // that accepted any layout would read a component nothing writes. See the header for why declining is better
    // than adapting.
    if (type_name == kOscillatorType) {
        return layout.components_per_particle == kOscillatorComponents;
    }
    if (type_name == kPendulumType) {
        return layout.components_per_particle == kPendulumComponents;
    }
    if (type_name == kProjectileType) {
        return layout.components_per_particle == kProjectileComponents;
    }
    return false;
}

std::unique_ptr<ex::IStateOperator> ModelsBinder::bind(std::string_view type_name,
                                                       const graph::Node& node,
                                                       const ex::StateView& layout) {
    if (!can_bind(type_name, layout)) return nullptr;

    // A parameter that is missing or not a number takes the **description's own default**, and the defaults are
    // the physical ones: a frequency and a length that make a real experiment, gravity as the standard value, and
    // zero for every initial rate and every damping. A model that refused a node nobody had configured yet would
    // make dropping a node on the canvas an error, which is not how anyone builds a graph -- and the values are
    // visible in the property panel, so the default is a starting point rather than a hidden assumption.
    if (type_name == kOscillatorType) {
        const double omega = number(node, 3, 12.0);
        const double gamma = number(node, kFirstPhysicalPort, 0.0);
        if (!(omega > 0.0) || gamma < 0.0) return nullptr;
        return make_damped_oscillator(omega, gamma);
    }
    if (type_name == kPendulumType) {
        const double g = number(node, 3, kStandardGravity);
        const double length = number(node, kFirstPhysicalPort, 1.0);
        if (!(g > 0.0) || !(length > 0.0)) return nullptr;
        return make_pendulum(g, length);
    }
    // The projectile, which is the only remaining type `can_bind` accepts.
    const double g = number(node, 3, kStandardGravity);
    const double drag = number(node, kFirstPhysicalPort, 0.0);
    if (!(g > 0.0) || drag < 0.0) return nullptr;
    return make_projectile(g, drag);
}

std::size_t ModelsBinder::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (NodeDesc& desc : node_types()) {
        // Counted rather than fatal, for the reason `plugins/instruments` counts: a type whose name is taken is
        // one palette entry missing, and refusing the set would turn a name clash into "this build has no
        // models".
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

}  // namespace qp::plugins::models
