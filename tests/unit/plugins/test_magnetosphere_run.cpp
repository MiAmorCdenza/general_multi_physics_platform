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
#include <qp/plugins/magnetosphere/boris.hpp>
#include <qp/plugins/magnetosphere/dipole.hpp>
#include <qp/plugins/magnetosphere/emitter.hpp>
#include <qp/plugins/magnetosphere/field_lines_item.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>
#include <qp/plugins/magnetosphere/plan.hpp>
#include <qp/plugins/magnetosphere/render_nodes.hpp>
#include <qp/plugins/magnetosphere/run.hpp>

#include <qp/plugins/magnetosphere/rk4.hpp>
#include <qp/plugins/magnetosphere/view_item.hpp>
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
        // Fourteen field models now: the dipole, the uniform field, the sum, the electric field, the region mask,
        // the multiplier, the convection field, the corotation field, the atmosphere, the current sheet, the blend,
        // the resampler, the magnetopause and the mix.
        REQUIRE(FieldNodes::mount(host) == 14);
        REQUIRE(PusherNodes::mount(host) == 2);
        REQUIRE(EmitterNodes::mount(host) == 1);
        // The render item too, or `build_plan` cannot look its descriptor up and silently records no
        // declaration for it -- which is the failure this case exists to catch, and it caught it here first.
        REQUIRE(RenderNodes::mount(host) == 2);
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

    /// @brief Sets a **boolean** port, which is not the same thing as setting a double to one.
    ///
    /// Added because a case got it wrong: the pusher's drag switch is a kBool port and the plan reads it with
    /// s_bool(), so a node carrying 1.0 reads as *false* and the drag silently does nothing -- a run that
    /// looks correct and decays by nothing. The helper exists so the type is in the call rather than in a comment.
    void set_flag(graph::NodeId id, graph::PortNumber port, bool v) {
        g.find_node_mutable(id)->set_param(port, qp::ports::Value{v});
        g.bump_version();
    }

    void wire(graph::NodeId from, graph::PortNumber from_port, graph::NodeId to, graph::PortNumber to_port) {
        REQUIRE(g.connect(graph::PortRef{from, from_port, graph::PortDirection::output},
                          graph::PortRef{to, to_port, graph::PortDirection::input}));
    }

    [[nodiscard]] qp::diag::Result<graph::EvalStats> bake() {
        result = graph::EvalResult{};
        // The evaluator borrows the graph, exactly as `MagnetosphereRun` hands it over: a node with field inputs
        // can reach its inputs' samples only by following their wires.
        evaluator.set_graph(g);
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
                              double pitch_deg, const char* pusher_type = PusherNodes::kBorisType) {
    Chain chain;
    chain.field = scene.add_dipole(tilt_degrees);
    chain.emitter = scene.add(EmitterNodes::kRingType);
    scene.set(chain.emitter, EmitterNodes::kPortLShell, l_shell);
    scene.set(chain.emitter, EmitterNodes::kPortCount, count);
    scene.set(chain.emitter, EmitterNodes::kPortBeta, beta);
    scene.set(chain.emitter, EmitterNodes::kPortPitchAngle, pitch_deg);
    scene.set(chain.emitter, EmitterNodes::kPortFlowAngle, 90.0);
    scene.set(chain.emitter, EmitterNodes::kPortRingSpan, 360.0);
    chain.pusher = scene.add(pusher_type);
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

TEST_CASE("magnetosphere.rk4.a_run_reports_which_scheme_it_used", "[magnetosphere]") {
    // **The family is one question with several answers, and a report may not name the wrong one.** A comparison
    // between two integrators whose note said "boris" for both would be a comparison of nothing, and the run is
    // where the name surfaces: it reads it from the kernel the plan built. So this case drives the same chain the
    // closed-loop case drives, with the fourth-order node instead, and checks the name -- and the physics that says
    // the run really happened -- at every place it appears.
    Scene scene;
    const Chain chain = add_chain(scene, /*tilt=*/0.0, /*l_shell=*/6.6, /*count=*/4, /*beta=*/0.01,
                                  /*pitch=*/90.0, PusherNodes::kRk4Type);
    REQUIRE(scene.bake().has_value());
    REQUIRE(scene.host.node_types().find(PusherNodes::kRk4Type) != nullptr);

    MagnetosphereRun run;
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) == RunRefusal::ok);
    REQUIRE(run.built());
    REQUIRE(run.plan().steps.size() == 1);
    REQUIRE(run.plan().pushers == 1);
    REQUIRE(run.state().count() == 4);
    // The kernel the plan built for this step is the fourth-order one, and it says so by name.
    REQUIRE(run.plan().steps.front().kernel != nullptr);
    REQUIRE(run.plan().steps.front().kernel->name() == std::string_view{Rk4Advancer::kName});
    // The required-slot mask is the family's, and the recipe is the same: a step with no magnetic field is refused
    // by name whatever scheme would have run it.
    REQUIRE(run.plan().steps.front().required_slots == PusherParams::kRequiredFields);

    const std::size_t steps = 100;
    REQUIRE(run.advance(steps, 0.01).has_value());
    REQUIRE(run.advance_report().steps == steps);
    REQUIRE(run.state().live_count() == 4);
    // The note names the scheme: `rk4`, not the default. It is the run's own record, which is the sentence a ledger
    // entry carries.
    const qp::graph::execution::GraphRunReport record = run.report();
    REQUIRE(record.note.find(Rk4Advancer::kName) != std::string::npos);
    REQUIRE(record.note.find(BorisAdvancer::kName) == std::string::npos);

    // And the same graph with the default node names the default, so the two statements above are about the
    // scheme rather than about the string "rk4" appearing somewhere in a note.
    Scene boris_scene;
    const Chain boris_chain = add_chain(boris_scene, 0.0, 6.6, 4.0, 0.01, 90.0);
    REQUIRE(boris_scene.bake().has_value());
    MagnetosphereRun boris_run;
    REQUIRE(boris_run.build(boris_scene.g, boris_scene.declared, boris_scene.host.node_types(),
                            boris_scene.fields) == RunRefusal::ok);
    REQUIRE(boris_run.plan().steps.front().kernel->name() == std::string_view{BorisAdvancer::kName});
    REQUIRE(boris_run.advance(steps, 0.01).has_value());
    REQUIRE(boris_run.report().note.find(BorisAdvancer::kName) != std::string::npos);
    // One cadence, two schemes, and a population launched the same way: the speeds differ, because one scheme loses
    // a little of every rotation and the other does not. That is the pair the kit exists to show.
    const auto speed_of = [](const pp::ParticleState& state) {
        return norm(Vec3{state.at(0, 0, pp::ParticleState::Slot::velocity),
                         state.at(0, 1, pp::ParticleState::Slot::velocity),
                         state.at(0, 2, pp::ParticleState::Slot::velocity)});
    };
    REQUIRE(speed_of(run.state()) < speed_of(boris_run.state()));
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
    REQUIRE(run.advance_report().steps == 0);

    // The field the run reads is the one the graph baked: the state's particles were launched against it, and
    // the pusher binds the same entry.
    REQUIRE(run.plan().steps.front().field(pp::SlotName::magnetic).data ==
            scene.fields.view(gfield::FieldKey{chain.field.index, FieldNodes::kPortField}).data);

    const std::size_t steps = 1000;
    REQUIRE(run.advance(steps, 0.01).has_value());
    REQUIRE(run.advance_report().steps == steps);
    // Nothing was clamped and nothing was retired: a magnetic field does no work, so a population launched
    // inside the grid has no reason to leave it or to be throttled.
    REQUIRE(run.advance_report().clamped == 0);
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
    const std::size_t before = run.advance_report().steps;
    REQUIRE(run.advance(0, 0.01).has_value());
    REQUIRE(run.advance_report().steps == before);
    REQUIRE(run.advance(10, 0.01).has_value());
    REQUIRE(run.advance_report().steps == before + 10);

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
    REQUIRE(run.advance_report().steps == 50);

    // The same graph again: the previous state is gone, and the counters are its own.
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) == RunRefusal::ok);
    REQUIRE(run.built());
    REQUIRE(run.state().count() == 6);
    REQUIRE(run.advance_report().steps == 0);

    // A changed parameter, re-baked and rebuilt: a different population, still with its own counters.
    scene.set(chain.emitter, EmitterNodes::kPortCount, 15.0);
    scene.set(chain.emitter, EmitterNodes::kPortLShell, 4.0);
    REQUIRE(scene.bake().has_value());
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) == RunRefusal::ok);
    REQUIRE(run.state().count() == 15);
    REQUIRE(run.emitter().l_shell_re == 4.0);
    REQUIRE(run.advance_report().steps == 0);

    // And a failed rebuild leaves an **empty** run rather than the previous one wearing the new refusal: a
    // caller that read `state()` after a refusal would otherwise integrate the experiment it just replaced.
    scene.g.find_node_mutable(chain.emitter)->set_param(EmitterNodes::kPortCount, qp::ports::Value{0.0});
    scene.g.bump_version();
    REQUIRE(run.build(scene.g, scene.declared, scene.host.node_types(), scene.fields) ==
            RunRefusal::emitter_unusable);
    REQUIRE_FALSE(run.built());
    REQUIRE(run.state().count() == 0);
    REQUIRE(run.advance_report().steps == 0);
    REQUIRE(run.plan().steps.empty());
    REQUIRE(run.plan_build() == PlanBuildRefusal::ok);
}


TEST_CASE("magnetosphere.run.a_provider_builds_a_run_from_a_graph", "[magnetosphere]") {
    // **The interface a caller with no idea what a particle is uses.** `IGraphRunProvider` is asked two
    // questions -- is this graph yours, and build me a run -- and the whole kit has to fit behind them: the bake,
    // the launch, the plan, the executor. What a caller gets back is a report and a snapshot of positions, which
    // is what a status line and a canvas need and is all this kit promises to hand out.
    Scene scene;
    const Chain chain = add_chain(scene, 0.0, 6.6, 8, 0.01, 90.0);
    (void)chain;

    MagnetosphereRunProvider provider;
    REQUIRE(provider.name() == std::string_view{"magnetosphere"});
    REQUIRE(provider.claims(scene.g));
    // An empty graph is not this provider's, and saying so is not a failure: it is the normal answer from all
    // but one provider in a list.
    const graph::Graph empty;
    REQUIRE_FALSE(provider.claims(empty));

    // **The provider bakes for itself.** It has nobody to borrow a store from, so `build_with_own_fields` runs
    // the field domain through this kit's evaluator and owns the result -- which is why the interface above can
    // be written without naming a field type at all.
    qp::graph::execution::RunBuildResult built = provider.build(scene.g, scene.host.node_types());
    REQUIRE(built.ok());
    REQUIRE(built.refusal.empty());
    REQUIRE(built.run != nullptr);

    // Before a step: the census is the emitter's, and nothing has moved.
    qp::graph::execution::GraphRunReport report = built.run->report();
    REQUIRE(report.particles == 8);
    REQUIRE(report.live == 8);
    REQUIRE(report.absorbed == 0);
    REQUIRE(report.escaped == 0);
    REQUIRE(report.steps == 0);
    REQUIRE(report.is_consistent());
    REQUIRE_FALSE(report.note.empty());
    REQUIRE(built.run->positions().size() == 24);

    // Ten thousand steps of the whole thing, through the interface and no other door.
    REQUIRE(built.run->advance(1000, 0.01).has_value());
    report = built.run->report();
    REQUIRE(report.steps == 1000);
    REQUIRE(report.live == 8);
    REQUIRE(report.is_consistent());
    REQUIRE(report.speed_clamps == 0);
    REQUIRE(report.clamped == 0);

    // The snapshot is in **earth radii**, converted at this boundary the way every other conversion in the kit
    // is: a canvas whose axes are in metres is a canvas nobody can read, and a caller that had to know `R_E` to
    // draw a picture would be a caller that had to know the physics.
    const std::vector<double> positions = built.run->positions();
    REQUIRE(positions.size() == 24);
    for (std::size_t i = 0; i < 8; ++i) {
        const double x = positions[i * 3 + 0];
        const double y = positions[i * 3 + 1];
        const double z = positions[i * 3 + 2];
        const double radius = std::sqrt(x * x + y * y + z * z);
        // The ring's guiding centres sit on 6.6 and each gyration adds at most twice a gyroradius, which is
        // 0.0475 earth radii here.
        REQUIRE(std::abs(radius - 6.6) < 0.11);
    }

    // A graph the provider claims but cannot build: a pusher with no field wired to it. The sentence is this
    // kit's own word for the finding, not an error code squashed into a number -- which is the whole reason
    // `RunBuildResult` carries a string.
    Scene incomplete;
    const graph::NodeId emitter = incomplete.add(EmitterNodes::kRingType);
    const graph::NodeId pusher = incomplete.add(PusherNodes::kBorisType);
    incomplete.wire(emitter, EmitterNodes::kPortState, pusher, PusherNodes::kPortStateIn);
    REQUIRE(provider.claims(incomplete.g));
    const qp::graph::execution::RunBuildResult refused = provider.build(incomplete.g, incomplete.host.node_types());
    REQUIRE_FALSE(refused.ok());
    REQUIRE(refused.run == nullptr);
    REQUIRE(refused.refusal == std::string{to_string(RunRefusal::field_not_baked)});
}


TEST_CASE("magnetosphere.render.the_item_is_declared_and_never_evaluated", "[magnetosphere]") {
    // **What the render domain is for.** A render node is a *declaration*: the graph says what it wants drawn,
    // nothing is evaluated, and a view item does the drawing. `graph/domain` already implements that split --
    // a declared output whose node has no compute goes into `plan.render.declared` -- and this case is what pins
    // the kit against it, because a render type that quietly set `has_compute` true would be evaluated during a
    // bake and would drag the view layer's representation into the kernel.
    Scene scene;
    const Chain chain = add_chain(scene, 0.0, 6.6, 4, 0.01, 90.0);
    const graph::NodeId item = scene.add(RenderNodes::kParticlesType);
    scene.set(item, RenderNodes::kPortTrail, 32.0);
    scene.wire(chain.pusher, PusherNodes::kPortStateOut, item, RenderNodes::kPortState);
    scene.declared.add(graph::DeclaredOutput{item, RenderNodes::kPortItem});

    const std::vector<graph::NodeDesc> types = RenderNodes::node_types();
    REQUIRE(types.size() == 2);
    REQUIRE(types.front().type_name == RenderNodes::kParticlesType);
    REQUIRE_FALSE(types.front().has_compute);
    REQUIRE(types.front().find_port(RenderNodes::kPortState, false) != nullptr);
    REQUIRE(types.front().find_port(RenderNodes::kPortItem, true) != nullptr);
    REQUIRE(types.front().find_port(RenderNodes::kPortItem, true)->type == qp::ports::kParticleBuffer);

    const graph::ExecutionPlan plan =
        graph::build_plan(scene.g, graph::PlanContext{&scene.host.node_types()}, scene.declared);

    // Declared, with the port the graph named -- the pair is what a view item is handed.
    REQUIRE(plan.render.declared().size() == 1);
    REQUIRE(plan.render.declared().front().node == item);
    REQUIRE(plan.render.declared().front().port == RenderNodes::kPortItem);
    // And **not evaluated**: the render domain has no execution order, and the node appears in neither
    // evaluation plan. A node in both would be drawn and computed, which is the weld this design prevents.
    REQUIRE(plan.render.order().empty());
    for (const graph::NodeId id : plan.field.order()) REQUIRE(id != item);
    for (const graph::NodeId id : plan.particle.order()) REQUIRE(id != item);
    // The chain that feeds it *is* in the particle plan, so the item is reached through the graph rather than
    // standing alone: the declaration is what makes the branch reachable, and the branch is what makes the
    // declaration meaningful.
    REQUIRE_FALSE(plan.particle.order().empty());
    REQUIRE(plan.field.order().size() == 1);
}


TEST_CASE("magnetosphere.render.the_field_lines_type_declares_the_ports_the_item_reads",
          "[magnetosphere]") {
    // The second render type, and the case is about **one difference** from the first: this declaration names a
    // baked product. A render node is still never evaluated -- `has_compute` is false here too -- but its data
    // socket is wired to a field node, and the item that draws it follows that wire into the run's samples. That
    // single difference is what made `ViewRequest` carry a field store and `IGraphRun` grow `fields()`, so it is
    // worth pinning at the declaration rather than only at the item.
    Scene scene;
    const graph::NodeId dipole = scene.add_dipole(0.0);
    const graph::NodeId lines = scene.add(RenderNodes::kFieldLinesType);
    scene.set(lines, RenderNodes::kPortLineCount, 5.0);
    scene.set(lines, RenderNodes::kPortSeedStart, 2.0);
    scene.set(lines, RenderNodes::kPortSeedEnd, 6.0);
    scene.set(lines, RenderNodes::kPortStepMax, 0.2);
    scene.set(lines, RenderNodes::kPortTolerance, 1.0e-4);
    scene.wire(dipole, FieldNodes::kPortField, lines, RenderNodes::kPortField);
    scene.declared.add(graph::DeclaredOutput{lines, RenderNodes::kPortFieldItem});

    const std::vector<graph::NodeDesc> types = RenderNodes::node_types();
    REQUIRE(types.size() == 2);
    const graph::NodeDesc& described = types[1];
    REQUIRE(described.type_name == RenderNodes::kFieldLinesType);
    REQUIRE(described.valid());
    // The same two flags as the particle item, and for the same reason: a render type with `has_compute` true
    // would be evaluated during a bake.
    REQUIRE_FALSE(described.has_compute);
    REQUIRE_FALSE(described.allow_in_field_domain);
    REQUIRE_FALSE(described.allow_in_particle_domain);
    // The data socket is a **connectable vector field**, which is what makes the wire to a field node legal:
    // `check_connection` is what refuses a scalar field or a particle state here, and it can only do that if the
    // declaration says which one it is.
    const graph::PortDesc* data = described.find_port(RenderNodes::kPortField, false);
    REQUIRE(data != nullptr);
    REQUIRE(data->connectable);
    REQUIRE(data->required);
    REQUIRE(data->type == qp::ports::kVectorField);
    REQUIRE(described.find_port(RenderNodes::kPortFieldItem, true) != nullptr);
    // Every parameter is a port and none of them is connectable: the tracer's configuration is chosen once for
    // the picture, and a wired step cap would be a value that changes under a drawing that has already been made.
    for (const graph::PortNumber number : {RenderNodes::kPortLineCount, RenderNodes::kPortSeedStart,
                                           RenderNodes::kPortSeedEnd, RenderNodes::kPortStepMax,
                                           RenderNodes::kPortTolerance}) {
        const graph::PortDesc* parameter = described.find_port(number, false);
        REQUIRE(parameter != nullptr);
        REQUIRE_FALSE(parameter->connectable);
        REQUIRE(parameter->required);
    }
    // The two items agree about where the drawn thing goes, which is the point of sharing the port number: a
    // reader who has wired one item has wired the other.
    REQUIRE(types[0].find_port(RenderNodes::kPortState, false)->number ==
            types[1].find_port(RenderNodes::kPortField, false)->number);

    // The declaration reaches the render plan and the field branch reaches the field plan, which is what makes
    // the item find a baked table when it follows the wire.
    const graph::ExecutionPlan plan =
        graph::build_plan(scene.g, graph::PlanContext{&scene.host.node_types()}, scene.declared);
    REQUIRE(plan.render.declared().size() == 1);
    REQUIRE(plan.render.declared().front().node == lines);
    REQUIRE(plan.render.declared().front().port == RenderNodes::kPortFieldItem);
    REQUIRE(plan.render.order().empty());
    REQUIRE(plan.field.order().size() == 1);
    REQUIRE(plan.field.order().front() == dipole);
    // And the bake really publishes under the key the item will look up: the node it followed the wire to, and the
    // port the wire lands on. This is the pair the item's lookup is built from, asserted here so that a change to
    // either side of it fails in one place.
    REQUIRE(scene.bake().has_value());
    REQUIRE(scene.fields.contains(gfield::FieldKey{dipole.index, FieldNodes::kPortField}));
    REQUIRE_FALSE(scene.fields.contains(gfield::FieldKey{lines.index, RenderNodes::kPortField}));
}

TEST_CASE("magnetosphere.render.a_snapshot_becomes_a_scene", "[magnetosphere]") {
    // The kit's drawing side, checked as a **value**: an item that painted would put this decision where no case
    // could reach it, which is the whole reason `ViewScene` is a value and `IViewItem` lives in `core/`.
    ParticleViewItem item;
    REQUIRE(item.name() == std::string_view{"particles"});
    REQUIRE(item.draws(RenderNodes::kParticlesType));
    REQUIRE_FALSE(item.draws("render.field_lines"));

    // A declaration, a run and a snapshot: the request is what a host has after pressing Run.
    Scene scene;
    const Chain chain = add_chain(scene, 0.0, 6.6, 8, 0.01, 90.0);
    const graph::NodeId node = scene.add(RenderNodes::kParticlesType);
    scene.wire(chain.pusher, PusherNodes::kPortStateOut, node, RenderNodes::kPortState);
    REQUIRE(scene.bake().has_value());

    MagnetosphereRunProvider provider;
    qp::graph::execution::RunBuildResult built = provider.build(scene.g, scene.host.node_types());
    REQUIRE(built.ok());
    REQUIRE(built.run->advance(500, 0.01).has_value());
    const std::vector<double> positions = built.run->positions();
    REQUIRE(positions.size() == 24);

    const std::vector<graph::DeclaredOutput> declared{graph::DeclaredOutput{node, RenderNodes::kPortItem}};
    const graph::ViewRequest request{&scene.g, &declared, &positions, 500};
    REQUIRE(request.valid());

    const graph::ViewScene drawn = item.scene(request);
    REQUIRE(drawn.points.size() == 8);
    // Earth radii, not metres: the conversion happens in `MagnetosphereRun::positions`, so an item that
    // converted again would put the unit system in a second place.
    for (const graph::ViewScene::Point& point : drawn.points) {
        const double radius = std::sqrt(point.x * point.x + point.y * point.y);
        REQUIRE(radius > 6.0);
        REQUIRE(radius < 7.5);
    }
    // The fit is symmetric about the Earth and carries a margin, so a particle on the boundary has somewhere to
    // go before the frame has to change.
    REQUIRE(drawn.has_bounds);
    REQUIRE(drawn.x_min == -drawn.x_max);
    REQUIRE(drawn.y_min == -drawn.y_max);
    REQUIRE(drawn.x_max > 6.0);
    REQUIRE(drawn.x_max < 8.0);
    // No polylines: a history is a series of snapshots and a request carries one. The host accumulates them; an
    // item that remembered them would be stateful, which the interface forbids. The particle item's shape is
    // "points and no curves", which is what makes the field-line item's shape a different answer rather than a
    // different field of the same struct.
    REQUIRE(drawn.polylines.empty());

    // A window that has not run yet: a valid request with an empty snapshot is an empty scene, not a failure.
    const std::vector<double> none;
    const graph::ViewScene blank = item.scene(graph::ViewRequest{&scene.g, &declared, &none, 0});
    REQUIRE(blank.empty());
    REQUIRE_FALSE(blank.has_bounds);
    // And no snapshot at all is refused by `valid()`, which is a host's check rather than an item's.
    REQUIRE_FALSE(graph::ViewRequest{&scene.g, &declared, nullptr, 0}.valid());
}


TEST_CASE("magnetosphere.render.a_field_becomes_a_family_of_curves", "[magnetosphere]") {
    // The second view item, end to end: a graph declares "draw this field", the run bakes it, and the item turns
    // the baked table into curves. Every mechanism this feature needed is on the path here -- the declaration, the
    // wire it follows, the store the run published, and the tracer -- which is why it is one case and not four.
    //
    // It also covers the **field-only run**: this graph has a dipole and a render item and no particles at all,
    // which is the simplest picture the kit can draw and which used to be refused as `no_pusher`.
    Scene scene;
    const graph::NodeId dipole = scene.add_dipole(0.0);
    const graph::NodeId declaration = scene.add(RenderNodes::kFieldLinesType);
    scene.set(declaration, RenderNodes::kPortLineCount, 4.0);
    scene.set(declaration, RenderNodes::kPortSeedStart, 2.0);
    scene.set(declaration, RenderNodes::kPortSeedEnd, 5.0);
    scene.set(declaration, RenderNodes::kPortStepMax, 0.2);
    scene.set(declaration, RenderNodes::kPortTolerance, 1.0e-4);
    scene.wire(dipole, FieldNodes::kPortField, declaration, RenderNodes::kPortField);

    MagnetosphereRunProvider provider;
    qp::graph::execution::RunBuildResult built = provider.build(scene.g, scene.host.node_types());
    // A graph with no pusher is **built**, not refused: the bake is the whole of what it asked for.
    REQUIRE(built.ok());
    REQUIRE(built.run->advance(500, 0.01).has_value());
    REQUIRE(built.run->positions().empty());
    const qp::graph::execution::GraphRunReport report = built.run->report();
    REQUIRE(report.is_consistent());
    REQUIRE(report.particles == 0);
    REQUIRE(report.note == std::string{"field baked, no particles declared"});
    // And the field it baked is reachable through the interface the header says was reopened for exactly this.
    REQUIRE(built.run->fields().size() == 1);
    REQUIRE(built.run->fields().contains(gfield::FieldKey{dipole.index, FieldNodes::kPortField}));

    const std::vector<graph::DeclaredOutput> declared{
        graph::DeclaredOutput{declaration, RenderNodes::kPortFieldItem}};
    const std::vector<double> no_particles;
    const graph::ViewRequest request{&scene.g, &declared, &no_particles, 500, &built.run->fields()};
    REQUIRE(request.valid());

    FieldLinesViewItem item;
    REQUIRE(item.draws(RenderNodes::kFieldLinesType));
    REQUIRE_FALSE(item.draws(RenderNodes::kParticlesType));
    REQUIRE(item.name() == std::string_view{"field lines"});

    const graph::ViewScene drawn = item.scene(request);
    // One curve per seed, and none of them empty: a seed whose trace produced nothing would be a missing line in
    // a picture whose whole content is the nesting.
    REQUIRE(drawn.polylines.size() == 4);
    for (const std::vector<graph::ViewScene::Point>& curve : drawn.polylines) {
        REQUIRE(curve.size() > 10);
    }

    // **The projection is the meridional plane**, and this is what proves it: each curve starts and ends on the
    // surface at `r = 1`, so its endpoints are at `(x, z)` with `x^2 + z^2 = 1`, and it reaches its widest `x` at
    // `z = 0` -- the equator -- with that widest value being the seed. An item that had projected `(x, y)` would
    // produce curves with `y = 0` everywhere and a degenerate vertical line here.
    std::vector<double> widest;
    for (const std::vector<graph::ViewScene::Point>& curve : drawn.polylines) {
        const graph::ViewScene::Point& first = curve.front();
        const graph::ViewScene::Point& last = curve.back();
        REQUIRE(std::abs(std::hypot(first.x, first.y) - 1.0) < 1.0e-9);
        REQUIRE(std::abs(std::hypot(last.x, last.y) - 1.0) < 1.0e-9);
        REQUIRE(first.y * last.y < 0.0);
        double far = 0.0;
        for (const graph::ViewScene::Point& point : curve) far = std::max(far, point.x);
        widest.push_back(far);
    }
    // Ascending, because the seeds are spread along `+x`: the family is nested rather than tangled.
    for (std::size_t i = 1; i < widest.size(); ++i) {
        REQUIRE(widest[i] > widest[i - 1]);
    }
    // And each curve's widest point is its own seed, to the accuracy of a 0.25 R_E table -- the statement that the
    // picture is of the field the graph asked for and not of some other shell.
    REQUIRE(std::abs(widest[0] - 2.0) < 0.05);
    REQUIRE(std::abs(widest[3] - 5.0) < 0.05);

    // The frame is the seeds and not the curves: the same graph draws the same axes, and the outermost seed plus
    // the item's own margin is what decides them.
    REQUIRE(drawn.has_bounds);
    REQUIRE(drawn.x_min == -drawn.x_max);
    REQUIRE(drawn.y_min == -drawn.y_max);
    REQUIRE(std::abs(drawn.x_max - 5.0 * FieldLinesViewItem::kFitMargin) < 1.0e-12);

    // Three ways to have nothing to draw, each answered with an empty scene rather than a failure: no run yet (no
    // store at all), a store that does not hold the field the wire names, and a declaration this item does not
    // draw. The third is the one a host relies on to offer a declaration to every item it has.
    REQUIRE(item.scene(graph::ViewRequest{&scene.g, &declared, &no_particles, 0}).empty());
    const qp::graph::field::FieldSet empty_store;
    const graph::ViewRequest not_baked{&scene.g, &declared, &no_particles, 0, &empty_store};
    REQUIRE(item.scene(not_baked).empty());
    const std::vector<graph::DeclaredOutput> other{
        graph::DeclaredOutput{dipole, FieldNodes::kPortField}};
    const graph::ViewRequest wrong_declaration{&scene.g, &other, &no_particles, 0, &built.run->fields()};
    REQUIRE(item.scene(wrong_declaration).empty());
}

TEST_CASE("magnetosphere.run.the_recorded_channels_are_the_ones_a_report_names", "[magnetosphere]") {
    // **The trace, and why it is the platform's point rather than a convenience.** Until this existed a run of this
    // kit produced a report, a position snapshot and a field store -- and **no record over time**, which meant no
    // measurements, no uncertainty and no provenance for either: the closed loop the platform is built around was
    // dead for its flagship experiment. What is asserted here is therefore in three parts: the channels exist and
    // are named and dimensioned, the physics they carry is the physics this configuration actually has, and the
    // samples belong to a run identity some ledger issued.
    Scene scene;
    const Chain chain = add_chain(scene, 0.0, 6.6, 4, 0.01, 90.0);

    MagnetosphereRunProvider provider;
    qp::graph::execution::RunBuildResult built = provider.build(scene.g, scene.host.node_types());
    REQUIRE(built.ok());
    // Not recorded until somebody says which record this is: a trace whose samples nobody can look up is not a
    // record, and `Trace` takes its identity at construction for exactly that reason.
    built.run->set_run(qp::runtime::RunId{11});
    REQUIRE(built.run->advance(600, 0.01).has_value());

    const qp::runtime::Trace& record = built.run->trace();
    REQUIRE(record.run() == qp::runtime::RunId{11});
    REQUIRE(record.channels().size() == 4);
    // By **name**, because that is how every consumer finds a channel: the confidence and measurement models
    // locate theirs by name and never by position, so a channel inserted at the front must not rename the rest.
    const auto channel = [&record](const char* name) -> const qp::runtime::Channel* {
        for (const qp::runtime::Channel& candidate : record.channels()) {
            if (candidate.name == name) return &candidate;
        }
        return nullptr;
    };
    REQUIRE(channel(MagnetosphereRun::kSpeedChannel) != nullptr);
    REQUIRE(channel(MagnetosphereRun::kEnergyChannel) != nullptr);
    REQUIRE(channel(MagnetosphereRun::kMuChannel) != nullptr);
    REQUIRE(channel(MagnetosphereRun::kRadiusChannel) != nullptr);
    REQUIRE(channel(MagnetosphereRun::kSpeedChannel)->dim == qp::units::dims::velocity);
    REQUIRE(channel(MagnetosphereRun::kEnergyChannel)->dim == qp::units::dims::energy);
    REQUIRE(channel(MagnetosphereRun::kMuChannel)->dim == qp::units::dims::magnetic_dipole);
    REQUIRE(channel(MagnetosphereRun::kRadiusChannel)->dim == qp::units::dims::length);
    // Provenance: every channel names the node the samples came from, which is the question a reader asks after
    // "which run" and the one a window needs in order to point at the thing a reading came from.
    REQUIRE(channel(MagnetosphereRun::kRadiusChannel)->source.valid());
    REQUIRE(channel(MagnetosphereRun::kRadiusChannel)->source.index == chain.pusher.index);

    // `steps + 1` samples: the initial condition plus one per step, the shape `GraphRun` produces and the shape a
    // reader assumes -- the first number in a column is where the experiment started.
    REQUIRE(record.size() == 601);
    REQUIRE(record.is_consistent());

    // -- the physics -----------------------------------------------------------------------------------------
    const auto read = [&record](std::size_t index, const char* name) {
        for (std::size_t slot = 0; slot < record.channels().size(); ++slot) {
            if (record.channels()[slot].name != name) continue;
            const auto value = record.value_at(static_cast<std::uint64_t>(index), slot);
            REQUIRE(value.has_value());
            return value->value;
        }
        return 0.0;
    };

    // **A magnetic force does no work, so the speed is exactly conserved by the motion** and every change in it is
    // the integrator's. Measured over six hundred steps at beta = 0.01: the relative change is below 1e-12, which
    // is rounding rather than truncation -- and that is the number a reader needs to tell "the run converged" from
    // "the run drifted".
    const double speed_first = read(0, MagnetosphereRun::kSpeedChannel);
    const double speed_last = read(record.size() - 1, MagnetosphereRun::kSpeedChannel);
    REQUIRE(speed_first > 0.0);
    REQUIRE(std::abs(speed_last - speed_first) / speed_first < 1.0e-12);
    // And the energy channel is the same statement in the units a report is written in: `m v^2 / 2` at the
    // proton's mass. Asserted against the closed form rather than against the speed channel, so a unit slip in
    // either conversion shows up as a disagreement between two independent expressions.
    const double energy_first = read(0, MagnetosphereRun::kEnergyChannel);
    REQUIRE(std::abs(energy_first - 0.5 * kProtonMassKg * speed_first * speed_first) / energy_first < 1.0e-12);
    // A tenth of the speed of light: beta = 0.01 is what the chain launched with, and a channel in metres per
    // second is what tells a reader that the SI conversion happened at the boundary.
    REQUIRE(std::abs(speed_first - 0.01 * kSpeedOfLightSI) / speed_first < 1.0e-3);

    // **The first adiabatic invariant drifts, and that is physics rather than error.** `mu` is conserved when the
    // field varies slowly over a gyro-orbit; a ring launched at L = 6.6 with a 90-degree pitch angle has a
    // gyro-radius small compared with the field's scale, so mu is conserved to a few parts in ten thousand -- and
    // the case asserts the order rather than a tight bound, because a tight bound would be asserting that the
    // adiabatic approximation is exact, which it is not and which this platform should never claim.
    const double mu_first = read(0, MagnetosphereRun::kMuChannel);
    const double mu_last = read(record.size() - 1, MagnetosphereRun::kMuChannel);
    const double radius_first = read(0, MagnetosphereRun::kRadiusChannel);
    REQUIRE(mu_first > 0.0);
    REQUIRE(radius_first > 0.0);

    // **The bound is derived, not chosen.** `mu` is conserved when the field varies slowly over a gyro-orbit, so
    // its drift is set by the ratio of the gyro-radius to the field's scale length -- here `rho / L`, with
    // `rho = v / omega_c` and `omega_c = (q/m) B` at the equator. Measured on this configuration: `rho/L` is
    // 7.2e-3 and the drift over six hundred steps is **1.55e-2**, a factor of about two. Asserting the band around
    // that prediction is asserting the *mechanism*; the first version of this case asserted 1e-3, which is
    // asserting that the adiabatic approximation is exact -- which it is not, and which this platform must never
    // claim.
    const double b_equator = kEquatorialSurfaceFieldT / (6.6 * 6.6 * 6.6);
    const double gyro_radius_ratio =
        (speed_first / (kProtonChargeMassSI * b_equator)) / radius_first;
    const double mu_drift = std::abs(mu_last - mu_first) / mu_first;
    REQUIRE(gyro_radius_ratio > 0.0);
    REQUIRE(gyro_radius_ratio < 0.05);
    REQUIRE(mu_drift > 0.5 * gyro_radius_ratio);
    REQUIRE(mu_drift < 5.0 * gyro_radius_ratio);
    // And the statement that separates the physics from the arithmetic: the **speed** is conserved nine orders of
    // magnitude better than `mu` drifts. A run whose speed drifted like this has a broken integrator; a run whose
    // `mu` did not drift at all has a broken diagnostic.
    REQUIRE(mu_drift > 1.0e9 * (std::abs(speed_last - speed_first) / speed_first));

    // The radius channel is in **metres**, which is what makes it readable by a measurement session whose dataset
    // is a length: the kit converts at its boundary and a session in metres must be able to take a reading from it.
    REQUIRE(std::abs(radius_first / kEarthRadiusM - 6.6) < 0.2);
    // A 90-degree pitch angle means the whole velocity is perpendicular, so `mu` and the energy are related:
    // `mu = E / B` exactly at the launch, and `B` at the equator of a tilted dipole is the model's own surface
    // field over `L^3`. Asserting the relation rather than either number is what makes this a check on the
    // *diagnostic* and not on the field model, which has its own cases.
    REQUIRE(std::abs(mu_first - energy_first / b_equator) / mu_first < 0.01);
}

TEST_CASE("magnetosphere.run.the_cadence_resolves_the_gyration", "[magnetosphere]") {
    // **The measurement that made this widening necessary, and the before/after that justifies it.** The window's
    // cadence -- 4096 steps of `1e-4`, chosen and measured for a laboratory oscillator -- is 8.7 **milliseconds**
    // here, because one normalized time unit is the light crossing time of an earth radius. A proton's gyro-period
    // at six earth radii is 0.63 seconds, so that run covers one seventy-third of a single gyration: the ring is
    // drawn almost exactly where it started, and nothing dynamic can be seen.
    //
    // Two runs over the same graph make that concrete rather than rhetorical: the same particles, the same field,
    // the two cadences, and the **range of the recorded radius channel** as the measure of whether the run was
    // about anything.
    Scene scene;
    const Chain chain = add_chain(scene, 0.0, 6.6, 4, 0.01, 90.0);
    MagnetosphereRunProvider provider;

    const auto radius_range = [](const qp::runtime::Trace& record) {
        double lowest = std::numeric_limits<double>::max();
        double highest = std::numeric_limits<double>::lowest();
        for (const qp::runtime::Sample& sample : record.samples()) {
            for (std::size_t slot = 0; slot < record.channels().size(); ++slot) {
                if (record.channels()[slot].name != std::string{MagnetosphereRun::kRadiusChannel}) continue;
                lowest = std::min(lowest, sample.values[slot].value);
                highest = std::max(highest, sample.values[slot].value);
            }
        }
        return highest - lowest;
    };

    // The window's cadence, which the operator path measures and keeps. **Spelled out as literals**, because a
    // plugin may not include the view layer -- `RunController` is where these two numbers live and this partition
    // cannot name it. That the kit has to spell them out is the point of the whole case: the window's cadence is
    // not something the kit knows, which is exactly why the *run* states its own.
    qp::graph::execution::RunBuildResult short_run = provider.build(scene.g, scene.host.node_types());
    REQUIRE(short_run.ok());
    short_run.run->set_run(qp::runtime::RunId{21});
    REQUIRE(short_run.run->advance(4096, 1.0e-4).has_value());
    const double short_range = radius_range(short_run.run->trace());

    // And the run's own, which resolves the gyration.
    qp::graph::execution::RunBuildResult long_run = provider.build(scene.g, scene.host.node_types());
    REQUIRE(long_run.ok());
    const std::optional<qp::graph::execution::RunCadence> cadence = long_run.run->preferred_cadence();
    REQUIRE(cadence.has_value());
    REQUIRE(cadence->steps > 0);
    REQUIRE(cadence->dt > 0.0);

    // The step size is one thirty-second of the gyro-period at the launch radius, computed here from the same
    // physics the kit used -- the species' charge-to-mass ratio and the field sampled at `L`:
    // `omega_c = |q/m| B(L)`, `T = 2 pi / omega_c`, and the normalized step is `T / 32` in light-crossing units.
    const double b_at_launch = kEquatorialSurfaceFieldT / (6.6 * 6.6 * 6.6);
    const double gyro_period_s = 2.0 * 3.14159265358979323846 / (kProtonChargeMassSI * b_at_launch);
    REQUIRE(std::abs(cadence->dt - gyro_period_s / 32.0 * kNormalizedPerSecond) / cadence->dt < 0.02);
    // The run it proposes is 128 gyrations -- 81 seconds -- rather than 8.7 milliseconds.
    const double total_s = static_cast<double>(cadence->steps) * cadence->dt / kNormalizedPerSecond;
    REQUIRE(total_s > 60.0);
    REQUIRE(total_s < 120.0);
    REQUIRE(std::abs(total_s / (128.0 * gyro_period_s) - 1.0) < 0.02);

    long_run.run->set_run(qp::runtime::RunId{22});
    REQUIRE(long_run.run->advance(cadence->steps, cadence->dt).has_value());
    const double long_range = radius_range(long_run.run->trace());

    // **The before and after.** Twenty-six kilometres of travel against a gyro-radius of three hundred: the short
    // run's radius channel is flat to a few kilometres, and the long run's swings by the full two gyro-radii.
    REQUIRE(short_range < 5.0e3);
    REQUIRE(long_range > 1.0e5);
    REQUIRE(long_range > 100.0 * short_range);

    // A graph with no particles to gyrate has no opinion, and says so rather than inventing a cadence: the
    // field-only run of the kit's own case is the example.
    Scene field_only_scene;
    field_only_scene.add_dipole(0.0);
    qp::graph::execution::RunBuildResult field_only =
        provider.build(field_only_scene.g, field_only_scene.host.node_types());
    REQUIRE(field_only.ok());
    REQUIRE_FALSE(field_only.run->preferred_cadence().has_value());
}

TEST_CASE("magnetosphere.run.an_atmosphere_takes_the_speed_away", "[magnetosphere]") {
    // **The first dissipative force in this kit, and the first thing here whose decay an experiment can measure.**
    // Every other force is conservative -- a magnetic field does no work at all, an electric field gives back what
    // it takes -- so this is the case where the trace stops being a diagnostic and becomes a measurement: the speed
    // channel falls, and it falls by the amount the model says.
    //
    // The graph is the kit's own chain plus one wire: an atmosphere node whose rate is quoted at the surface and
    // whose scale height is far larger than the region, so the rate is **nearly** constant (under a percent across
    // the box, asserted by the atmosphere's own case). That limit is what makes a closed-form check possible:
    // `v(t) = v(0) exp(-nu t)` for a constant rate.
    Scene scene;
    const Chain chain = add_chain(scene, 0.0, 6.6, 4, 0.01, 90.0);

    const double nu0 = 0.01;   // per second
    const graph::NodeId atmosphere = scene.add(FieldNodes::kAtmosphereType);
    // Its own grid, and it must be **the dipole's**: the kernel has one grid to sample every slot with, and the plan
    // builder now refuses a graph whose bound sockets disagree. Copying the dipole's parameters is what a user does,
    // and the refusal is what tells them when they did not.
    scene.set(atmosphere, FieldNodes::kPortAtmosphereNu0, nu0);
    scene.set(atmosphere, FieldNodes::kPortAtmosphereScaleHeight, 1.0e10);
    scene.set(atmosphere, FieldNodes::kPortAtmosphereReference, kEarthRadiusM);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const double value = offset < 3 ? -8.0 * kEarthRadiusM
                                         : (offset < 6 ? 0.25 * kEarthRadiusM : 65.0);
        scene.set(atmosphere, FieldNodes::kPortAtmosphereOrigin0 + offset, value);
    }
    scene.wire(atmosphere, FieldNodes::kPortAtmosphereOut, chain.pusher, PusherNodes::kPortDrag);
    scene.set_flag(chain.pusher, PusherNodes::kPortUseDrag, true);

    MagnetosphereRunProvider provider;
    qp::graph::execution::RunBuildResult built = provider.build(scene.g, scene.host.node_types());
    // Catch2 prints the sentence when it is not empty, which is how this case reports *why* a graph the kit should
    // accept was refused.
    REQUIRE(built.refusal == std::string{});
    REQUIRE(built.ok());
    built.run->set_run(qp::runtime::RunId{31});
    const std::optional<qp::graph::execution::RunCadence> cadence = built.run->preferred_cadence();
    REQUIRE(cadence.has_value());
    REQUIRE(built.run->advance(cadence->steps, cadence->dt).has_value());

    const qp::runtime::Trace& record = built.run->trace();
    REQUIRE(record.size() == cadence->steps + 1);
    std::size_t speed_slot = record.channels().size();
    for (std::size_t slot = 0; slot < record.channels().size(); ++slot) {
        if (record.channels()[slot].name == std::string{MagnetosphereRun::kSpeedChannel}) speed_slot = slot;
    }
    REQUIRE(speed_slot < record.channels().size());

    const auto speed_at = [&record, speed_slot](std::size_t index) {
        const auto value = record.value_at(static_cast<std::uint64_t>(index), speed_slot);
        REQUIRE(value.has_value());
        return value->value;
    };
    const double first = speed_at(0);
    const double last = speed_at(record.size() - 1);
    REQUIRE(first > 0.0);
    REQUIRE(last < first);

    // **The closed form.** The elapsed time is the sample count times the step, and `nu0` is the rate the node was
    // given; the only approximation between them and the measurement is the kernel's `1 - nu dt` damping, which
    // differs from an exponential by `O((nu dt)^2)` per step -- four parts in a hundred million at this step size,
    // so a one-percent tolerance is testing the model rather than the expansion.
    const double elapsed_s = static_cast<double>(record.size() - 1) * cadence->dt / kNormalizedPerSecond;
    const double predicted = first * std::exp(-nu0 * elapsed_s);
    REQUIRE(std::abs(last - predicted) / predicted < 0.01);
    // And the decay is large enough to be a measurement rather than a rounding: 0.01 per second over eighty seconds
    // takes away more than half the speed.
    REQUIRE(last / first < 0.7);
    REQUIRE(last / first > 0.3);
    // The magnetic field still does no work, so what the drag took is the *whole* of the change: a run whose energy
    // fell for another reason would be a different finding, and this is the assertion that separates them.
    std::size_t energy_slot = record.channels().size();
    for (std::size_t slot = 0; slot < record.channels().size(); ++slot) {
        if (record.channels()[slot].name == std::string{MagnetosphereRun::kEnergyChannel}) energy_slot = slot;
    }
    REQUIRE(energy_slot < record.channels().size());
    const auto energy_at = [&record, energy_slot](std::size_t index) {
        const auto value = record.value_at(static_cast<std::uint64_t>(index), energy_slot);
        REQUIRE(value.has_value());
        return value->value;
    };
    // Energy goes as the square of the speed, so it decays at twice the rate: `exp(-2 nu t)`.
    const double predicted_energy = energy_at(0) * std::exp(-2.0 * nu0 * elapsed_s);
    REQUIRE(std::abs(energy_at(record.size() - 1) - predicted_energy) / predicted_energy < 0.01);

    // A graph that wires the atmosphere and does **not** turn the drag socket on is not dragged: the switch is the
    // reference implementation's own, and a run that quietly applied it anyway would make the node's absence mean
    // nothing.
    Scene no_switch;
    const Chain chain2 = add_chain(no_switch, 0.0, 6.6, 4, 0.01, 90.0);
    const graph::NodeId quiet = no_switch.add(FieldNodes::kAtmosphereType);
    no_switch.set(quiet, FieldNodes::kPortAtmosphereNu0, nu0);
    no_switch.set(quiet, FieldNodes::kPortAtmosphereScaleHeight, 1.0e10);
    no_switch.set(quiet, FieldNodes::kPortAtmosphereReference, kEarthRadiusM);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const double value = offset < 3 ? -8.0 * kEarthRadiusM
                                         : (offset < 6 ? 0.25 * kEarthRadiusM : 65.0);
        no_switch.set(quiet, FieldNodes::kPortAtmosphereOrigin0 + offset, value);
    }
    no_switch.wire(quiet, FieldNodes::kPortAtmosphereOut, chain2.pusher, PusherNodes::kPortDrag);
    qp::graph::execution::RunBuildResult undragged = provider.build(no_switch.g, no_switch.host.node_types());
    REQUIRE(undragged.ok());
    undragged.run->set_run(qp::runtime::RunId{32});
    REQUIRE(undragged.run->advance(cadence->steps, cadence->dt).has_value());
    const qp::runtime::Trace& steady = undragged.run->trace();
    std::size_t steady_speed = steady.channels().size();
    for (std::size_t slot = 0; slot < steady.channels().size(); ++slot) {
        if (steady.channels()[slot].name == std::string{MagnetosphereRun::kSpeedChannel}) steady_speed = slot;
    }
    REQUIRE(steady_speed < steady.channels().size());
    const auto steady_first = steady.value_at(0, steady_speed);
    const auto steady_last = steady.value_at(static_cast<std::uint64_t>(steady.size() - 1), steady_speed);
    REQUIRE(steady_first.has_value());
    REQUIRE(steady_last.has_value());
    REQUIRE(std::abs(steady_last->value - steady_first->value) / steady_first->value < 1.0e-9);
}

TEST_CASE("magnetosphere.field_nodes.a_uniform_field_is_uniform", "[magnetosphere]") {
    // The second field model, and the one whose answer is **exact** under trilinear interpolation at any
    // spacing: every node holds the same vector, so a sample anywhere is that vector to the last bit. That is
    // what makes it the field a course checks a pusher against -- there is no interpolation error between the
    // measurement and the closed form it is compared with.
    Scene scene;
    const graph::NodeId uniform = scene.add(FieldNodes::kUniformType);
    scene.set(uniform, FieldNodes::kPortField0, 0.0);
    scene.set(uniform, FieldNodes::kPortField1, 0.0);
    scene.set(uniform, FieldNodes::kPortField2, 1.0e-4);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(uniform, FieldNodes::kPortUniformOrigin0 + axis, -2.0 * kEarthRadiusM);
        scene.set(uniform, FieldNodes::kPortUniformSpacing0 + axis, 1.0 * kEarthRadiusM);
        scene.set(uniform, FieldNodes::kPortUniformCount0 + axis, 5.0);
    }
    REQUIRE(scene.host.node_types().find(FieldNodes::kUniformType) != nullptr);

    REQUIRE(scene.bake().has_value());
    REQUIRE(scene.fields.size() == 1);
    const gfield::FieldValue view = scene.fields.view(gfield::FieldKey{uniform.index, FieldNodes::kPortField});
    REQUIRE(gfield::is_readable(view));
    REQUIRE(view.kind() == gfield::Kind::Volume);
    REQUIRE(view.is_vector());
    REQUIRE(view.point_count() == 125);

    // Every node, and not only the first: a bake that wrote one value and left the rest zero would pass a
    // single-point check.
    for (std::uint64_t point = 0; point < view.point_count(); ++point) {
        REQUIRE(gfield::get_component(view, point, 0) == 0.0);
        REQUIRE(gfield::get_component(view, point, 1) == 0.0);
        REQUIRE(gfield::get_component(view, point, 2) == 1.0e-4);
    }
    // And between nodes, which is where the exactness claim lives.
    const GridSpec grid = FieldNodes::read_from(*scene.g.find_node(uniform), FieldNodes::kPortUniformOrigin0);
    const Vec3 middle = grid.node_position(1, 1, 1) + grid.spacing_m * 0.5;
    const Vec3 sampled = sample_baked(view, grid.origin_m, grid.spacing_m, middle);
    // **Relative**, and the exactness claim is why: every node holds the same value, so the blend of eight equal
    // terms is that value up to the order the eight products are summed in -- which is a rounding, not an
    // interpolation error. At a node the read is exact bit for bit (the test above asserts that); between nodes
    // it is exact to the arithmetic, and that is the distinction a "uniform field" test exists to make.
    REQUIRE(relative(sampled.z, 1.0e-4) < 1.0e-15);
    REQUIRE(sampled.x == 0.0);

    // The two field types are independent node types with independent ports: a graph may hold both, and each
    // publishes under its own key -- which is what a sum of the two will need.
    const graph::NodeId dipole = scene.add_dipole(0.0);
    REQUIRE(scene.bake().has_value());
    REQUIRE(scene.fields.size() == 2);
    REQUIRE(scene.fields.contains(gfield::FieldKey{dipole.index, FieldNodes::kPortField}));
    REQUIRE(scene.fields.contains(gfield::FieldKey{uniform.index, FieldNodes::kPortField}));

    // **The electric field, which is the same table and a different quantity.** The pusher has had an electric
    // socket since it was written and nothing produced a field for it; this node is that production, and what
    // distinguishes it from the magnetic bake is exactly one thing -- the dimension in the description.
    const graph::NodeId electric = scene.add(FieldNodes::kUniformElectricType);
    scene.set(electric, FieldNodes::kPortE0, 0.0);
    scene.set(electric, FieldNodes::kPortE1, 1.0e-3);
    scene.set(electric, FieldNodes::kPortE2, 0.0);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(electric, FieldNodes::kPortElectricOrigin0 + axis, -2.0 * kEarthRadiusM);
        scene.set(electric, FieldNodes::kPortElectricOrigin0 + 3 + axis, 1.0 * kEarthRadiusM);
        scene.set(electric, FieldNodes::kPortElectricOrigin0 + 6 + axis, 5.0);
    }
    REQUIRE(scene.bake().has_value());
    const gfield::FieldValue volts =
        scene.fields.view(gfield::FieldKey{electric.index, FieldNodes::kPortField});
    REQUIRE(gfield::is_readable(volts));
    REQUIRE(volts.point_count() == 125);
    // Volts per metre: kg m / (A s^3). The magnetic table beside it is kg / (A s^2), and the two descriptions
    // differ in exactly the exponents that tell them apart -- which is what the dimension parameter is for.
    REQUIRE(volts.dimension().M == 1);
    REQUIRE(volts.dimension().L == 1);
    REQUIRE(volts.dimension().T == -3);
    REQUIRE(volts.dimension().I == -1);
    REQUIRE(view.dimension().L == 0);
    REQUIRE(view.dimension().T == -2);
    for (std::uint64_t point = 0; point < volts.point_count(); ++point) {
        REQUIRE(gfield::get_component(volts, point, 0) == 0.0);
        REQUIRE(gfield::get_component(volts, point, 1) == 1.0e-3);
        REQUIRE(gfield::get_component(volts, point, 2) == 0.0);
    }

    // A non-finite component is refused rather than baked: a field table full of NaN is a run that produces
    // NaN positions with nothing said.
    gfield::FieldSet refused;
    REQUIRE_FALSE(bake_uniform(Vec3{0.0, 0.0, std::nan("")}, grid, gfield::FieldKey{1, 1}, refused));
    REQUIRE(refused.size() == 0);
}


TEST_CASE("magnetosphere.field_nodes.two_fields_add_where_the_graph_says", "[magnetosphere]") {
    // **The composition principle as a node.** The reference implementation states it: each node outputs one
    // independent field and composition happens by wiring. A sum node is what makes that a sentence a graph can
    // say, and this case is what makes the sentence checkable -- the sum at every node is the two addends added,
    // and the addends were reached through their **wires**, not through their descriptions.
    //
    // That last part is the interesting one: `ports::Value`'s field handle carries a `LatticeDesc` and no key, so
    // an evaluator cannot find its inputs' samples in the store unless it can resolve `(node, port)`. It does
    // that by borrowing the graph, which is what `set_graph` is for and what this case exercises.
    Scene scene;
    const graph::NodeId dipole = scene.add_dipole(0.0);
    const graph::NodeId uniform = scene.add(FieldNodes::kUniformType);
    scene.set(uniform, FieldNodes::kPortField0, 0.0);
    scene.set(uniform, FieldNodes::kPortField1, 0.0);
    scene.set(uniform, FieldNodes::kPortField2, 1.0e-4);
    // The **same lattice** as the dipole's, which the sum requires: two tables that disagree about their
    // geometry are refused rather than fitted.
    // The **same lattice as the dipole's**, which is 0.25 earth radii and 65 nodes: two tables that disagree
    // about their geometry are not a sum, and the first version of this case set a one-radii grid here and got a
    // refused bake -- which is the refusal working.
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(uniform, FieldNodes::kPortUniformOrigin0 + axis, -8.0 * kEarthRadiusM);
        scene.set(uniform, FieldNodes::kPortUniformSpacing0 + axis, 0.25 * kEarthRadiusM);
        scene.set(uniform, FieldNodes::kPortUniformCount0 + axis, 65.0);
    }
    const graph::NodeId sum = scene.add(FieldNodes::kSumType);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(sum, FieldNodes::kPortOrigin0 + axis, -8.0 * kEarthRadiusM);
        scene.set(sum, FieldNodes::kPortSpacing0 + axis, 0.25 * kEarthRadiusM);
        scene.set(sum, FieldNodes::kPortCount0 + axis, 65.0);
    }
    scene.wire(dipole, FieldNodes::kPortField, sum, FieldNodes::kPortAddendA);
    scene.wire(uniform, FieldNodes::kPortField, sum, FieldNodes::kPortAddendB);

    REQUIRE(scene.bake().has_value());
    REQUIRE(scene.fields.size() == 3);

    const gfield::FieldValue left = scene.fields.view(gfield::FieldKey{dipole.index, FieldNodes::kPortField});
    const gfield::FieldValue right = scene.fields.view(gfield::FieldKey{uniform.index, FieldNodes::kPortField});
    const gfield::FieldValue total = scene.fields.view(gfield::FieldKey{sum.index, FieldNodes::kPortField});
    REQUIRE(gfield::is_readable(total));
    REQUIRE(total.point_count() == left.point_count());
    REQUIRE(total.desc.count[0] == 65);

    // Every node, not a sample: a sum that wrote one correct entry would pass a spot check.
    for (std::uint64_t point = 0; point < total.point_count(); ++point) {
        for (std::uint32_t component = 0; component < 3; ++component) {
            const double expected = gfield::get_component(left, point, component) +
                                    gfield::get_component(right, point, component);
            REQUIRE(gfield::get_component(total, point, component) == expected);
        }
    }

    // The sum is a **field of its own**, under its own key, and the addends are untouched: a node that wrote into
    // an input's buffer would make a graph's second reader see the first reader's arithmetic.
    REQUIRE(scene.fields.contains(gfield::FieldKey{sum.index, FieldNodes::kPortField}));
    const std::uint64_t probe = (8ULL * 17 + 8) * 17 + 8;
    REQUIRE(gfield::get_component(left, probe, 2) < 0.0);          // the dipole alone, still southward
    REQUIRE(gfield::get_component(total, probe, 2) > 0.0);         // and the sum has lifted it: 1e-4 beats 3e-5

    // Two addends that disagree about their geometry are refused rather than fitted, and the refusal is visible
    // as a bake that fails rather than as a plausible field.
    Scene mismatched;
    const graph::NodeId small = mismatched.add_dipole(0.0);
    const graph::NodeId coarse = mismatched.add(FieldNodes::kUniformType);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        mismatched.set(coarse, FieldNodes::kPortUniformOrigin0 + axis, -8.0 * kEarthRadiusM);
        mismatched.set(coarse, FieldNodes::kPortUniformSpacing0 + axis, 1.0 * kEarthRadiusM);
        mismatched.set(coarse, FieldNodes::kPortUniformCount0 + axis, 9.0);   // nine nodes, not sixty-five
    }
    const graph::NodeId bad_sum = mismatched.add(FieldNodes::kSumType);
    mismatched.wire(small, FieldNodes::kPortField, bad_sum, FieldNodes::kPortAddendA);
    mismatched.wire(coarse, FieldNodes::kPortField, bad_sum, FieldNodes::kPortAddendB);
    REQUIRE_FALSE(mismatched.bake().has_value());
    REQUIRE_FALSE(mismatched.fields.contains(gfield::FieldKey{bad_sum.index, FieldNodes::kPortField}));
}
