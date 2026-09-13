/**
 * @file emitter.hpp
 * @brief The node that says where the particles start, and the code that expands it into a state.
 *
 * ## Why an emitter is a node and not a function argument
 *
 * A run's initial condition is part of the experiment: a ring at six earth radii with a given pitch angle is one
 * lesson, and the same ring at four with a different pitch angle is another. Making it a node means the condition
 * is saved with the graph (a student opens the experiment and sees what was launched), shown in the property
 * panel (the numbers are visible rather than buried in a call), and **checked by the same machinery as everything
 * else** -- the port table says what a ring needs, and a value that is not a number is refused where the user can
 * see it.
 *
 * ## Why the ring is launched against the **field** and not against an axis
 *
 * The pitch angle is the angle between the particle's velocity and the **local magnetic field**, which is what
 * makes it the quantity a loss-cone argument is about. So the emitter has a `magnetic` socket, wired to the same
 * field node the pusher reads, and it samples that field at each launch point. The alternative -- measuring the
 * angle against `+z` -- would be a different physical quantity wearing the same name, and it would be wrong by
 * exactly the tilt wherever the two differ.
 *
 * That is also why the emitter is the second caller of `sample_baked`: it has to know the local field direction,
 * and two trilinear samplers in one kit would be two answers to "what is the field between two nodes".
 *
 * ## The channel a state travels on
 *
 * The emitter declares an output port of type `kParticleBuffer`, and the pusher declares an input of the same
 * type. **No value ever crosses it.** The particle domain's product is the `ParticleState`, which lives in the
 * executor and is far too large to be a `ports::Value` -- the port type exists so the *wire* can be drawn, type
 * checked and seen by `graph/domain`'s reachability walk, and so the run can ask the graph "which emitter feeds
 * this pusher" instead of guessing by looking for an emitter node somewhere. The same decision the platform
 * already made for declared outputs applies here: topology is data, and a value is not always what a wire
 * carries.
 *
 * @ownership   observes the graph; owns nothing
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No particle state is ever produced as a port value
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads,
 *              magnetosphere.emitter.a_ring_is_launched_at_the_pitch_angle_it_asks_for
 */
#pragma once

#include <qp/graph/field/field_set.hpp>
#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/ir/node.hpp>
#include <qp/graph/particles/particle_state.hpp>
#include <qp/host/host.hpp>

#include <cstdint>
#include <vector>

#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

namespace qp::plugins::magnetosphere {

/**
 * @brief The initial condition one emitter node describes, in SI.
 *
 * A value rather than a live reference into the node, for the reason `GridSpec` is one: two callers read it (the
 * run that expands it, and a test that checks the arithmetic) and neither wants to hold the graph open.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `usable()` is true exactly when every field is a number a run can use
 * @errors      See `usable`
 * @frozen      no
 * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads
 */
struct EmitterSpec final {
    /// The species' charge-to-mass ratio, in coulombs per kilogram. Negative for an electron.
    double charge_mass_si = kProtonChargeMassSI;
    /// The equatorial distance of the ring, in earth radii.
    double l_shell_re = 6.6;
    /// How many particles to launch.
    std::uint32_t count = 64;
    /// The speed, as a fraction of `c`.
    double beta = 0.01;
    /// The angle between the velocity and the **local** field, in degrees.
    double pitch_angle_deg = 90.0;
    /// Where the velocity points in the plane perpendicular to the field, in degrees from `r_hat` towards
    /// `phi_hat`.
    ///
    /// 90 degrees is a **ring current**: every particle moves azimuthally, which is the initial condition the
    /// radiation-belt lesson is about. 0 is a radial outflow and 180 a radial inflow.
    ///
    /// A separate angle from the pitch angle because they answer different questions -- how much of the motion is
    /// along the field, and which way the rest of it points -- and because tying this angle to the particle's
    /// **position** on the ring is a trap: the local `(r_hat, phi_hat)` frame rotates with the position, so a
    /// flow angle that advanced with it would come out the same global direction for every particle. The first
    /// version of this emitter did exactly that, and launched a translating ring instead of a gyrotropic one.
    double flow_angle_deg = 90.0;
    /// How much of a full turn the population's **positions** span.
    ///
    /// 360 degrees is a closed ring; 90 is an arc, which is what a partial-ring or a "one sector" demonstration
    /// wants.
    double ring_span_deg = 360.0;

    /**
     * @brief Whether every number here is one a run can use.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        True exactly when the charge-to-mass ratio is finite and non-zero, the shell is finite and
     *              positive, the speed is finite and in `[0, 1)`, every angle is finite, and the count is
     *              between 1 and the cap
     * @invariant   A spec that answers true cannot produce a particle whose state is not a number
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.emitter.a_ring_is_launched_at_the_pitch_angle_it_asks_for
     */
    [[nodiscard]] bool usable() const noexcept;
};

/**
 * @brief The ring-emitter node type and the arithmetic that expands it.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every port number here is one the descriptions in `node_types` declare
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads
 */
class EmitterNodes final {
public:
    /// @brief The ring emitter: a population on an equatorial circle, launched at a pitch angle.
    static constexpr const char* kRingType = "particle.ring_emitter";

    /// @brief The species' charge-to-mass ratio, in coulombs per kilogram. An enum: proton, electron, alpha.
    static constexpr qp::graph::PortNumber kPortSpecies = 1;
    /// @brief The equatorial distance of the ring, in earth radii.
    static constexpr qp::graph::PortNumber kPortLShell = 2;
    /// @brief How many particles.
    static constexpr qp::graph::PortNumber kPortCount = 3;
    /// @brief The speed as a fraction of `c`.
    static constexpr qp::graph::PortNumber kPortBeta = 4;
    /// @brief The pitch angle, in degrees.
    static constexpr qp::graph::PortNumber kPortPitchAngle = 5;
    /// @brief Where the velocity points perpendicular to the field, in degrees.
    static constexpr qp::graph::PortNumber kPortFlowAngle = 6;
    /// @brief How much of a full turn the positions span, in degrees.
    static constexpr qp::graph::PortNumber kPortRingSpan = 7;
    /// @brief The field the pitch angle is measured against. A `kVectorField` socket.
    static constexpr qp::graph::PortNumber kPortMagnetic = 8;
    /// @brief The state this emitter produces -- a wire, not a value. See the file comment.
    static constexpr qp::graph::PortNumber kPortState = 1;

    /// @brief The most particles one emitter may launch.
    ///
    /// A million: four slots of f64 each is 32 MB, which is a large run and not an absurd one. The cap is here
    /// for the same reason the bake grid's is: one digit too many asks for an allocation no machine has, and the
    /// vector's answer to that is to throw, which on this path terminates the process.
    static constexpr std::uint32_t kMaxCount = 1U << 20;

    /// @brief The species a choice index names: 0 proton, 1 electron, 2 alpha.
    ///
    /// A table rather than a switch, because the *order* is what the property panel shows and what a saved
    /// document stores: an index whose meaning moved when somebody reordered a switch would silently change the
    /// species of every experiment already written down.
    static constexpr std::uint32_t kSpeciesCount = 3;

    EmitterNodes() = delete;

    /**
     * @brief The charge-to-mass ratio of species `index`, in coulombs per kilogram.
     *
     * @param index The choice index. Out of range takes the proton, which is the default a fresh node has.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        One of the three published ratios
     * @invariant   `charge_mass_of(0) == kProtonChargeMassSI`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads
     */
    [[nodiscard]] static double charge_mass_of(std::int64_t index) noexcept;

    /**
     * @brief The mass of species `index`, in kilograms.
     *
     * **Why a second table when the first one is a ratio.** The pusher needs `q/m` and nothing else: a Boris push
     * divides by the mass in every stage, so the ratio is exactly the quantity it wants and carrying the pair
     * separately would be two numbers where one is used. The **measurement chain** needs the mass: an energy is
     * `m v^2 / 2` and the first adiabatic invariant is `m v_perp^2 / 2B`, and a trace channel that reported either
     * of those with the mass missing would be reporting a quantity the kit does not have. So the ratio stays where
     * the kernel reads it and the mass joins it here, beside the same index, from the same table order -- and the
     * index is asserted by the same test, because "which species is 2" has to mean one thing.
     *
     * @param index The choice index. Out of range takes the proton, as `charge_mass_of` does.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        One of the three published masses, in kilograms
     * @invariant   `mass_of(0) == kProtonMassKg`, and `charge_mass_of(i) * mass_of(i)` is the species' charge
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads
     */
    [[nodiscard]] static double mass_of(std::int64_t index) noexcept;

    /**
     * @brief The node types this build ships, ready to register with a host.
     *
     * @ownership   owns the returned descriptions
     * @thread      main
     * @pre         none
     * @post        One description for the ring emitter, with the port numbers the run reads
     * @invariant   The type declares one output, the state channel, and it is **not** allowed in the field domain
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads
     */
    [[nodiscard]] static std::vector<qp::graph::NodeDesc> node_types();

    /**
     * @brief Registers the emitter type with `host` as a built-in.
     *
     * @param host The host. Borrowed.
     *
     * @ownership   observes `host`
     * @thread      main
     * @pre         none
     * @post        The type is registered unless its name was taken
     * @invariant   A type already registered under this name is left alone rather than duplicated
     * @errors      noexcept
     * @complexity  O(types)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads
     */
    static std::size_t mount(qp::host::PluginHost& host) noexcept;

    /**
     * @brief The initial condition a node's parameters describe.
     *
     * @param node The node instance.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Every port the spec has is the node's value or the description's default
     * @invariant   Unset parameters take defaults, so a node dropped on the canvas launches something rather
     *              than nothing
     * @errors      noexcept
     * @complexity  O(parameters)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.emitter.the_type_declares_the_ports_the_run_reads
     */
    [[nodiscard]] static EmitterSpec read_from(const qp::graph::Node& node) noexcept;
};

/**
 * @brief Expands a spec into a particle state, launching against the field it is handed.
 *
 * The particles are placed on the equatorial circle at `l_shell_re`, spread over `ring_span_deg` of a turn, and
 * each is given a speed of `beta c` whose angle to the **local** field is the spec's pitch angle and whose
 * perpendicular component points along the spec's flow angle. With the default flow angle of 90 degrees that is
 * a **ring current**: every particle moves azimuthally, and the population's guiding centres sit on a circle of
 * the same radius. The flow angle is measured in the frame of the local field, so it is not tied to the
 * particle's position on the ring -- see the spec's own comment for why that distinction is load-bearing.
 *
 * @param spec     What to launch. Refused -- not clamped -- when it is not usable.
 * @param magnetic The field the pitch angle is measured against: a volume of tesla vectors, SI.
 * @param grid     Where that field's nodes are. Must describe the same table `magnetic` holds.
 * @param out      The state to fill. **Resized**, so whatever was there is discarded.
 *
 * @ownership   owns the particles it writes into `out`
 * @thread      main
 * @pre         `grid` describes `magnetic`'s lattice
 * @post        On true, `out.count() == spec.count` and every particle is live with a finite state
 * @invariant   On false `out` is left empty
 * @errors      Returns false -- never throws -- for an unusable spec, for an unreadable magnetic table, and for
 *              a **zero field** at a launch point: a pitch angle cannot be measured against a zero field, and the
 *              alternative is a velocity whose direction is arbitrary
 * @complexity  O(count)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.emitter.a_ring_is_launched_at_the_pitch_angle_it_asks_for
 */
[[nodiscard]] bool emit_ring(const EmitterSpec& spec, const qp::graph::field::FieldValue& magnetic,
                             const GridSpec& grid, qp::graph::particles::ParticleState& out);

}  // namespace qp::plugins::magnetosphere
