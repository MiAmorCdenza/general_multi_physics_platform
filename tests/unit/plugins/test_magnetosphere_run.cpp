/**
 * @file test_magnetosphere_run.cpp
 * @brief The emitter and the run: a graph in, an experiment out.
 *
 * Test case ids match the @tests fields in `plugins/magnetosphere/include`.
 *
 * ## What these cases are about
 *
 * The kit's other two test files measure physics and the bridge. This one measures the **run**: that a graph with
 * a field, an emitter and a pusher becomes a batch of particles that move, that every way of being incomplete is
 * refused by a name rather than by a straight-line trajectory, and that the initial condition is the one the node
 * asked for -- a pitch angle is an angle, and the whole point of the emitter having a magnetic socket is that it
 * is measured against the field and not against an axis.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/eval/evaluator.hpp>
#include <qp/graph/field/field_set.hpp>
#include <qp/graph/particles/particle_state.hpp>
#include <qp/graph/structure.hpp>
#include <qp/host/host.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/dipole.hpp>
#include <qp/plugins/magnetosphere/emitter.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>
#include <qp/plugins/magnetosphere/plan.hpp>
#include <qp/plugins/magnetosphere/run.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace qp::plugins::magnetosphere;

namespace {

namespace graph = qp::graph;
namespace gfield = qp::graph::field;
namespace pp = qp::graph::particles;

/// @brief The relative difference between two numbers, guarded against a zero denominator.
[[nodiscard]] double relative(double a, double b) {
    const double scale = std::abs(b) > 0.0 ? std::abs(b) : 1.0;
    return std::abs(a - b) / scale;
}

/// @brief The particle state's own SI values, as vectors, for a case that wants to do algebra on them.
[[nodiscard]] Vec3 position_of(const pp::ParticleState& state, std::size_t i) {
    return Vec3{state.at(i, 0, pp::ParticleState::Slot::position),
                state.at(i, 1, pp::ParticleState::Slot::position),
                state.at(i, 2, pp::ParticleState::Slot::position)};
}

[[nodiscard]] Vec3 velocity_of(const pp::ParticleState& state, std::size_t i) {
    return Vec3{state.at(i, 0, pp::ParticleState::Slot::velocity),
                state.at(i, 1, pp::ParticleState::Slot::velocity),
                state.at(i, 2, pp::ParticleState::Slot::velocity)};
}

/// @brief A graph, its catalog, the store, and the evaluator that bakes into it -- one experiment's workspace.
struct Scene final {
    qp::host::PluginHost host{qp::plugin::Capability::node_types | qp::plugin::Capability::field_domain |
                              qp::plugin::Capability::particle_domain};
    graph::Graph g{};
    gfield::FieldSet fields{};
    DipoleEvaluator evaluator{fields};
    graph::EvalCache cache{64};
    graph::EvalResult result{};
    graph::Declarations declared{};

    Scene() {
        REQUIRE(FieldNodes::mount(host) == 1);
        REQUIRE(PusherNodes::mount(host) == 1);
        REQUIRE(EmitterNodes::mount(host) == 1);
    }

    [[nodiscard]] graph::EvalContext ctx() noexcept {
        return graph::EvalContext{&host.node_types(), &qp::ports::builtin_registry(), &evaluator, &cache};
    }

    [[nodiscard]] graph::NodeId add(const char* type) {
        const auto r = g.add_node(type);
        REQUIRE(r.has_value());
        return r.value();
    }

    void set(graph::NodeId id, graph::PortNumber port, double v) {
        g.find_node_mutable(id)->set_param(port, qp::ports::Value{v});
        g.bump_version();
    }

    void wire(graph::NodeId from, graph::PortNumber from_port, graph::NodeId to, graph::PortNumber to_port) {
        REQUIRE(g.connect(graph::PortRef{from, from_port, graph::PortDirection::output},
                          graph::PortRef{to, to_port, graph::PortDirection::input}));
    }

    [[nodiscard]] qp::diag::Result<graph::EvalStats> bake() {
        result = graph::EvalResult{};
        return graph::evaluate_graph(g, ctx(), result);
    }

    /// @brief A dipole with the grid every case here uses: eight earth radii either way, a quarter radius a cell.
    [[nodiscard]] graph::NodeId add_dipole(double tilt_degrees) {
        const graph::NodeId id = add(FieldNodes::kDipoleType);
        set(id, FieldNodes::kPortTiltDegrees, tilt_degrees);
        set(id, FieldNodes::kPortMomentAm2, kDipoleMomentAm2);
        for (graph::PortNumber axis = 0; axis < 3; ++axis) {
            set(id, FieldNodes::kPortOrigin0 + axis, -8.0 * kEarthRadiusM);
            set(id, FieldNodes::kPortSpacing0 + axis, 0.25 * kEarthRadiusM);
            set(id, FieldNodes::kPortCount0 + axis, 65.0);
        }
        return id;
    }
};

/// @brief One field-emitter-pusher chain, wired and with its state declared.
struct Chain final {
    graph::NodeId field{};
    graph::NodeId emitter{};
    graph::NodeId pusher{};
};

[[nodiscard]] Chain add_chain(Scene& scene, double tilt_degrees, double l_shell, double count, double beta,
                              double pitch_deg) {
    Chain chain;
    chain.field = scene.add_dipole(tilt_degrees);
    chain.emitter = scene.add(EmitterNodes::kRingType);
    scene.set(chain.emitter, EmitterNodes::kPortLShell, l_shell);
    scene.set(chain.emitter, EmitterNodes::kPortCount, count);
    scene.set(chain.emitter, EmitterNodes::kPortBeta, beta);
    scene.set(chain.emitter, EmitterNodes::kPortPitchAngle, pitch_deg);
    scene.set(chain.emitter, EmitterNodes::kPortFlowAngle, 90.0);
    scene.set(chain.emitter, EmitterNodes::kPortRingSpan, 360.0);
    chain.pusher = scene.add(PusherNodes::kBorisType);
    scene.set(chain.pusher, PusherNodes::kPortMaxRangeRe, 20.0);
    scene.wire(chain.field, FieldNodes::kPortField, chain.emitter, EmitterNodes::kPortMagnetic);
    scene.wire(chain.field, FieldNodes::kPortField, chain.pusher, PusherNodes::kPortMagnetic);
    scene.wire(chain.emitter, EmitterNodes::kPortState, chain.pusher, PusherNodes::kPortStateIn);
    scene.declared.add(graph::DeclaredOutput{chain.pusher, PusherNodes::kPortStateOut});
    return chain;
}

}  // namespace

TEST_CASE("magnetosphere.emitter.the_type_declares_the_ports_the_run_reads", "[magnetosphere]") {
    // The emitter's port table is what the run reads its initial condition from, so the two are checked against
    // each other: every port the reader asks for exists, the field socket is the only one that may be wired, and
    // the state channel is declared at both ends.
    const std::vector<graph::NodeDesc> types = EmitterNodes::node_types();
    REQUIRE(types.size() == 1);
    const graph::NodeDesc& emitter = types.front();
    REQUIRE(emitter.type_name == EmitterNodes::kRingType);
    REQUIRE(emitter.valid());
    REQUIRE(emitter.allow_in_particle_domain);
    REQUIRE_FALSE(emitter.allow_in_field_domain);
    // Computed, in the sense `graph/domain` means: `build_plan` puts a node in a domain's plan only when this
    // flag is set, so it says "I have an implementation" -- the emitter's is the run -- and not "the bake
    // produces a port value for me". The state it launches is not a port value; the wire carries topology.
    REQUIRE(emitter.has_compute);

    const graph::PortNumber parameters[] = {EmitterNodes::kPortSpecies,    EmitterNodes::kPortLShell,
                                            EmitterNodes::kPortCount,      EmitterNodes::kPortBeta,
                                            EmitterNodes::kPortPitchAngle, EmitterNodes::kPortFlowAngle,
                                            EmitterNodes::kPortRingSpan};
    for (const graph::PortNumber number : parameters) {
        const graph::PortDesc* port = emitter.find_port(number, false);
        REQUIRE(port != nullptr);
        // Parameters, not sockets: a wired initial condition would need the run to evaluate its source, which is
        // a feature and not this one.
        REQUIRE_FALSE(port->connectable);
        REQUIRE(port->required);
        REQUIRE_FALSE(port->name.empty());
    }
    const graph::PortDesc* species = emitter.find_port(EmitterNodes::kPortSpecies, false);
    REQUIRE(species->type == qp::ports::kEnum);
    REQUIRE(species->choice_names.size() == EmitterNodes::kSpeciesCount);
    REQUIRE(species->choice_names[0] == std::string{"proton"});
    REQUIRE(species->choice_names[1] == std::string{"electron"});
    REQUIRE(species->choice_names[2] == std::string{"alpha"});
    REQUIRE(species->choice_labels.size() == species->choice_names.size());

    const graph::PortDesc* magnetic = emitter.find_port(EmitterNodes::kPortMagnetic, false);
    REQUIRE(magnetic != nullptr);
    REQUIRE(magnetic->type == qp::ports::kVectorField);
    REQUIRE(magnetic->connectable);
    // Required, and it is the opposite declaration from the pusher's magnetic socket. See `PusherNodes`: an
    // emitter with no field cannot compute a pitch angle at all, so the graph validator's "this socket must be
    // wired" is the right finding. The pusher's is the silent case and is refused by the step's mask instead.
    REQUIRE(magnetic->required);
    REQUIRE_FALSE(emitter.find_port(PusherNodes::kPortMagnetic, false) == nullptr);

    const graph::PortDesc* state = emitter.find_port(EmitterNodes::kPortState, true);
    REQUIRE(state != nullptr);
    REQUIRE(state->type == qp::ports::kParticleBuffer);
    REQUIRE(state->connectable);

    // The species table, against the published charge-to-mass ratios. The **order** is what a saved document
    // stores, so an index whose meaning moved would change the species of every experiment already written down.
    REQUIRE(EmitterNodes::charge_mass_of(0) == kProtonChargeMassSI);
    REQUIRE(EmitterNodes::charge_mass_of(1) == kElectronChargeMassSI);
    REQUIRE(EmitterNodes::charge_mass_of(2) == kAlphaChargeMassSI);
    REQUIRE(EmitterNodes::charge_mass_of(99) == kProtonChargeMassSI);
    REQUIRE(relative(kProtonChargeMassSI, 9.5788332e7) < 1.0e-6);
    REQUIRE(kElectronChargeMassSI < 0.0);
    REQUIRE(relative(kAlphaChargeMassSI / kProtonChargeMassSI, 0.5) < 1.0e-15);

    // A fresh node produces a spec a run can use, and its defaults are the ones the description promises.
    graph::Node fresh;
    fresh.type_name = EmitterNodes::kRingType;
    const EmitterSpec spec = EmitterNodes::read_from(fresh);
    REQUIRE(spec.usable());
    REQUIRE(spec.charge_mass_si == kProtonChargeMassSI);
    REQUIRE(spec.count == 64);
    REQUIRE(spec.pitch_angle_deg == 90.0);
    REQUIRE(spec.flow_angle_deg == 90.0);
    REQUIRE(spec.ring_span_deg == 360.0);
    // A node carrying a species index reads it; the enum's value is an integer parameter, which is what the
    // property panel's choice list writes.
    graph::Node electron;
    electron.set_param(EmitterNodes::kPortSpecies, qp::ports::Value{std::int64_t{1}});
    REQUIRE(EmitterNodes::read_from(electron).charge_mass_si == kElectronChargeMassSI);

    qp::host::PluginHost host{qp::plugin::Capability::particle_domain};
    REQUIRE(EmitterNodes::mount(host) == 1);
    REQUIRE(host.node_types().find(EmitterNodes::kRingType) != nullptr);
    REQUIRE(EmitterNodes::mount(host) == 0);
}

TEST_CASE("magnetosphere.emitter.a_ring_is_launched_at_the_pitch_angle_it_asks_for", "[magnetosphere]") {
    // **The case that pins the launch geometry.** The pitch angle is the angle between the velocity and the
    // **local** field, and a population launched with a tilt in the field is where an implementation that
    // measured the angle against `+z` would differ -- by exactly the tilt. So the check is done against the same
    // baked table the emitter read, at every particle, and it is done for a tilted dipole.
    constexpr double kTiltDegrees = 30.0;
    BakedField table{Vec3{-8.0 * kEarthRadiusM, -8.0 * kEarthRadiusM, -8.0 * kEarthRadiusM},
                     Vec3{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM}, 33, 33, 33};
    const DipoleField dipole{kTiltDegrees};
    for (std::uint32_t i = 0; i < 33; ++i) {
        for (std::uint32_t j = 0; j < 33; ++j) {
            for (std::uint32_t k = 0; k < 33; ++k) {
                table.set_node(i, j, k, dipole.at(table.node_position(i, j, k)));
            }
        }
    }
    const GridSpec grid{table.origin(), table.spacing(), 33, 33, 33};

    for (const double pitch : {90.0, 45.0, 20.0}) {
        EmitterSpec spec;
        spec.charge_mass_si = kProtonChargeMassSI;
        spec.l_shell_re = 6.6;
        spec.count = 12;
        spec.beta = 0.02;
        spec.pitch_angle_deg = pitch;
        spec.flow_angle_deg = 90.0;
        spec.ring_span_deg = 360.0;
        REQUIRE(spec.usable());

        pp::ParticleState state;
        REQUIRE(emit_ring(spec, table.view(), grid, state));
        REQUIRE(state.count() == 12);
        REQUIRE(state.live_count() == 12);

        for (std::size_t i = 0; i < state.count(); ++i) {
            const Vec3 position = position_of(state, i);
            const Vec3 velocity = velocity_of(state, i);

            // On the ring: an equatorial circle at L, and the tilt does not move it.
            REQUIRE(relative(norm(Vec3{position.x, position.y, 0.0}), spec.l_shell_re * kEarthRadiusM) < 1.0e-12);
            REQUIRE(position.z == 0.0);

            // The speed, exactly what the spec asked for: `beta c`, in metres per second.
            REQUIRE(relative(norm(velocity), spec.beta * kSpeedOfLightSI) < 1.0e-12);

            // The angle to the **local** field, read from the same table the emitter read. This is the assertion
            // that a `+z` implementation fails, and it fails by the tilt.
            const Vec3 b = sample_baked(table.view(), grid.origin_m, grid.spacing_m, position);
            const double cosine = dot(velocity, b) / (norm(velocity) * norm(b));
            const double degrees = std::acos(cosine) * 180.0 / 3.14159265358979323846;
            REQUIRE(relative(degrees, pitch) < 1.0e-9);

            // And the charge is the species', on the particle rather than in a table the pusher would have to
            // consult.
            REQUIRE(state.at(i, 0, pp::ParticleState::Slot::charge_mass) == kProtonChargeMassSI);
        }

        // The positions really are spread around the ring rather than clustered: consecutive particles are
        // `span / count` apart in azimuth.
        for (std::size_t i = 1; i < state.count(); ++i) {
            const Vec3 a = position_of(state, i - 1);
            const Vec3 b = position_of(state, i);
            const double angle = std::acos(dot(a, b) / (norm(a) * norm(b))) * 180.0 / 3.14159265358979323846;
            REQUIRE(relative(angle, 360.0 / 12.0) < 1.0e-9);
        }
    }

    // **The population, not just the particles**, and it is measured in an **untilted** field on purpose. With a
    // tilt the local field is not perpendicular to the radius, so "the flow is azimuthal" becomes a statement
    // about the field's own frame rather than about the radius, and the radius is what a reader can check. With
    // no tilt the two coincide, which is also the configuration a ring-current lesson uses.
    BakedField flat{Vec3{-8.0 * kEarthRadiusM, -8.0 * kEarthRadiusM, -8.0 * kEarthRadiusM},
                    Vec3{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM}, 33, 33, 33};
    const DipoleField untilted{0.0};
    for (std::uint32_t i = 0; i < 33; ++i) {
        for (std::uint32_t j = 0; j < 33; ++j) {
            for (std::uint32_t k = 0; k < 33; ++k) {
                flat.set_node(i, j, k, untilted.at(flat.node_position(i, j, k)));
            }
        }
    }

    // A ring current: every velocity perpendicular to its own radius. This is the property that a flow angle
    // tied to the particle's position destroys -- the first version of this emitter produced velocities that
    // were all parallel to each other, a translating ring -- and it is asserted as a number rather than as a
    // picture.
    EmitterSpec ring;
    ring.count = 12;
    ring.pitch_angle_deg = 90.0;
    ring.flow_angle_deg = 90.0;
    pp::ParticleState ring_state;
    REQUIRE(emit_ring(ring, flat.view(), grid, ring_state));
    for (std::size_t i = 0; i < ring_state.count(); ++i) {
        const Vec3 position = position_of(ring_state, i);
        const Vec3 radial = position * (1.0 / norm(position));
        const double radial_flow = dot(velocity_of(ring_state, i), radial) / norm(velocity_of(ring_state, i));
        REQUIRE(std::abs(radial_flow) < 1.0e-12);
    }

    // And a radial outflow: the same ring launched outward instead of azimuthally. The two populations differ in
    // exactly the angle the spec names, which is what makes the flow angle a parameter rather than a constant.
    EmitterSpec outflow = ring;
    outflow.flow_angle_deg = 0.0;
    pp::ParticleState radial_state;
    REQUIRE(emit_ring(outflow, flat.view(), grid, radial_state));
    for (std::size_t i = 0; i < radial_state.count(); ++i) {
        const Vec3 position = position_of(radial_state, i);
        const Vec3 radial = position * (1.0 / norm(position));
        const double radial_flow = dot(velocity_of(radial_state, i), radial) / norm(velocity_of(radial_state, i));
        REQUIRE(relative(radial_flow, 1.0) < 1.0e-12);
    }

    // An arc rather than a full ring: the span decides how much of the circle the population fills.
    EmitterSpec arc;
    arc.count = 4;
    arc.ring_span_deg = 90.0;
    pp::ParticleState arc_state;
    REQUIRE(emit_ring(arc, table.view(), grid, arc_state));
    REQUIRE(arc_state.count() == 4);
    for (std::size_t i = 0; i < arc_state.count(); ++i) {
        const Vec3 position = position_of(arc_state, i);
        // Inside the quarter turn the span asks for, and evenly spaced within it: particle `i` sits at
        // `span * i / count` degrees, so the four of them are at 0, 22.5, 45 and 67.5 -- none of them at 90.
        const double azimuth = std::atan2(position.y, position.x) * 180.0 / 3.14159265358979323846;
        const double expected = 90.0 * static_cast<double>(i) / 4.0;
        REQUIRE(relative(azimuth, expected) < 1.0e-9);
    }

    // A full spread but a single particle: one particle gets phase zero rather than a division by zero, and it
    // is the case a demonstration of a single orbit is.
    EmitterSpec single;
    single.count = 1;
    single.pitch_angle_deg = 90.0;
    pp::ParticleState one;
    REQUIRE(emit_ring(single, table.view(), grid, one));
    REQUIRE(one.count() == 1);
    REQUIRE(one.live_count() == 1);
    REQUIRE(relative(norm(velocity_of(one, 0)), single.beta * kSpeedOfLightSI) < 1.0e-12);

    // The refusals, each of which would otherwise launch a particle whose state is not a number or whose
    // direction is arbitrary.
    pp::ParticleState refused;
    EmitterSpec bad = single;
    bad.count = 0;
    REQUIRE_FALSE(bad.usable());
    REQUIRE_FALSE(emit_ring(bad, table.view(), grid, refused));
    bad = single;
    bad.beta = 1.0;   // the speed of light itself: an infinite Lorentz factor, refused where the pusher does too
    REQUIRE_FALSE(bad.usable());
    bad = single;
    bad.l_shell_re = 0.0;
    REQUIRE_FALSE(bad.usable());
    bad = single;
    bad.charge_mass_si = 0.0;
    REQUIRE_FALSE(bad.usable());
    bad = single;
    bad.pitch_angle_deg = std::nan("");
    REQUIRE_FALSE(bad.usable());
    bad = single;
    bad.count = EmitterNodes::kMaxCount + 1;
    REQUIRE_FALSE(bad.usable());
    REQUIRE(refused.count() == 0);

    // A zero field at the launch point: a pitch angle against nothing, refused rather than launched with an
    // arbitrary frame.
    BakedField empty_field{Vec3{-kEarthRadiusM, -kEarthRadiusM, -kEarthRadiusM},
                           Vec3{kEarthRadiusM, kEarthRadiusM, kEarthRadiusM}, 2, 2, 2};
    const GridSpec empty_grid{empty_field.origin(), empty_field.spacing(), 2, 2, 2};
    pp::ParticleState none;
    REQUIRE_FALSE(emit_ring(single, empty_field.view(), empty_grid, none));
    REQUIRE(none.count() == 0);
}

TEST_CASE("magnetosphere.run.a_graph_becomes_a_run", "[magnetosphere]") {
    // **The closed loop**: a graph, a bake, a run, and a population that does what the graph says.
    Scene scene;
    const Chain chain = add_chain(scene, /*tilt=*/0.0, /*l_shell=*/6.6, /*count=*/8, /*beta=*/0.01,
                                  /*pitch=*/90.0);
    REQUIRE(scene.bake().has_value());
    REQUIRE(scene.fields.size() == 1);

    MagnetosphereRun run;
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) == RunRefusal::ok);
    REQUIRE(run.built());
    REQUIRE(run.state().count() == 8);
    REQUIRE(run.state().live_count() == 8);
    REQUIRE(run.plan().steps.size() == 1);
    REQUIRE(run.plan().pushers == 1);
    REQUIRE(run.emitter().count == 8);
    REQUIRE(run.emitter().l_shell_re == 6.6);
    REQUIRE(run.baked_bytes() > 0);
    REQUIRE(run.report().steps == 0);

    // The field the run reads is the one the graph baked: the state's particles were launched against it, and
    // the pusher binds the same entry.
    REQUIRE(run.plan().steps.front().field(pp::SlotName::magnetic).data ==
            scene.fields.view(gfield::FieldKey{chain.field.index, FieldNodes::kPortField}).data);

    const std::size_t steps = 1000;
    REQUIRE(run.advance(steps, 0.01).has_value());
    REQUIRE(run.report().steps == steps);
    // Nothing was clamped and nothing was retired: a magnetic field does no work, so a population launched
    // inside the grid has no reason to leave it or to be throttled.
    REQUIRE(run.report().clamped == 0);
    REQUIRE(run.state().live_count() == 8);

    // The physics the graph predicted, measured over the whole population. The gyroradius is `v / omega` with
    // `omega = (q/m) B / gamma`, and `B` at 6.6 earth radii is the dipole's own 103 nT.
    const Vec3 b_start = DipoleField{0.0}.at(Vec3{6.6 * kEarthRadiusM, 0.0, 0.0});
    const double gamma0 = 1.0 / std::sqrt(1.0 - 0.01 * 0.01);
    const double omega = kProtonNormalizedChargeMass * (norm(b_start) * kNormalizedPerTesla) / gamma0;
    const double gyroradius = 0.01 / omega;
    REQUIRE(relative(gyroradius, 0.0475) < 0.05);

    for (std::size_t i = 0; i < run.state().count(); ++i) {
        const Vec3 position = position_of(run.state(), i);
        const Vec3 velocity = velocity_of(run.state(), i);
        // The speed is unchanged, particle by particle: the property a magnetic field has and the one a wrong
        // scheme loses first.
        REQUIRE(relative(norm(velocity), 0.01 * kSpeedOfLightSI) < 1.0e-10);
        // Each particle's guiding centre is on the ring, so no particle wanders further than twice a gyroradius
        // from where it was launched. Two, not one: the launch phase decides which side of the ring the centre
        // sits on, and the gyration then adds its own radius.
        const double radius = norm(position) * kNormalizedPerMetre;
        REQUIRE(std::abs(radius - 6.6) < 2.2 * gyroradius);
        // Still a particle in the equatorial plane: a pure magnetic field along the axis does no work and
        // exerts no force along itself.
        REQUIRE(std::abs(position.z) * kNormalizedPerMetre < 2.2 * gyroradius);
    }

    // A run that was asked for zero steps did nothing, and one that was asked for more kept going from where it
    // was rather than from the start.
    const std::size_t before = run.report().steps;
    REQUIRE(run.advance(0, 0.01).has_value());
    REQUIRE(run.report().steps == before);
    REQUIRE(run.advance(10, 0.01).has_value());
    REQUIRE(run.report().steps == before + 10);

    // The counter strings a report quotes.
    REQUIRE(std::string{to_string(RunRefusal::ok)} == "ok");
    REQUIRE(std::string{to_string(RunRefusal::several_emitters)} == "several_emitters");
    REQUIRE(std::string{to_string(RunRefusal::executor_rejected)} == "executor_rejected");
}

TEST_CASE("magnetosphere.run.a_run_without_a_field_is_refused", "[magnetosphere]") {
    // Every way an incomplete graph is refused, and each refusal has to be a **name** rather than a trajectory:
    // a run that started anyway would produce particles in a straight line, which is what a broken field model
    // looks like too.
    const graph::Declarations nothing_declared;
    Scene empty;
    MagnetosphereRun run;
    REQUIRE(run.build(empty.g, nothing_declared, empty.host.node_types(), empty.fields) ==
            RunRefusal::empty_plan);
    REQUIRE_FALSE(run.built());
    REQUIRE(run.state().count() == 0);

    // A chain whose field is not in the store: the bake did not run. This is the refusal that keeps a run from
    // integrating zeros, and it is the reason the run does not bake for itself.
    {
        Scene scene;
        const Chain chain = add_chain(scene, 0.0, 6.6, 4, 0.01, 90.0);
        (void)chain;
        REQUIRE(scene.fields.size() == 0);
        REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
                RunRefusal::field_not_baked);
        REQUIRE_FALSE(run.built());
    }

    // An emitter whose magnetic socket is not wired: nothing to measure a pitch angle against. The graph
    // validator reports this too, from the port's own `required` flag, and the run refuses it because it cannot
    // launch without it.
    {
        Scene scene;
        const graph::NodeId emitter = scene.add(EmitterNodes::kRingType);
        const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
        scene.wire(emitter, EmitterNodes::kPortState, pusher, PusherNodes::kPortStateIn);
        scene.declared.add(graph::DeclaredOutput{pusher, PusherNodes::kPortStateOut});
        REQUIRE(scene.bake().has_value());
        REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
                RunRefusal::field_not_baked);
    }

    // **The payoff from the step's requirement mask**: a pusher whose magnetic socket is unwired is bound with
    // an absent slot, and the executor refuses the plan by name -- `slot_unbound` -- rather than integrating a
    // zero field. The two refusals are different findings: one is "the bake did not run", the other is "the graph
    // does not wire this socket", and the user's next action differs.
    {
        Scene scene;
        const graph::NodeId field = scene.add_dipole(0.0);
        const graph::NodeId emitter = scene.add(EmitterNodes::kRingType);
        scene.set(emitter, EmitterNodes::kPortCount, 4.0);
        const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
        scene.wire(field, FieldNodes::kPortField, emitter, EmitterNodes::kPortMagnetic);
        scene.wire(emitter, EmitterNodes::kPortState, pusher, PusherNodes::kPortStateIn);
        scene.declared.add(graph::DeclaredOutput{pusher, PusherNodes::kPortStateOut});
        REQUIRE(scene.bake().has_value());
        REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
                RunRefusal::executor_rejected);
        REQUIRE(run.executor_refusal() == pp::PlanRefusal::slot_unbound);
        REQUIRE_FALSE(run.built());
        // An advance on a run that was never built is refused rather than silently doing nothing.
        REQUIRE_FALSE(run.advance(1, 0.01).has_value());
    }

    // A pusher whose state channel leads nowhere: there is nothing to advance.
    {
        Scene scene;
        const graph::NodeId field = scene.add_dipole(0.0);
        const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
        scene.wire(field, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);
        scene.declared.add(graph::DeclaredOutput{pusher, PusherNodes::kPortStateOut});
        REQUIRE(scene.bake().has_value());
        REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
                RunRefusal::no_emitter);
    }

    // An emitter that cannot launch: zero particles is the case a user reaches by typing one digit.
    {
        Scene scene;
        const Chain chain = add_chain(scene, 0.0, 6.6, 0.0, 0.01, 90.0);
        (void)chain;
        REQUIRE(scene.bake().has_value());
        REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
                RunRefusal::emitter_unusable);
    }

    // Two independent chains, both declared: the state is one buffer, so "which particles" has no answer, and
    // the run says so rather than picking one. This is the graph a document holding two experiments produces.
    {
        Scene scene;
        add_chain(scene, 0.0, 6.6, 4, 0.01, 90.0);
        add_chain(scene, 0.0, 4.0, 4, 0.01, 90.0);
        REQUIRE(scene.bake().has_value());
        REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
                RunRefusal::several_emitters);
    }

    REQUIRE(std::string{to_string(RunRefusal::field_not_baked)} == "field_not_baked");
    REQUIRE(std::string{to_string(RunRefusal::no_emitter)} == "no_emitter");
    REQUIRE(std::string{to_string(RunRefusal::empty_plan)} == "empty_plan");
}

TEST_CASE("magnetosphere.run.a_rebuild_replaces_the_run", "[magnetosphere]") {
    // A run is rebuilt rather than patched: a student changes a number and the next run is the new experiment.
    // Two properties make that usable -- everything from the previous run is released first, so a rebuild cannot
    // leave half of an old state behind, and the counters restart, so a report is about the run it describes.
    Scene scene;
    const Chain chain = add_chain(scene, 0.0, 6.6, 6, 0.01, 90.0);
    REQUIRE(scene.bake().has_value());

    MagnetosphereRun run;
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) == RunRefusal::ok);
    REQUIRE(run.state().count() == 6);
    REQUIRE(run.advance(50, 0.01).has_value());
    REQUIRE(run.report().steps == 50);

    // The same graph again: the previous state is gone, and the counters are its own.
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) == RunRefusal::ok);
    REQUIRE(run.built());
    REQUIRE(run.state().count() == 6);
    REQUIRE(run.report().steps == 0);

    // A changed parameter, re-baked and rebuilt: a different population, still with its own counters.
    scene.set(chain.emitter, EmitterNodes::kPortCount, 15.0);
    scene.set(chain.emitter, EmitterNodes::kPortLShell, 4.0);
    REQUIRE(scene.bake().has_value());
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) == RunRefusal::ok);
    REQUIRE(run.state().count() == 15);
    REQUIRE(run.emitter().l_shell_re == 4.0);
    REQUIRE(run.report().steps == 0);

    // And a failed rebuild leaves an **empty** run rather than the previous one wearing the new refusal: a
    // caller that read `state()` after a refusal would otherwise integrate the experiment it just replaced.
    scene.g.find_node_mutable(chain.emitter)->set_param(EmitterNodes::kPortCount, qp::ports::Value{0.0});
    scene.g.bump_version();
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
            RunRefusal::emitter_unusable);
    REQUIRE_FALSE(run.built());
    REQUIRE(run.state().count() == 0);
    REQUIRE(run.report().steps == 0);
    REQUIRE(run.plan().steps.empty());
    REQUIRE(run.plan_build() == PlanBuildRefusal::ok);
}
