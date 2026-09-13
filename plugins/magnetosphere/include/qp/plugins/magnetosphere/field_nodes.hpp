/**
 * @file field_nodes.hpp
 * @brief The field-domain node types this kit ships, and the evaluator that bakes them.
 *
 * ## One node, one independent force field
 *
 * The reference implementation this kit is ported from gives every field model its own node (`dipole`, `t89`,
 * `t96`, `t01`, `t04`, `ts05`, `ta16`) and states the principle in its own comments: each node outputs **one**
 * independent force field, and composition happens by **wiring the graph** rather than inside a node. A
 * Volland-Stern shielding field is `mul(convection, shield)`, not a node of its own. That is preserved here
 * because it is the difference between "the platform can model a magnetosphere" and "the platform has the
 * magnetosphere we happened to write".
 *
 * So this file ships exactly one type today, `field.dipole`, and the Tsyganenko family arrives as **further
 * types in this list** when a course needs them -- each one a different baker, not a different framework. They
 * are not here yet because each needs an external Fortran expansion, and a dependency that heavy must be pulled
 * by a real experiment rather than by symmetry.
 *
 * ## The grid belongs to the node, and that is why its port numbers live in one place
 *
 * A baked field is a model **evaluated onto a lattice**, so the node that owns the model must also own the
 * lattice: where it starts, how fine it is, how many nodes. The alternative -- a run-wide bake resolution set
 * somewhere else -- would make two field nodes in one graph share a grid whether or not that suited either of
 * them, and a dipole wants a different box from a magnetotail model.
 *
 * The consequence is a small contract that two readers have to agree on: the **evaluator** reads the grid from
 * its input values in order to bake, and the **plan builder** reads the same grid from the node in order to fill
 * the kernel's six grid-metadata slots (`BorisAdvancer::kIndexGridOrigin0` and its neighbours). Two readers of
 * one set of port numbers is two chances to disagree, so the numbers are constants here, `GridSpec::read_from`
 * is the only reader, and both call it. That is the same rule the kernel's parameter indices follow, and for the
 * same reason: the failure it prevents is a kernel sampling a grid that is not where it thinks it is, which
 * produces a plausible field from the wrong place.
 *
 * ## This is the first production `INodeEvaluator`
 *
 * `core/graph/eval` has had a complete contract since it was written -- `INodeEvaluator`, `EvalContext`,
 * `EvalCache`, `evaluate_graph` -- and, until this file, **no production code implemented or called any of it**.
 * The only implementations in the tree were three stubs in `tests/unit/graph/test_eval.cpp`. This kit is
 * therefore the field domain's first real consumer, exactly as it became the particle domain's first real
 * consumer one round earlier.
 *
 * @ownership   observes (the store is borrowed; the node is not modified)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No physical model other than the dipole is implemented here
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_type_declares_the_ports_the_evaluator_reads,
 *              magnetosphere.field_nodes.the_dipole_is_baked_onto_the_grid_it_declares,
 *              magnetosphere.field_nodes.a_handle_names_the_lattice_in_the_store,
 *              magnetosphere.field_nodes.a_node_of_another_domain_produces_nothing
 */
#pragma once

#include <qp/graph/field/field_set.hpp>
#include <qp/graph/eval/evaluator.hpp>
#include <qp/graph/ir/descriptor.hpp>
#include <qp/graph/ir/node.hpp>
#include <qp/host/host.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/geometry.hpp>
// For `kEarthRadiusM`, which the mask's default radii are written in. `emitter.hpp` includes it for the same
// reason and the two headers are normally used together: a default that says "one earth radius" and then spells
// the number out again would be a second place for the conversion to go stale.
#include <qp/plugins/magnetosphere/units.hpp>

namespace qp::plugins::magnetosphere {

/**
 * @brief The lattice a field node declares, in SI.
 *
 * A value rather than a reference to a node, because two callers read it from two different sources: the
 * evaluator has the node's **input values** (which is where an unwired parameter arrives, per the unified
 * Param/Port rule in `descriptor.hpp`) and the plan builder has the **node**. Both end up here.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `point_count()` equals `nx * ny * nz` for any counts at all
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_dipole_is_baked_onto_the_grid_it_declares
 */
struct GridSpec final {
    /// Where node `(0, 0, 0)` is, in metres.
    Vec3 origin_m{};
    /// The distance between neighbouring nodes along each axis, in metres.
    Vec3 spacing_m{};
    /// Nodes along `x`, `y`, `z`.
    std::uint32_t nx = 0;
    std::uint32_t ny = 0;
    std::uint32_t nz = 0;

    /// @brief How many sample points the grid holds.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        `nx * ny * nz`, computed in 64 bits so a typo cannot wrap into a small number
    /// @invariant   Never overflows for any counts a `std::uint32_t` can hold
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.the_dipole_is_baked_onto_the_grid_it_declares
    [[nodiscard]] std::uint64_t point_count() const noexcept {
        return static_cast<std::uint64_t>(nx) * ny * nz;
    }

    /// @brief The position of node `(i, j, k)`, in metres.
    ///
    /// @param i Index along `x`.
    /// @param j Index along `y`.
    /// @param k Index along `z`.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        `origin + (i * sx, j * sy, k * sz)`
    /// @invariant   Agrees with `BakedField::node_position` for the same grid
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.the_dipole_is_baked_onto_the_grid_it_declares
    [[nodiscard]] Vec3 node_position(std::uint32_t i, std::uint32_t j, std::uint32_t k) const noexcept {
        return Vec3{origin_m.x + static_cast<double>(i) * spacing_m.x,
                    origin_m.y + static_cast<double>(j) * spacing_m.y,
                    origin_m.z + static_cast<double>(k) * spacing_m.z};
    }
};

/**
 * @brief The field-domain node types of this kit, their port numbers, and how they are baked.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every port number here is one the descriptions in `node_types` declare
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_type_declares_the_ports_the_evaluator_reads
 */
class FieldNodes final {
public:
    /// @brief The tilted-dipole node type. The first of the family, and the one a first course needs.
    static constexpr const char* kDipoleType = "field.dipole";
    /// @brief A uniform field: the same vector everywhere.
    ///
    /// The second field model, and it is here before the Tsyganenko family for a reason that is not laziness: a
    /// uniform field is the case whose answer is **exact under trilinear interpolation** at any spacing, so it is
    /// the field a course uses to check a pusher (a gyrofrequency, a pitch angle, a drift) against a closed form
    /// with no interpolation error in the comparison at all. Every test of the Boris kernel in this kit builds
    /// one by hand; this node is what puts that in the palette.
    static constexpr const char* kUniformType = "field.uniform";

    /// @brief A uniform **electric** field: the second kind of field a pusher reads.
    ///
    /// The pusher has had an electric socket since it was written and **nothing produced a field for it**. This
    /// is that production: a constant `E`, in volts per metre, on the same uniform lattice the magnetic bakes use.
    /// The cross-polar-cap convection field is the case a first course treats as uniform, and the lesson it
    /// enables is the `E x B` drift -- the reason a magnetosphere has a convection pattern at all.
    static constexpr const char* kUniformElectricType = "field.uniform_electric";

    /// @brief The electric field's `x`, `y`, `z` components, in volts per metre.
    static constexpr qp::graph::PortNumber kPortE0 = 1;
    /// @brief The electric field's `y` component, in volts per metre.
    static constexpr qp::graph::PortNumber kPortE1 = 2;
    /// @brief The electric field's `z` component, in volts per metre.
    static constexpr qp::graph::PortNumber kPortE2 = 3;
    /// @brief Where the **electric** node's grid starts: after the three components.
    static constexpr qp::graph::PortNumber kPortElectricOrigin0 = 4;

    /// @brief The **sum** of two fields: the composition principle as a node.
    ///
    /// The reference implementation states the principle this exists for: each node outputs **one** independent
    /// force field, and composition happens by **wiring the graph** -- a shielding field is the sum of the two
    /// models, not a switch inside one node. Without a node like this, "composition by wiring" is a sentence
    /// rather than something a graph can say.
    static constexpr const char* kSumType = "field.sum";

    /// @brief The two fields to add. Both required: a sum with one addend is not a sum.
    static constexpr qp::graph::PortNumber kPortAddendA = 1;
    /// @brief The second addend.
    static constexpr qp::graph::PortNumber kPortAddendB = 2;

    /// @brief The uniform field's three components, in tesla: `x`, `y`, `z`.
    static constexpr qp::graph::PortNumber kPortField0 = 1;
    /// @brief The uniform field's `y` component, in tesla.
    static constexpr qp::graph::PortNumber kPortField1 = 2;
    /// @brief The uniform field's `z` component, in tesla.
    static constexpr qp::graph::PortNumber kPortField2 = 3;
    /// @brief Where the **uniform** node's grid starts.
    ///
    /// The dipole's grid starts at 3 because its model-specific ports are 1 and 2; a uniform field needs three
    /// (the components), so its grid starts at 4. Port numbers are unique within a type, and a grid that reused
    /// the dipole's numbering would declare ports 3, 4 and 5 twice -- which the host refuses at registration,
    /// silently for the caller: `mount` counts successes, so the second field type simply did not appear in the
    /// catalog until this was fixed.
    static constexpr qp::graph::PortNumber kPortUniformOrigin0 = 4;
    /// @brief The uniform grid's origin `y`.
    static constexpr qp::graph::PortNumber kPortUniformOrigin1 = 5;
    /// @brief The uniform grid's origin `z`.
    static constexpr qp::graph::PortNumber kPortUniformOrigin2 = 6;
    /// @brief The uniform grid's spacing along `x`, `y`, `z`.
    static constexpr qp::graph::PortNumber kPortUniformSpacing0 = 7;
    /// @brief The uniform grid's spacing along `y`.
    static constexpr qp::graph::PortNumber kPortUniformSpacing1 = 8;
    /// @brief The uniform grid's spacing along `z`.
    static constexpr qp::graph::PortNumber kPortUniformSpacing2 = 9;
    /// @brief The uniform grid's node counts along `x`, `y`, `z`.
    static constexpr qp::graph::PortNumber kPortUniformCount0 = 10;
    /// @brief The uniform grid's node count along `y`.
    static constexpr qp::graph::PortNumber kPortUniformCount1 = 11;
    /// @brief The uniform grid's node count along `z`.
    static constexpr qp::graph::PortNumber kPortUniformCount2 = 12;

    /// @brief The magnetic latitude of the dipole axis, in degrees.
    static constexpr qp::graph::PortNumber kPortTiltDegrees = 1;
    /// @brief The dipole moment, in ampere square metres.
    static constexpr qp::graph::PortNumber kPortMomentAm2 = 2;
    /// @brief The grid origin's `x`, `y`, `z`, in metres.
    static constexpr qp::graph::PortNumber kPortOrigin0 = 3;
    /// @brief The grid origin's `y`, in metres.
    static constexpr qp::graph::PortNumber kPortOrigin1 = 4;
    /// @brief The grid origin's `z`, in metres.
    static constexpr qp::graph::PortNumber kPortOrigin2 = 5;
    /// @brief The grid's node spacing along `x`, `y`, `z`, in metres.
    static constexpr qp::graph::PortNumber kPortSpacing0 = 6;
    /// @brief The grid's node spacing along `y`, in metres.
    static constexpr qp::graph::PortNumber kPortSpacing1 = 7;
    /// @brief The grid's node spacing along `z`, in metres.
    static constexpr qp::graph::PortNumber kPortSpacing2 = 8;
    /// @brief How many nodes along `x`, `y`, `z`.
    static constexpr qp::graph::PortNumber kPortCount0 = 9;
    /// @brief How many nodes along `y`.
    static constexpr qp::graph::PortNumber kPortCount1 = 10;
    /// @brief How many nodes along `z`.
    static constexpr qp::graph::PortNumber kPortCount2 = 11;
    /// @brief The baked field, a vector lattice in tesla.
    static constexpr qp::graph::PortNumber kPortField = 1;

    /// @brief The largest grid a bake will accept, in sample points.
    ///
    /// Four million points is about 100 MB of f64 vectors, which is a large field and not an absurd one. The cap
    /// exists because the alternative to refusing is not a slow run: a count typed with one digit too many asks
    /// for an allocation no machine has, and `std::vector`'s answer to that is to throw, which on this path
    /// terminates the process. **A refusal a user can read beats a crash they cannot.**
    static constexpr std::uint64_t kMaxPoints = 4U * 1024U * 1024U;

    /// @brief The scalar-region mask: a **weight** field, one number per node.
    ///
    /// The kit's first product that is not a vector field, and it exists because two sockets in this tree want a
    /// scalar and nothing could make one: the pusher's `drag` socket (`kPortDrag`, in per second) and the
    /// reference implementation's `mul`/`blend` pair, which modulate a vector field by a weight. A weight is a
    /// pure number -- dimensionless, which is a real `FieldDim` and not a missing one -- and that is why it is a
    /// field rather than a parameter: a *region* is a shape in space, and a shape is what a lattice is for.
    static constexpr const char* kMaskType = "field.mask";

    /// @brief The region the mask weights: a choice, so the index is what a document stores.
    enum class MaskRegion : std::int32_t {
        /// Everything inside `r0`: an atmosphere, a plasmasphere, the planet itself.
        sphere = 0,
        /// Everything between `r0` and `r1`: a belt, a shell, a boundary layer.
        shell = 1,
        /// Everything on the sunward side of the terminator plane.
        dayside = 2,
        /// Everything on the far side of it.
        nightside = 3,
    };

    /// @brief How many regions the choice offers.
    static constexpr std::uint32_t kMaskRegionCount = 4;
    /// @brief The region choice.
    static constexpr qp::graph::PortNumber kPortMaskRegion = 1;
    /// @brief The mask's inner radius, in metres. Used by `sphere` and `shell`.
    static constexpr qp::graph::PortNumber kPortMaskR0 = 2;
    /// @brief The mask's outer radius, in metres. Used by `shell`.
    static constexpr qp::graph::PortNumber kPortMaskR1 = 3;
    /// @brief Where the **mask** node's grid starts: after the three parameters.
    static constexpr qp::graph::PortNumber kPortMaskOrigin0 = 4;
    /// @brief The mask's weight table, a scalar lattice.
    static constexpr qp::graph::PortNumber kPortWeight = 1;

    /// @brief The mask's default inner radius, in metres: one earth radius, the planet's surface.
    static constexpr double kDefaultMaskR0Re = 1.0;
    /// @brief The mask's default outer radius, in metres: three earth radii, a belt's outer edge.
    static constexpr double kDefaultMaskR1Re = 3.0;

    /// @brief The multiplier: a vector field times a **scalar weight**, node by node.
    ///
    /// The other half of the pair the mask belongs to, and the shape `plan.hpp` already names: "a shielding field
    /// is `mul(convection, shield)`, not a switch inside a node". It has **no grid of its own**, and that is the
    /// decision rather than an omission -- the product of two tables is defined on the lattice they share, so a
    /// multiplier that asked for a grid would be offering the user a way to describe a lattice its inputs do not
    /// have. A graph that wants the product somewhere else resamples first, which is a different node.
    static constexpr const char* kMulType = "field.mul";

    /// @brief The vector field to scale. What lands here is the product's direction.
    static constexpr qp::graph::PortNumber kPortMulField = 1;
    /// @brief The scalar weight. A `kScalarField`, which is what a mask publishes.
    static constexpr qp::graph::PortNumber kPortMulWeight = 2;
    /// @brief The product, a vector lattice on the input's own grid.
    static constexpr qp::graph::PortNumber kPortMulOut = 1;

    /// @brief The **convection** electric field: the dawn-dusk potential, in volts per metre.
    ///
    /// `efield.py` in the reference implementation carries three of these -- the convection field, the corotation
    /// field and the Volland-Stern shielding factor -- and this is the first of them. It is the field that makes a
    /// magnetosphere *interesting*: with the magnetic field it produces the `E x B` drift, and that drift is why a
    /// magnetosphere has a convection pattern at all rather than being a set of closed shells.
    ///
    /// ## The model, and the reason it can be checked exactly
    ///
    /// The Volland-Stern potential is `phi = A r^gamma sin(gamma * azimuth)` in the equatorial plane, and at
    /// `gamma = 2` -- the value every course writes down -- the trigonometry collapses in Cartesian coordinates:
    ///
    ///     sin(2 * azimuth) = 2 x y / r^2   =>   phi(x, y) = 2 A x y
    ///
    /// which is a **quadratic polynomial**, so the field
    ///
    ///     E = -grad phi = (-2 A y, -2 A x, 0)
    ///
    /// is *linear*. A linear field sampled on a uniform lattice is reproduced by the kit's trilinear interpolation
    /// **exactly**, at nodes and between them, so this node's case can assert equality with the closed form rather
    /// than a tolerance. That is worth the sentence: it is the difference between testing a bake and testing a
    /// model, and it is why `gamma` is fixed at 2 rather than being a parameter.
    ///
    /// ## Why gamma is fixed, and what would reopen it
    ///
    /// The full Volland-Stern scaling comes from `Kp`, which is a **driver** -- the reference implementation has a
    /// `kp_source` node for it -- and this platform does not have one. A parameter that had to be a function of a
    /// driver would be a knob a user could set to a configuration the model never describes. The reopening
    /// condition is that source: when `Kp` exists, it feeds this node (and the shielding factor) rather than
    /// replacing them.
    ///
    /// ## The sign convention, stated because a drift that goes the wrong way is undebuggable
    ///
    /// `+x` is sunward -- the same convention the region mask's `dayside` uses -- and the azimuth is measured from
    /// it towards `+y`. With `A > 0` the field is `(-2Ay, -2Ax, 0)`, and against the dipole's southward equatorial
    /// field (`B = -B z_hat`) the `E x B` drift is **sunward on the dayside and antisunward on the nightside**,
    /// which is the observed pattern. The case asserts that drift direction through the cross product rather than
    /// asserting the numbers, because the numbers are the model and the direction is the physics.
    static constexpr const char* kConvectionType = "field.convection";
    /// @brief The potential's amplitude `A`, in volts per metre squared.
    static constexpr qp::graph::PortNumber kPortConvectionA = 1;
    /// @brief Where the **convection** node's grid starts: after the one parameter.
    static constexpr qp::graph::PortNumber kPortConvectionOrigin0 = 2;

    /// @brief The default amplitude, in volts per metre squared.
    ///
    /// Chosen by its **observable** rather than picked: `|E| = 2 A r` is 0.2 mV/m at ten earth radii, which is the
    /// order of the real cross-polar-cap field mapped to the equatorial plane. A course that measures the drift
    /// speed at that radius gets a number it can check against a textbook, and the arithmetic is in this comment so
    /// that changing the default is a decision rather than a nudge.
    static constexpr double kDefaultConvectionA = 1.5e-12;

    /// @brief The **corotation** electric field: the field a plasma moving with the planet sees.
    ///
    /// The second of `efield.py`'s three, and the one that has to exist for the convection field to mean anything:
    /// in the real magnetosphere the two compete, and the radius at which they balance is the plasmapause. A graph
    /// with only the convection field shows a magnetosphere that never rotates.
    ///
    /// ## The model, and the one thing it derives rather than assumes
    ///
    /// A plasma corotating with the Earth moves at `v = Omega x r`, and the electric field it sees is
    /// `E = -v x B = -(Omega x r) x B`. With `Omega` along `+z` and the dipole's field along `-z` at the equator,
    /// the product is **radially outward**:
    ///
    ///     E = Omega B(r) * (x, y, 0)          |E| = Omega B(r) r
    ///
    /// so `|E| / |B| = Omega r` -- the drift speed is the rigid rotation speed, which is what "corotating" means
    /// and what this node's case asserts. `B(r)` is **read from an input field** rather than assumed to be a
    /// dipole: this node does not model the magnetic field, it reacts to whatever field is wired into it, and a
    /// graph that wired a shielded or compressed field gets corotation in *that* field.
    ///
    /// ## Why it takes a field and not a magnitude
    ///
    /// A scalar `|B|` parameter would be simpler and wrong in a way that is hard to see: `B` varies by three orders
    /// of magnitude across the region a magnetosphere picture covers, so a single number would put the corotation
    /// speed right at one radius and wrong everywhere else. The input is a vector field, so the direction is
    /// available as well -- and using the local **vector** rather than its magnitude is what makes this correct for
    /// a field that is not perpendicular to the equatorial plane, where the corotation field has a component along
    /// the field lines that a scalar model would miss.
    static constexpr const char* kCorotationType = "field.corotation";
    /// @brief The magnetic field to corotate in. A `kVectorField`; the node derives its `E` from it.
    static constexpr qp::graph::PortNumber kPortCorotationMagnetic = 1;
    /// @brief The corotation field, in volts per metre.
    static constexpr qp::graph::PortNumber kPortCorotationOut = 1;

    /// @brief The atmosphere: an exponential **drag rate**, in per second, as a scalar field.
    ///
    /// The second scalar producer, and the one the pusher's drag socket was really waiting for. `field.mask` can
    /// fill that socket with a 0 or a 1, which is a drag that switches on at a boundary; this is a drag that varies
    /// the way an atmosphere does:
    ///
    ///     nu(r) = nu0 * exp(-(r - r_ref) / H)
    ///
    /// with `nu0` the rate at the reference radius and `H` the scale height. It is the reference implementation's
    /// `drag_single`, and its `drag_layered` -- several scale heights, one per layer -- is a different node when a
    /// course needs it rather than a second mode inside this one.
    ///
    /// ## Why the drag is the first force in this kit that takes energy away
    ///
    /// Every other force here is conservative: a magnetic field does no work at all, and an electric field does work
    /// but can give it back. A drag is the first thing that makes a trajectory *decay*, and that matters for this
    /// platform's subject rather than for its physics: a decaying quantity is what an experiment measures, and a
    /// rate is what a report quotes. It is also the first force whose effect the run cadence had to be fixed before
    /// anyone could see -- 8.7 milliseconds of a 0.01-per-second process is a change in the eighth decimal place.
    ///
    /// ## The parameters, and the units a reader has to know
    ///
    /// `nu0` is a **rate** (per second) and `H` is a **length** (metres), so the exponential's argument is
    /// dimensionless as it must be. The reference radius is a parameter rather than a constant because what "the
    /// rate at the surface" means depends on where the model's surface is, and a course that measures a decay at a
    /// satellite altitude wants to state the altitude it is quoting the rate at.
    static constexpr const char* kAtmosphereType = "field.atmosphere";
    /// @brief The drag rate at the reference radius, in per second.
    static constexpr qp::graph::PortNumber kPortAtmosphereNu0 = 1;
    /// @brief The scale height, in metres. How far the rate falls by a factor of `e`.
    static constexpr qp::graph::PortNumber kPortAtmosphereScaleHeight = 2;
    /// @brief Where `nu0` is quoted, in metres.
    static constexpr qp::graph::PortNumber kPortAtmosphereReference = 3;
    /// @brief Where the **atmosphere** node's grid starts: after the three parameters.
    static constexpr qp::graph::PortNumber kPortAtmosphereOrigin0 = 4;
    /// @brief The drag table, a scalar lattice in per second.
    static constexpr qp::graph::PortNumber kPortAtmosphereOut = 1;

    /// @brief The default drag rate at the surface, in per second. Zero: no atmosphere until a course asks for one.
    static constexpr double kDefaultAtmosphereNu0 = 0.0;
    /// @brief The default scale height, in metres: 100 km, the thermosphere's order at low altitude.
    static constexpr double kDefaultAtmosphereScaleHeightM = 100.0e3;
    /// @brief Where the default `nu0` is quoted, in metres: the equator's surface.
    static constexpr double kDefaultAtmosphereReferenceM = kEarthRadiusM;

    /// @brief What an atmosphere node's parameters say.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   `scale_height_m > 0` and `nu0_per_s >= 0`
    /// @errors      noexcept
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.an_atmosphere_thins_the_way_an_exponential_does
    struct AtmosphereSpec final {
        /// The drag rate at `reference_m`, in per second.
        double nu0_per_s = kDefaultAtmosphereNu0;
        /// The scale height, in metres.
        double scale_height_m = kDefaultAtmosphereScaleHeightM;
        /// Where `nu0_per_s` is quoted, in metres.
        double reference_m = kDefaultAtmosphereReferenceM;
    };

    /// @brief An atmosphere node's parameters, read from the node itself.
    ///
    /// A **negative** rate and a non-positive scale height are refused by `bake_atmosphere` rather than clamped here,
    /// which is the split `field.mask` also uses: the reader reports what the node says, and the bake decides what
    /// can be baked. Clamping a negative rate to zero here would turn a user's sign error into a silent no-op.
    ///
    /// @param node The node. Borrowed.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The three numbers the node carries, or the defaults for the ones it does not
    /// @invariant   Reads each port once, through the same `InputView` shape the evaluator is handed
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.an_atmosphere_thins_the_way_an_exponential_does
    [[nodiscard]] static AtmosphereSpec read_atmosphere(const graph::Node& node) noexcept;

    /// @brief The same reader for an evaluator's own inputs.
    ///
    /// @param inputs The evaluator's inputs. Borrowed for the call.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The same values `read_atmosphere` gives for the same ports
    /// @invariant   One reader, two sources
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.an_atmosphere_thins_the_way_an_exponential_does
    [[nodiscard]] static AtmosphereSpec read_atmosphere_from(const graph::InputView& inputs) noexcept;

    /// @brief What a mask node's parameters say.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   `r0_m <= r1_m`
    /// @errors      noexcept
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_mask_weights_the_region_it_names
    struct MaskSpec final {
        /// Which region the weight covers.
        MaskRegion region = MaskRegion::sphere;
        /// The inner radius, in metres.
        double r0_m = kDefaultMaskR0Re * kEarthRadiusM;
        /// The outer radius, in metres.
        double r1_m = kDefaultMaskR1Re * kEarthRadiusM;
    };

    /// @brief Names one region, for a report or a log line.
    ///
    /// @param region The region to name.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        One of the four names, never null
    /// @invariant   Total
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_mask_weights_the_region_it_names
    [[nodiscard]] static const char* to_string(MaskRegion region) noexcept;

    /// @brief A mask node's parameters, read from the node itself.
    ///
    /// Reads **before** `bake_mask` and not inside it, for the reason `read_from` gives about the grid: the node's
    /// parameters are interpreted in one place, and a second reader is a second answer to "which port is r0".
    ///
    /// @param node The node. Borrowed.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        An ordered radius pair, and the region the node names
    /// @invariant   Out-of-range values take the defaults rather than failing: a half-filled node is the
    ///              ordinary state of a graph being edited, and refusing to bake it would mean the picture
    ///              disappears while the user is still typing
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_mask_weights_the_region_it_names
    [[nodiscard]] static MaskSpec read_mask(const graph::Node& node) noexcept;

    /// @brief The same reader for an evaluator's own inputs, so "which port is `r0`" has one answer.
    ///
    /// Two sources and one implementation, exactly as `read_from` is: the node's parameters are put into the same
    /// `InputView` shape the evaluator is handed. A second reader for the same three ports would be a second place
    /// the meaning of port 2 is written down, and the two would agree until one of them was edited.
    ///
    /// @param inputs The evaluator's inputs. Borrowed for the call.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        An ordered radius pair and the region the inputs name
    /// @invariant   Out-of-range region indices take `sphere`, as `read_mask` does
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_mask_weights_the_region_it_names
    [[nodiscard]] static MaskSpec read_mask_from(const graph::InputView& inputs) noexcept;

    /// @brief The grid a dipole node starts with: eight earth radii either way, one node per earth radius.
    ///
    /// A default a student can run without editing anything, and coarse enough that a bake is instant. The
    /// interpolation error over a one-earth-radius cell is a few percent of a `1/r^3` field, which is visible
    /// and is why the node's spacing is a parameter rather than a constant: a measurement that needs better
    /// lowers it, and the reopening condition is written down in `baked_field.hpp`.
    static constexpr double kDefaultHalfExtentRe = 8.0;
    /// @brief The default node spacing, in earth radii.
    static constexpr double kDefaultSpacingRe = 1.0;
    /// @brief The default node count per axis: `2 * half_extent / spacing + 1`.
    static constexpr std::uint32_t kDefaultNodesPerAxis = 17;

    FieldNodes() = delete;

    /**
     * @brief The node types this build ships, ready to register with a host.
     *
     * A static function rather than member data, for the reason `ModelsBinder::node_types` gives: the
     * descriptions are constants of the **build**, not of an object, and a caller that wants palette entries
     * should not have to construct anything to get them.
     *
     * @ownership   owns the returned descriptions
     * @thread      main
     * @pre         none
     * @post        One description per field type, with the port numbers the evaluator reads
     * @invariant   Every type is allowed in the field domain and forbidden in the particle domain, because a
     *              bake may allocate and block and the particle domain forbids both
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.the_type_declares_the_ports_the_evaluator_reads
     */
    [[nodiscard]] static std::vector<qp::graph::NodeDesc> node_types();

    /**
     * @brief Registers the field types with `host` as built-ins.
     *
     * @param host The host. Borrowed.
     *
     * @ownership   observes `host`
     * @thread      main
     * @pre         none
     * @post        Every type whose name was free is registered and attributed to `PluginHost::kBuiltinOrigin`
     * @invariant   A type already registered under its name is left alone rather than duplicated
     * @errors      noexcept
     * @complexity  O(types)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.the_type_declares_the_ports_the_evaluator_reads
     */
    static std::size_t mount(qp::host::PluginHost& host) noexcept;

    /**
     * @brief The grid declared by a node's **parameters**.
     *
     * The reader the plan builder uses: it has the node and needs the six numbers the kernel's parameter block
     * wants. A parameter that is missing takes the description's own default, so a node a user dropped on the
     * canvas without touching anything bakes and runs.
     *
     * @param node        The node instance.
     * @param origin_port The first of the nine grid ports; see the `InputView` overload.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The grid the node's parameters describe, or the defaults where one is unset
     * @invariant   Equals `read_from` on the input values of the same node
     * @errors      noexcept
     * @complexity  O(parameters)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.the_dipole_is_baked_onto_the_grid_it_declares
     */
    [[nodiscard]] static GridSpec read_from(const qp::graph::Node& node,
                                            qp::graph::PortNumber origin_port = kPortOrigin0) noexcept;

    /**
     * @brief The grid declared by a node's **input values**.
     *
     * The reader the evaluator uses, and it is the same function's other half rather than a second
     * implementation: `evaluate_graph` collects an input port's value from the edge if there is one and from the
     * node's parameters otherwise, so the values the evaluator sees are the node's own parameters whenever
     * nothing is wired -- which for a bake grid is always.
     *
     * @param inputs      The input values the evaluator was handed.
     * @param origin_port The first of the nine grid ports. Defaulted to the dipole's, because the dipole's grid
     *                    is the one at 3; a type whose model-specific ports run to three or more starts its grid
     *                    later and says so here.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The grid the inputs describe, or the defaults where one is absent
     * @invariant   Uses the same port numbers as `read_from`
     * @errors      noexcept
     * @complexity  O(inputs)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.a_node_of_another_domain_produces_nothing
     */
    [[nodiscard]] static GridSpec read_from(const qp::graph::InputView& inputs,
                                            qp::graph::PortNumber origin_port = kPortOrigin0) noexcept;
};

/**
 * @brief Bakes a tilted dipole onto a grid and publishes it.
 *
 * Free rather than a member, because it is a function of the model and the grid and nothing else: the evaluator
 * is one caller and a test is another, and a test that had to construct an evaluator to check the arithmetic
 * would be checking the evaluator's plumbing instead.
 *
 * @param tilt_degrees The dipole axis's magnetic latitude.
 * @param moment_am2   The dipole moment.
 * @param grid         Where to evaluate and how finely.
 * @param key          Who is publishing, for the store's key.
 * @param fields       The store. Mutated: a successful bake leaves one more field in it.
 *
 * @ownership   observes `fields`, owns nothing after the call
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable volume of tesla vectors on `grid`
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws for a bad grid -- when the grid is unusable (a non-positive
 *              spacing, fewer than two nodes on an axis, more than `kMaxPoints` points), and for a non-finite
 *              tilt or moment
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_dipole_is_baked_onto_the_grid_it_declares
 */
[[nodiscard]] bool bake_dipole(double tilt_degrees, double moment_am2, const GridSpec& grid,
                               qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Bakes a uniform field onto a grid and publishes it.
 *
 * Separate from `bake_dipole` rather than a branch inside it: the two share the grid and the publication and
 * nothing else, and a function that took "which model" as an argument would be the first line of a switch that
 * every future field model has to be added to.
 *
 * @param value The field everywhere, in tesla.
 * @param grid  Where to evaluate and how finely.
 * @param key   Who is publishing, for the store's key.
 * @param fields The store. Mutated: a successful bake leaves one more field in it.
 *
 * @ownership   observes `fields`, owns nothing after the call
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable volume of tesla vectors equal to `value` at every node
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for an unusable grid or a non-finite component
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_uniform_field_is_uniform
 */
[[nodiscard]] bool bake_uniform(const Vec3& value, const GridSpec& grid, qp::graph::field::FieldKey key,
                                qp::graph::field::FieldSet& fields,
                                qp::abi::FieldDim dimension = tesla_dimension());

/**
 * @brief Adds two published fields node by node and publishes the result under `key`.
 *
 * @param a    The first addend. Must be a readable vector volume of f64.
 * @param b    The second addend, on the **same lattice**: a sum of two tables that disagree about their geometry
 *             is not a sum, and it is refused rather than fitted.
 * @param key  Who is publishing, for the store's key.
 * @param fields The store. Mutated on success.
 *
 * @ownership   observes `fields`
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is `a + b` at every node
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for an unreadable addend, for lattices whose counts disagree, or
 *              for a non-f64 element type
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.two_fields_add_where_the_graph_says
 */
[[nodiscard]] bool bake_sum(const qp::graph::field::FieldValue& a, const qp::graph::field::FieldValue& b,
                            qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Bakes a region mask's **weight** table onto `grid`.
 *
 * Every node gets exactly `0.0` or `1.0` -- never a blend and never a fraction -- and that is a decision rather
 * than a simplification: the weight is a **region test**, so a table of intermediate values would be a claim about
 * a boundary this node does not model. The smooth boundary the reference implementation blends across is a
 * magnetopause model, which is a different node and would arrive as one.
 *
 * The published table is a **scalar** f64 volume of `grid.point_count()` values, where every field this kit made
 * until now was three per node. That is the part worth stating: a weight is a pure number, the descriptor says so,
 * and a consumer that assumed three components would read its own samples out of alignment.
 *
 * @param mask  The region and its radii, in metres.
 * @param grid  Where to bake. At least two nodes an axis and a positive spacing; anything else is refused.
 * @param key   Who is publishing, for the store's key.
 * @param fields The store. Mutated on success.
 *
 * @ownership   observes `fields`
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable scalar volume of `grid.point_count()` values, each
 *              exactly 0 or 1
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for a grid that cannot be baked, a non-finite radius, or a radius
 *              pair whose inner bound is above its outer one: see the implementation for why a reversed pair is
 *              refused here while the reader orders it
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_mask_weights_the_region_it_names
 */
[[nodiscard]] bool bake_mask(const FieldNodes::MaskSpec& mask, const GridSpec& grid,
                             qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Multiplies a published vector field by a published scalar weight, node by node.
 *
 * The product is `a_i * w_i` at every node, component by component, and **the weight is read once per node rather
 * than once per component**: a scalar table has one value where a vector table has three, so a loop that walked
 * the vectors and indexed the weight by the same offset would read every third weight and three times nothing
 * else. That is the mistake this function's shape exists to make impossible -- the two tables are indexed by
 * **point**, and only the output walks components.
 *
 * The output carries `a`'s descriptor unchanged, including its dimension: scaling a field by a pure number does
 * not change what the field is, and a product labelled with a different `FieldDim` would be a second answer to
 * "what is this table".
 *
 * @param a    The vector field to scale. A readable f64 volume of vectors.
 * @param w    The weight, on the **same lattice**: counts must agree on all three axes, and a product of two
 *             tables that disagree about their geometry is refused rather than fitted.
 * @param key  Who is publishing, for the store's key.
 * @param fields The store. Mutated on success.
 *
 * @ownership   observes `fields`
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is `a * w` at every node, described exactly as `a` is
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for an unreadable input, for a `w` that is not a scalar volume,
 *              for a vector in the weight socket, or for lattices whose counts disagree
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_field_scales_by_its_weight
 */
[[nodiscard]] bool bake_scaled(const qp::graph::field::FieldValue& a, const qp::graph::field::FieldValue& w,
                               qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Bakes the Volland-Stern convection field, `E = (-2Ay, -2Ax, 0)`, onto `grid`.
 *
 * The model and the sign convention are argued on `kConvectionType`; what belongs here is why the arithmetic is
 * written the way it is. The field is **linear in position**, so it is evaluated from the node's own coordinates
 * rather than from a potential that was differenced numerically: a finite difference of `phi = 2Axy` would be a
 * second approximation stacked on the interpolation, and it would make the exactness this node's case rests on
 * impossible to state.
 *
 * `z` is untouched, and that is the model rather than an oversight: the Volland-Stern potential is defined in the
 * equatorial plane, and a field that grew with `|z|` would be a field with a divergence and a curl nobody asked
 * for. A three-dimensional convection model is a different node.
 *
 * @param amplitude_v_per_m2 The potential's amplitude `A`. Zero gives a zero field, which is a legal experiment.
 * @param grid  Where to bake. At least two nodes an axis and a positive spacing; anything else is refused.
 * @param key   Who is publishing it.
 * @param fields The store. Borrowed; the samples are moved into it on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable vector volume of volt-per-metre values, equal at every
 *              node to `(-2Ay, -2Ax, 0)` evaluated there
 * @invariant   On false the store is unchanged
 * @errors      Returns false rather than throwing, for an un-bakeable grid or a non-finite amplitude
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_convection_field_is_the_potentials_gradient
 */
[[nodiscard]] bool bake_convection(double amplitude_v_per_m2, const GridSpec& grid,
                                   qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Bakes the corotation field, `E = -(Omega x r) x B`, from a magnetic field that is already published.
 *
 * The magnetic field is read **through the sampler**, at every node, from the table the input node published: the
 * geometry travels beside the value for the same reason the pusher is handed six parameter slots for it, and the
 * caller is the one that knows where the samples are.
 *
 * A node where the field is unreadable or zero is left at **zero**, not refused: a field that vanishes somewhere
 * is a real configuration (a null point, or the edge of a lattice whose samples stop), and `E = 0` there is the
 * model's own answer -- corotation in no field is no electric field. Refusing the whole bake instead would make a
 * graph with one bad corner undrawable.
 *
 * @param magnetic The magnetic field to corotate in. Must be a readable f64 volume of vectors.
 * @param origin_m  Where the magnetic table's node `(0, 0, 0)` is, in metres.
 * @param spacing_m The magnetic table's node spacing, in metres.
 * @param grid  Where to bake this field. At least two nodes an axis and a positive spacing.
 * @param key   Who is publishing it.
 * @param fields The store. Borrowed; the samples are moved into it on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable vector volume of volt-per-metre values, equal at every
 *              node to `(Omega x r) x B` reversed in sign, evaluated with the field sampled there
 * @invariant   On false the store is unchanged
 * @errors      Returns false rather than throwing, for an unreadable magnetic field or an un-bakeable grid
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.corotation_is_the_rotation_the_field_allows
 */
[[nodiscard]] bool bake_corotation(const qp::graph::field::FieldValue& magnetic, const Vec3& origin_m,
                                   const Vec3& spacing_m, const GridSpec& grid,
                                   qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Where a wired field's samples are: the node that baked them, and the grid that node declared.
 *
 * ## Why this is one function and not a clause in each caller
 *
 * `abi::LatticeDesc` carries **counts and not positions**, so every consumer that has to sample a published field
 * needs two things the value cannot tell it: which node produced the table, and what geometry that node declared.
 * There are now three such consumers -- the plan builder (filling a pusher's six grid slots), the corotation bake
 * (sampling `B` at every node) and any future node that reads a field pointwise -- and a rule written in three
 * places is a rule that agrees until one of them is edited.
 *
 * `plan.cpp` had the first version of this: it walked the wire, and then **refused any source that was not a
 * dipole** (`grid_unknown`). That was honest -- it named what it could not do -- and it also meant a pusher could
 * only ever be wired to a dipole's field, which stopped being true the moment this kit had other producers. The
 * refusal is replaced by the table below.
 *
 * ## The combinators have no grid of their own, so the walk continues through them
 *
 * `field.sum` and `field.mul` publish on **their input's** lattice -- a sum of two tables is defined on the lattice
 * they share, and a multiplier is the same -- so asking *them* for a grid is asking the wrong node. This function
 * therefore follows the wire back through any node that has no grid of its own until it reaches one that does.
 * That is one rule about this kit rather than a special case per type: a node either declares a grid or forwards to
 * whoever fed it.
 *
 * @param graph    The graph. Borrowed.
 * @param consumer The node whose socket is wired.
 * @param socket   The input port to follow.
 * @param out      Filled on true: the publishing node and the grid it declared.
 *
 * @ownership   observes `graph`
 * @thread      main
 * @pre         none
 * @post        On true, `out.node` is the node that baked the field on that socket and `out.grid` is what it
 *              declared; on false `out` is untouched and the caller has a refusal to make
 * @invariant   Never follows more hops than the graph has nodes, so a cycle terminates rather than hanging
 * @errors      Returns false -- never throws -- for a socket that is not wired, a wire whose source no longer
 *              exists, a source type this build does not know, or a walk that fails to reach a grid
 * @complexity  O(hops)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_wired_field_reports_the_grid_it_was_baked_on
 */
[[nodiscard]] bool resolve_field_origin(const qp::graph::Graph& graph, qp::graph::NodeId consumer,
                                        qp::graph::PortNumber socket, GridSpec& out) noexcept;

/**
 * @brief Bakes an atmosphere's exponential drag rate onto `grid`: `nu(r) = nu0 exp(-(r - r_ref) / H)`.
 *
 * The published table is a **scalar** f64 volume in per second -- the drag socket's own unit, as `boris.hpp`
 * documents it -- and it is what the pusher multiplies a velocity by, once per sub-step, as `1 - nu dt`. That is
 * a first-order damping rather than an exact exponential decay, and the difference is worth stating because this
 * node's case checks the decay against `exp(-nu t)`: the two agree to `O((nu dt)^2)` per step, which for a rate of
 * `0.01` per second and a step of twenty milliseconds is four parts in a hundred million. The case's tolerance is
 * a percent, so it is testing the model rather than the integrator's expansion.
 *
 * A **negative rate** or a non-positive scale height is refused rather than clamped: a drag that adds energy, or a
 * scale height of zero (an atmosphere that is a wall), are not configurations this model describes, and silently
 * turning either into something legal would hide a user's sign error behind a plausible decay.
 *
 * @param spec  The rate, the scale height and where the rate is quoted, in SI.
 * @param grid  Where to bake. At least two nodes an axis and a positive spacing.
 * @param key   Who is publishing it.
 * @param fields The store. Borrowed; the samples are moved into it on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable scalar volume in hertz, equal at every node to the
 *              exponential above evaluated there
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for a negative rate, a non-positive or non-finite scale height or
 *              reference, or a grid that cannot be baked
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.an_atmosphere_thins_the_way_an_exponential_does
 */
[[nodiscard]] bool bake_atmosphere(const FieldNodes::AtmosphereSpec& spec, const GridSpec& grid,
                                   qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief The node evaluator that bakes this kit's field types into a store.
 *
 * @ownership   observes the store, which must outlive it
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Holds no per-node state: the same node and inputs publish the same samples
 * @errors      See `evaluate`
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_handle_names_the_lattice_in_the_store
 */
class DipoleEvaluator final : public qp::graph::INodeEvaluator {
public:
    /**
     * @brief Binds the evaluator to the store it publishes into.
     *
     * @param fields The store. Borrowed; must outlive this object.
     *
     * @ownership   observes
     * @thread      main
     * @pre         `fields` outlives this object
     * @post        none
     * @invariant   The evaluator never publishes anywhere else
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.a_handle_names_the_lattice_in_the_store
     */
    explicit DipoleEvaluator(qp::graph::field::FieldSet& fields) noexcept : fields_(&fields) {}

    /**
     * @brief Tells the evaluator which graph it is baking, so a node with **field inputs** can find them.
     *
     * A `field.sum` node reads two field inputs, and what it needs is not their descriptors but their **data**.
     * `ports::Value`'s `field_handle` carries a `LatticeDesc` and no key -- deliberately, because a value must not
     * carry a pointer into somebody else's lifetime -- so the only way to the samples is the store, and the store
     * is keyed by `(node, port)`. `INodeEvaluator::evaluate` is handed neither: its own id, its own description
     * and its inputs' **values**, not their origins.
     *
     * So the evaluator borrows the graph, which the composition root already has when it starts a bake, and
     * resolves each field input the way the plan builder resolves a socket: follow the edge into `(id, port)`.
     * No frozen interface changes, and the wiring is read from the one place that owns it.
     *
     * Reopening condition, written down rather than implied: **when a second evaluator needs the graph**, its
     * home is `EvalContext`, which whoever drives can carry without every evaluator holding a pointer of its own.
     *
     * @param graph The graph being baked. Borrowed; it must outlive the bake.
     *
     * @ownership   observes `graph`
     * @thread      main
     * @pre         `graph` outlives every `evaluate` call made after this
     * @post        `evaluate` can resolve the inputs of a node with field sockets
     * @invariant   The evaluator never modifies the graph
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.two_fields_add_where_the_graph_says
     */
    void set_graph(const qp::graph::Graph& graph) noexcept { graph_ = &graph; }

    /**
     * @brief Bakes the node's field and answers with a handle naming it.
     *
     * **A node of another type produces nothing, and that is deliberate.** `evaluate_graph` walks the whole
     * graph -- all three domains -- so the bake is called for particle and render nodes too. Answering
     * `unknown_node` for them, which is what the test fixtures do, would make a graph that contains a pusher
     * impossible to bake at all; a graph whose node type nobody implements is a **validation** finding
     * (`core/graph/validate` reports an unknown type at load time), not a bake failure. So the rule is: the
     * types in `FieldNodes::node_types` are mine and are baked; everything else has no bake-time value.
     *
     * @param id     The node, which becomes half of the store key.
     * @param desc   The node's type description.
     * @param inputs Its input values, which carry its unwired parameters.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On success the store holds the node's field and the returned handle describes it
     * @invariant   The returned descriptor is the store's own, not a second construction of the same thing
     * @errors      `invalid_argument` for a grid the bake cannot honour, and for a non-finite tilt or moment
     * @complexity  O(points)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.a_handle_names_the_lattice_in_the_store,
     *              magnetosphere.field_nodes.a_node_of_another_domain_produces_nothing
     */
    [[nodiscard]] qp::diag::Result<std::vector<std::pair<qp::graph::PortNumber, qp::ports::Value>>> evaluate(
        qp::graph::NodeId id, const qp::graph::NodeDesc& desc,
        const std::vector<std::pair<qp::graph::PortNumber, qp::ports::Value>>& inputs) override;

private:
    /// @brief The field a socket of `id` is wired to, or an unreadable value when nothing is or it is not baked.
    [[nodiscard]] qp::graph::field::FieldValue input_field(qp::graph::NodeId id,
                                                          qp::graph::PortNumber port) const noexcept;

    qp::graph::field::FieldSet* fields_;
    const qp::graph::Graph* graph_ = nullptr;
};

}  // namespace qp::plugins::magnetosphere
