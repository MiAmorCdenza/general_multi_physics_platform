/**
 * @file emitter.cpp
 * @brief The frame the ring is launched in, and the arithmetic that does it.
 *
 * One geometry decision is worth writing down because everything else follows from it. At a point `r` the local
 * field direction is `b_hat`, and the velocity is
 *
 *     v = beta c ( cos(alpha) b_hat + sin(alpha) ( cos(psi) p_hat_1 + sin(psi) p_hat_2 ) )
 *
 * with `alpha` the pitch angle, `psi` the flow angle, and `(p_hat_1, p_hat_2)` a right-handed pair perpendicular
 * to `b_hat`, built as `p_hat_1 = normalize(r_hat - (r_hat . b_hat) b_hat)` and `p_hat_2 = b_hat x p_hat_1`.
 *
 * **The pair rotates with the position, and that is why the flow angle must not.** `p_hat_1` is the radial
 * direction projected into the perpendicular plane, so as a particle's azimuth advances by `phi`, the pair
 * rotates by `phi` too. A velocity written with a flow angle *equal to the particle's azimuth* therefore comes
 * out pointing the same way in space for every particle -- the rotation cancels exactly -- and the population
 * that results is a translating ring rather than a gyrotropic one. The first version of this emitter did that,
 * and the case that caught it measured the angle between consecutive particles' velocities and found zero where
 * it expected a full turn's worth of spread. The flow angle is now one number for the whole population, which is
 * what makes 90 degrees mean "everyone moves azimuthally" -- a ring current -- and 0 mean a radial outflow.
 *
 * The degenerate case is `r_hat` parallel to `b_hat`, which happens on the magnetic axis. There the projection is
 * zero and no pair can be built from the position; the fallback takes any axis not parallel to `b_hat`, and it is
 * a fallback rather than a refusal because an emitter on the axis is a legitimate (if unusual) experiment and the
 * particle it launches is still a particle in a field.
 */
#include <qp/plugins/magnetosphere/emitter.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace qp::plugins::magnetosphere {
namespace {

namespace graph = qp::graph;
namespace gfield = qp::graph::field;
namespace pp = qp::graph::particles;

constexpr double kPi = 3.14159265358979323846;

/// @brief A parameter's value, or `fallback` when the node does not carry one or carries a broken one.
[[nodiscard]] double real_or(const graph::Node& node, graph::PortNumber port, double fallback) noexcept {
    const qp::ports::Value value = node.param(port);
    if (!value.valid()) return fallback;
    const double raw = value.to_double();
    return std::isfinite(raw) ? raw : fallback;
}

/// @brief A unit vector in the direction of `v`, or zero when `v` has no direction.
[[nodiscard]] Vec3 unit_or_zero(const Vec3& v) noexcept {
    const double length = norm(v);
    if (!(length > 0.0) || !std::isfinite(length)) return Vec3{};
    return v * (1.0 / length);
}

/// @brief Any unit vector not parallel to `axis`, for the degenerate case where the position gives no frame.
[[nodiscard]] Vec3 any_perpendicular(const Vec3& axis) noexcept {
    const Vec3 candidate = std::abs(axis.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    return unit_or_zero(cross(axis, candidate));
}

}  // namespace

bool EmitterSpec::usable() const noexcept {
    if (!std::isfinite(charge_mass_si) || charge_mass_si == 0.0) return false;
    if (!std::isfinite(l_shell_re) || l_shell_re <= 0.0) return false;
    if (count == 0 || count > EmitterNodes::kMaxCount) return false;
    // `beta < 1` rather than `<= 1`: a particle at the speed of light has no rest frame and an infinite Lorentz
    // factor, and the pusher refuses a speed limit of exactly one for the same reason.
    if (!std::isfinite(beta) || beta < 0.0 || beta >= 1.0) return false;
    if (!std::isfinite(pitch_angle_deg) || !std::isfinite(flow_angle_deg)) return false;
    if (!std::isfinite(ring_span_deg)) return false;
    return true;
}

double EmitterNodes::charge_mass_of(std::int64_t index) noexcept {
    // The order is the property panel's and a saved document's, so it is written once and asserted by the type's
    // own test: an index whose meaning moved would silently change the species of every experiment on disk.
    switch (index) {
        case 0: return kProtonChargeMassSI;
        case 1: return kElectronChargeMassSI;
        case 2: return kAlphaChargeMassSI;
        default: return kProtonChargeMassSI;
    }
}

EmitterSpec EmitterNodes::read_from(const graph::Node& node) noexcept {
    EmitterSpec spec;
    spec.charge_mass_si = charge_mass_of(node.param(kPortSpecies).as_i64());
    spec.l_shell_re = real_or(node, kPortLShell, 6.6);
    const double count = real_or(node, kPortCount, 64.0);
    spec.count = count >= 0.0 && count <= static_cast<double>(kMaxCount)
                     ? static_cast<std::uint32_t>(count)
                     : kMaxCount;
    spec.beta = real_or(node, kPortBeta, 0.01);
    spec.pitch_angle_deg = real_or(node, kPortPitchAngle, 90.0);
    spec.flow_angle_deg = real_or(node, kPortFlowAngle, 90.0);
    spec.ring_span_deg = real_or(node, kPortRingSpan, 360.0);
    return spec;
}

bool emit_ring(const EmitterSpec& spec, const gfield::FieldValue& magnetic, const GridSpec& grid,
               pp::ParticleState& out) {
    if (!spec.usable()) return false;
    if (!gfield::is_readable(magnetic)) return false;

    // The launch points first, then the particles: a point whose field is zero stops the whole emission, and it
    // is better to refuse before any state is written than to leave a state half launched.
    const std::size_t count = spec.count;
    const double r_m = spec.l_shell_re * kEarthRadiusM;
    const double alpha = spec.pitch_angle_deg * kPi / 180.0;
    const double psi = spec.flow_angle_deg * kPi / 180.0;
    const double span = spec.ring_span_deg * kPi / 180.0;
    const double speed = spec.beta * kSpeedOfLightSI;

    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // The azimuth of particle `i`, spread over the population's share of a turn. One particle gets azimuth
        // zero rather than a division by zero; a population of one is a legitimate demonstration.
        const double phi = count > 1 ? span * static_cast<double>(i) / static_cast<double>(count) : 0.0;
        const Vec3 position{r_m * std::cos(phi), r_m * std::sin(phi), 0.0};
        const Vec3 radial = unit_or_zero(position);
        if (!is_finite(radial)) {
            out.resize(0);
            return false;
        }

        const Vec3 b = sample_baked(magnetic, grid.origin_m, grid.spacing_m, position);
        const Vec3 b_hat = unit_or_zero(b);
        if (!is_finite(b_hat) || norm2(b_hat) == 0.0) {
            // A pitch angle against a zero field is an angle against nothing, and the velocity that came out
            // would point wherever the fallback frame happened to point. Refused, and the run reports it.
            out.resize(0);
            return false;
        }

        // The perpendicular pair: the radial direction projected into the plane perpendicular to the local
        // field, so the tilt cannot be lost. The projection is zero on the magnetic axis, where the fallback
        // frame is used and said so.
        Vec3 p1 = unit_or_zero(radial - b_hat * dot(radial, b_hat));
        if (norm2(p1) == 0.0) p1 = any_perpendicular(b_hat);
        const Vec3 p2 = cross(b_hat, p1);

        const Vec3 direction = b_hat * std::cos(alpha) +
                               (p1 * std::cos(psi) + p2 * std::sin(psi)) * std::sin(alpha);
        const Vec3 velocity = direction * speed;

        out.set(i, 0, pp::ParticleState::Slot::position, position.x);
        out.set(i, 1, pp::ParticleState::Slot::position, position.y);
        out.set(i, 2, pp::ParticleState::Slot::position, position.z);
        out.set(i, 0, pp::ParticleState::Slot::velocity, velocity.x);
        out.set(i, 1, pp::ParticleState::Slot::velocity, velocity.y);
        out.set(i, 2, pp::ParticleState::Slot::velocity, velocity.z);
        out.set(i, 0, pp::ParticleState::Slot::charge_mass, spec.charge_mass_si);
        out.set_status(i, pp::Status::live);
    }
    return true;
}

std::vector<graph::NodeDesc> EmitterNodes::node_types() {
    graph::NodeDesc emitter;
    emitter.type_name = kRingType;
    emitter.label = "Ring emitter";
    emitter.description = "Launches a population on an equatorial circle, each particle at the pitch angle you "
                          "ask for measured against the local field and moving along the flow angle you ask "
                          "for. Wire its state output into a pusher.";
    emitter.category = "particles";
    emitter.version = 1;
    // The particle domain: it runs when a run is set up, and it is set up on the main thread. It is not allowed
    // in the field domain because a bake has no particles and an emitter that ran during a bake would be
    // launching into a state nobody owns.
    emitter.allow_in_field_domain = false;
    emitter.allow_in_particle_domain = true;
    // **Computed, and the flag does not mean what it sounds like.** `graph/domain`'s `build_plan` puts a node in
    // a domain's plan only when `has_compute` is set, so the flag is how a node says "I have an implementation"
    // -- and it says nothing about whether the *bake* produces a port value for it. This node's implementation
    // is the run: it expands into a `ParticleState`, which is not a port value and never crosses the state wire.
    // Declaring it false here would keep the emitter out of the particle plan entirely, and the run would report
    // `empty_plan` about a graph that is complete.
    emitter.has_compute = true;

    const auto parameter = [](graph::PortNumber number, const char* name, const char* label, const char* unit,
                              qp::ports::PortTypeId type, double step) {
        graph::PortDesc port;
        port.number = number;
        port.name = name;
        port.label = label;
        port.description = "An initial condition: a parameter rather than a socket, because it describes what is "
                           "launched rather than what is computed. A wired one would need the run to evaluate its "
                           "source, which is a feature and not this one.";
        port.type = type;
        port.connectable = false;
        port.required = true;
        port.unit_symbol = unit;
        port.step = step;
        return port;
    };

    graph::PortDesc species = parameter(kPortSpecies, "species", "Species", "", qp::ports::kEnum, 1.0);
    species.description = "Which particle: proton, electron or alpha. An enum rather than a charge-to-mass "
                          "number, because the number is one a student would have to look up and the choice is "
                          "one they already know.";
    species.choice_names = {"proton", "electron", "alpha"};
    species.choice_labels = {"Proton", "Electron", "Alpha particle"};
    emitter.inputs.push_back(species);
    emitter.inputs.push_back(parameter(kPortLShell, "l_shell_re", "L shell", "R_E", qp::ports::kScalarF64, 0.1));
    emitter.inputs.push_back(parameter(kPortCount, "count", "Particles", "", qp::ports::kInt64, 1.0));
    emitter.inputs.push_back(parameter(kPortBeta, "beta", "Speed", "c", qp::ports::kScalarF64, 0.01));
    emitter.inputs.push_back(parameter(kPortPitchAngle, "pitch_angle_deg", "Pitch angle", "deg",
                                       qp::ports::kScalarF64, 1.0));
    graph::PortDesc flow = parameter(kPortFlowAngle, "flow_angle_deg", "Flow angle", "deg",
                                     qp::ports::kScalarF64, 15.0);
    flow.description = "Where the velocity points in the plane perpendicular to the field, from the radial "
                       "direction towards the azimuthal one. 90 degrees is a ring current; 0 is a radial "
                       "outflow.";
    emitter.inputs.push_back(flow);
    emitter.inputs.push_back(parameter(kPortRingSpan, "ring_span_deg", "Ring span", "deg",
                                       qp::ports::kScalarF64, 15.0));

    graph::PortDesc magnetic;
    magnetic.number = kPortMagnetic;
    magnetic.name = "magnetic";
    magnetic.label = "Magnetic field";
    magnetic.description = "The field the pitch angle is measured against, and the one the particles will be "
                           "pushed by. Wire the same field node into the pusher.";
    magnetic.type = qp::ports::kVectorField;
    magnetic.connectable = true;
    // Required here, unlike the pusher's magnetic socket, and the difference is worth stating: an emitter with no
    // field cannot compute a pitch angle **at all**, so there is nothing to launch and the graph validator
    // reporting "this socket must be wired" is the finding at the layer where the user is looking. The pusher's
    // field is the other case: a run **would** start, and the failure would be a trajectory rather than a
    // message.
    magnetic.required = true;
    magnetic.unit_symbol = "T";
    emitter.inputs.push_back(magnetic);

    graph::PortDesc state;
    state.number = kPortState;
    state.name = "state";
    state.label = "Particle state";
    state.description = "The particles this emitter launches. No value crosses this wire -- the state lives in "
                        "the run -- but the wire is what tells the run which emitter feeds which pusher.";
    state.type = qp::ports::kParticleBuffer;
    state.connectable = true;
    state.required = false;
    emitter.outputs.push_back(state);

    return {std::move(emitter)};
}

std::size_t EmitterNodes::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (graph::NodeDesc& desc : node_types()) {
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

}  // namespace qp::plugins::magnetosphere
