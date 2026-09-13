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

    /// @brief The IMF: the reference's `imf_source`, the **third** of the three sources and the only one that
    ///        publishes a field.
    ///
    /// ## What it is
    ///
    /// A **uniform** field whose direction is the Parker spiral angle and whose magnitude grows with the activity
    /// index: where `field.uniform` takes three typed components, this one computes them from a model of the solar
    /// wind. That is why it is a field node and not a third scalar driver: what a magnetosheath needs is a table,
    /// and the reference arranges it the same way -- its `imf_source` fills a lattice and hands back
    /// `Field("vector", data, lat)`.
    ///
    /// The arithmetic is the reference's, verbatim, with its two constants named here so that the paragraph below
    /// has something to point at:
    ///
    ///     b_ref   = 3.0 + 0.5 Kp            (nT -- the reference's own 3 and 0.5)
    ///     b_total = b_ref * kReferenceMagnitudeFactor
    ///     B       = ( sign b_total cos(theta), -sign b_total sin(theta), 0 )    theta = the spiral angle
    ///
    /// ## The factor of the square root of two, and the comment that disagrees with it
    ///
    /// The reference carries `b_total = b_ref * sqrt(2)` with the comment "preserve magnitude at 45 degrees". The
    /// comment is true of the **components** and not of the magnitude: at the canonical 45 degrees each component
    /// is `b_total/sqrt(2) = b_ref`, while `|B| = b_total = sqrt(2) b_ref` at *every* angle -- the components are
    /// a rotation of one vector, so the angle cannot change its length. So the reading that makes the reference's
    /// own sentence true is "`b_ref` is the component scale at the canonical spiral angle", and that is what this
    /// port bakes: the factor is a **named constant** (`kReferenceMagnitudeFactor`) rather than a written-out
    /// `1.414`, so that a future experiment can choose between the two readings and measure the difference, which
    /// is a forty-one percent change in `|B|`.
    ///
    /// ## What is deliberately **not** ported: the zero field
    ///
    /// The reference emits `(0, 0, 0)` unless a `parker_custom` flag is set, and its own docstring calls that a
    /// property of the legacy product rather than of the model. It is not ported, and the reason is the rule this
    /// platform applies everywhere else: **a zero field is a legal physical state**, so "nobody configured the
    /// IMF" and "the IMF is zero" must be distinguishable, and a node that answers the first with the second is
    /// reporting an unmeasured value as a measurement. The node therefore always computes, and the angle it uses
    /// is the reference's own default for the enabled case.
    ///
    /// ## `B_z` is exactly zero, and that is a statement about the model
    ///
    /// The reference's IMF has no clock angle: the field lies in the equatorial plane, so its `z` component is
    /// **exactly** zero at every node rather than a small number that ought to be. That is a real limitation and
    /// not a detail -- a southward `B_z` is what opens the magnetopause, and no run of this node can show it. The
    /// negative statement is written here because a reader who assumes a solar-wind driver produces reconnection
    /// would be assuming something this model does not contain; the porting ledger (`plan-tree` 9.33) records what
    /// a driver that did carry a clock angle would be.
    static constexpr const char* kImfType = "field.imf";

    /// @brief The polarity: which way the spiral's field points along the Sun-Earth line.
    ///
    /// An **enum** port with two named choices rather than a signed number, because the reference's `-1`/`+1` is a
    /// label in disguise and a parameter panel that offers "toward the Sun" and "away from it" cannot be typed
    /// wrong. Choice order is frozen: the stored value is the index, so swapping the two names would silently
    /// reverse every saved document's field.
    static constexpr qp::graph::PortNumber kPortImfPolarity = 1;
    /// @brief The spiral angle, in degrees from the Sun-Earth line.
    static constexpr qp::graph::PortNumber kPortImfAngle = 2;
    /// @brief The activity index, as an **optional socket** -- the fourth consumer of that shape.
    static constexpr qp::graph::PortNumber kPortImfKp = 3;
    /// @brief Where the IMF node's grid starts: after the three model ports.
    static constexpr qp::graph::PortNumber kPortImfOrigin0 = 4;

    /// @brief The reference's two magnitude constants, named.
    static constexpr double kReferenceImfBaseNt = 3.0;
    static constexpr double kReferenceImfPerKpNt = 0.5;
    /// @brief The reference's `sqrt(2)`, kept as a name because its meaning is argued above.
    ///
    /// Written as a constant rather than as `1.4142135...` so that the case can talk about the reading rather than
    /// about the digits: `std::sqrt(2.0)` is also what the reference computes.
    static constexpr double kReferenceMagnitudeFactor = 1.41421356237309504880;
    /// @brief The angle's default, in degrees: the reference's own, and the classic Parker value.
    static constexpr double kDefaultImfAngleDegrees = 40.0;
    /// @brief The spiral's observed range, in degrees. The reference clamps into it and so does this reader.
    static constexpr double kMinImfAngleDegrees = 25.0;
    static constexpr double kMaxImfAngleDegrees = 55.0;

    /// @brief The magnetic latitude of the dipole axis, in degrees.
    /// @brief The dipole's **tilt**, in degrees: the parameter a user types.
    static constexpr qp::graph::PortNumber kPortTiltDegrees = 1;
    /// @brief The optional **tilt driver**: wire a date here and the dipole leans by what that day implies.
    ///
    /// Numbered after the nine grid ports for the reason `kPortMagnetopauseKp` gives: a type's ports are its own and
    /// a new one goes at the end, so a saved document keeps meaning what it meant. The wired value **wins** over
    /// `kPortTiltDegrees`, which becomes the fallback -- the same shape the magnetopause's Kp socket established, and
    /// the second consumer of it, which is what makes the pattern a pattern rather than one node's exception.
    static constexpr qp::graph::PortNumber kPortTiltDriver = 12;
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

    /// @brief The Volland-Stern **shielding coefficient**: a scalar that suppresses convection inside `r0`.
    ///
    /// The third of `efield.py`'s three, and the one that is **not a field**: the reference's own header states the
    /// family's composition rule and then shows it --
    ///
    ///     E = add( corotation(B), mul( convection, volland_shield ) )
    ///
    /// -- so what this node publishes is the factor inside that `mul`, `w = (r / r0)^2` for `r < r0` and `1`
    /// outside. It composes with `field.mul` in this kit exactly as it does there, which is why it is a scalar
    /// producer with its own grid and no field inputs.
    ///
    /// ## What the coefficient construction is, and what it is not
    ///
    /// Multiplying a field by a function of position is **not** the same as multiplying its potential: with
    /// `E = w (-grad phi)` the curl is `grad w x (-grad phi)`, which is not zero, so the shielded field is not the
    /// gradient of the shielded potential `w phi`. The reference multiplies the field, and this node publishes the
    /// factor rather than deciding for the caller which of the two constructions they asked for -- a graph that
    /// wants `-grad(w phi)` can have it by wiring a different combination, and the difference between the two has a
    /// closed form (`phi grad w`) that the case measures rather than argues about. That is the whole reason this is
    /// a **coefficient** and not a field: a node that published `w E` would have made the choice for every graph
    /// that ever needed the factor.
    ///
    /// ## The reference's floor, and why it is not here
    ///
    /// The reference writes `where((r < r0) & (r > 0.1), (r/r0)^2, 1.0)`, so inside a tenth of an earth radius its
    /// coefficient is **one** -- no shielding at all. `(r/r0)^2` has no singularity at the origin (it is zero
    /// there), so the floor is guarding something else, and in this kit a coefficient that jumps to one at the
    /// centre of the planet would be a number nobody asked for. The formula is used as written, without the floor.
    ///
    /// ## Where the Kp scaling is not, and why
    ///
    /// The full Volland-Stern model scales both `A` and `r0` with `Kp`, and the convection node's contract already
    /// records that the reopening condition for its amplitude is a driver. The driver now exists
    /// (`SourceNodes::kKpType`), but the reference's `efield.py` has **no `Kp` formula in it** -- its convection
    /// node carries a bare `multiplier` and this node a bare `r0` -- so no scaling is invented here. The
    /// reopening condition is a **source** for that scaling rather than a driver: when one is chosen, `Kp` feeds
    /// both nodes through the optional-socket pattern `field.magnetopause` established.
    static constexpr const char* kShieldType = "field.shield";
    /// @brief The radius the coefficient reaches one at, in metres: inside it, convection is suppressed.
    static constexpr qp::graph::PortNumber kPortShieldR0 = 1;
    /// @brief Where the **shield** node's grid starts: after the one parameter.
    static constexpr qp::graph::PortNumber kPortShieldOrigin0 = 2;
    /// @brief The coefficient.
    static constexpr qp::graph::PortNumber kPortShieldOut = 1;

    /// @brief The default shielding radius, in metres: four earth radii, the reference implementation's own value.
    static constexpr double kDefaultShieldR0M = 4.0 * kEarthRadiusM;

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

    /// @brief The tail: a **Harris current sheet**, the analytic model of the stretched nightside.
    ///
    /// The reference implementation's `tail.py` is "the distant tail's analytic model", and the analytic tail is the
    /// Harris sheet -- the one model of a magnetotail every course writes down:
    ///
    ///     B = ( B0 tanh(z / L), 0, 0 )
    ///
    /// A field along `x` that reverses across the equatorial plane, saturating at `+/-B0` more than a few `L` away
    /// from it, and **exactly zero in the plane itself**. That last property is what a reconnecting tail is built
    /// on: with `B` along `x` and the field reversing at `z = 0`, a particle crossing the plane sees no field at all
    /// and is not turned, which is how the plasma sheet is populated.
    ///
    /// ## Why this is a node rather than a parameter on the dipole
    ///
    /// The same reason every other field here is: this is an **independent** field, and the composition is the
    /// wiring. `sum(dipole, current_sheet)` is the classic magnetosphere cross-section -- closed lines on the
    /// dayside, stretched ones on the nightside -- and a dipole with a "tail shape" parameter would be a model
    /// nobody can reason about, because the two fields have nothing to do with each other physically. This one is
    /// the field of a current, and the current is not the dipole's.
    ///
    /// ## The current, which is derived rather than assumed
    ///
    /// Ampere's law turns the field into the current that makes it: `J_y = -(1/mu0) dB_x/dz = -(B0 / (mu0 L))
    /// sech^2(z / L)`. A sheet current flowing in `y`, concentrated within a few `L` of the plane, carrying
    /// `B0 / mu0` amperes per metre in total. That is a **derived** quantity the case measures by differentiating
    /// the baked table, so a bake that got the argument or the scale wrong fails on the physics rather than on a
    /// value it also chose.
    ///
    /// ## `L` is a half-thickness, and the units say so
    ///
    /// `tanh(z/L)` is dimensionless, so `L` is a length in metres and `B0` is a field in tesla. The default is the
    /// observed order: the plasma sheet's half-thickness is a few earth radii at the distances a first course looks
    /// at, and `B0` is a few nanotesla twenty radii downwind -- four orders below the surface field, which is why a
    /// picture of the sum of this and a dipole is a dipole near the Earth and a sheet far from it.
    static constexpr const char* kCurrentSheetType = "field.current_sheet";
    /// @brief The lobe field, in tesla: what `|B|` saturates to away from the plane.
    static constexpr qp::graph::PortNumber kPortSheetB0 = 1;
    /// @brief The sheet's half-thickness, in metres.
    static constexpr qp::graph::PortNumber kPortSheetThickness = 2;
    /// @brief Where the **sheet** node's grid starts: after the two parameters.
    static constexpr qp::graph::PortNumber kPortSheetOrigin0 = 3;
    /// @brief The hinge socket: the tilt the tail's equatorial plane is hinged by, in degrees. Optional.
    ///
    /// ## What a hinge is, and why the sheet needs one
    ///
    /// A current sheet lying in `z = 0` is the right model only while the dipole is upright: the real tail's
    /// plasma sheet follows the dipole near the Earth and the solar wind far down it, so at the solstices the
    /// whole nightside configuration leans. A flat sheet beside a leaning dipole is two models disagreeing about
    /// where the equator is -- which is what this kit shipped until now, deliberately, with the caveat written in
    /// plan-tree 9.37 and in the demo's own comment. This port is what closes it.
    ///
    /// The transform is the reference's, `_z_shift` in `nodes/tail.py` and `_tail_harris` in the legacy bridge --
    /// the two carry the same expression:
    ///
    ///     z_shift(x) = 0.5 tan(ps) [ (x + Rc) - sqrt( (x + Rc)^2 + a^2 ) ],    Rc = 10 R_E,  a = 4 R_E
    ///     B          = ( B0 tanh( (z - z_shift(x)) / L ), 0, 0 )
    ///
    /// ## Three properties, which are what make it a hinge rather than a shear
    ///
    /// 1. **Sunward it does nothing.** With `u = x + Rc`, the bracket is `u - sqrt(u^2 + a^2)`, whose magnitude
    ///    falls off as `a^2 / 2u`; so `z_shift -> 0` for `x -> +inf` and the dayside is untouched. That is not a
    ///    convenience: the dayside is where the dipole's own field dominates, and a transform that moved the sheet
    ///    there would be moving it where the model it belongs to is not.
    /// 2. **At `x = -Rc` it is exactly `-2 tan(ps) R_E`** -- the `u = 0` case, where the bracket is `-a`. Ten earth
    ///    radii downtail is where the hinge has grown to an earth radius or two, which is measurable on a lattice
    ///    and asserted.
    /// 3. **Down the tail it becomes a straight line of slope `tan(ps)` through `x = -Rc`**: for `u -> -inf`,
    ///    `u - sqrt(u^2 + a^2) -> 2u`. That is the reference's "far down the tail the solar wind flattens it", and
    ///    it says `z_shift / (x tan ps) -> 1`: far enough down, the sheet is as tilted as the dipole's equator.
    ///
    /// ## The sign is a cross-node convention, and it is measured rather than argued
    ///
    /// `field.dipole`'s `tilt_degrees` rotates the **point** into the dipole's frame about `+y` (`rotate_y` in
    /// `dipole.cpp`), so a positive tilt puts that dipole's magnetic equator on `z = x tan(ps)` -- **below** the
    /// plane in the tail, where `x < 0`. The reference's hinge has the same sense: its `z_shift` is negative for
    /// `x < -Rc` and `ps > 0`. This is exactly the kind of agreement that two individually-correct nodes can get
    /// wrong, and its failure mode is a picture that looks like a magnetosphere with the sheet leaning away from
    /// its own dipole. So the case asserts the **agreement**, not only the formula: in the tail the sheet's
    /// displacement is on the dipole's side of the plane and smaller than the dipole's own equator --
    /// `0 < -z_shift < -x tan(ps)`.
    ///
    /// ## Degrees here, radians inside, and where the conversion lives
    ///
    /// The reference carries `ps` in radians and its engine wants radians. This kit's driving socket is
    /// `field.dipole`'s `tilt_from`, which is in degrees -- one date has to feed both nodes without a conversion
    /// node between them -- so this port is in degrees too, and the radians conversion happens here, inside the one
    /// model that has an opinion about radians. The port's unit symbol says `deg`, which is the same way the date
    /// driver states its own output.
    ///
    /// An empty socket is **zero degrees**, which is the unhinged sheet: a graph written before this port existed
    /// keeps its field **bit for bit**, and the bake says so by skipping the transform entirely rather than by
    /// multiplying by `tan(0)`.
    static constexpr qp::graph::PortNumber kPortSheetHinge = 12;

    /// @brief The activity socket: wire a `source.kp` here and the tail's own three numbers follow that index.
    ///
    /// ## One socket that sets three numbers, and why it is still one socket
    ///
    /// The reference's `tail.py` derives everything about the sheet from the index: `B00 = 30 + 5 Kp` nanotesla for
    /// the lobe field, `Bz0 = 1.5 + 0.3 Kp` for the northward component, and `L0 = 1.5 R_E` for the half-thickness.
    /// This port makes that available **without taking the parameters away**: with nothing wired, `b0` and
    /// `half_thickness` (and `bz`) are the model, which is what every graph written before this port means and what
    /// an experiment that wants to hold the tail still uses; with a wire, the index decides and the parameters are
    /// the fallback. That is the shape `field.magnetopause`'s Kp socket established and the reason it exists there:
    /// a driver that set one of a model's numbers would leave the others describing a different day.
    ///
    /// The relations themselves are the **driver's** (`SourceNodes::lobe_field_t_for_kp` and
    /// `tail_bz_t_for_kp`), for the reason recorded there: `30 + 5 Kp` is a solar-wind relation that a tail model
    /// happens to use, not a property of a current sheet.
    static constexpr qp::graph::PortNumber kPortSheetKp = 13;

    /// @brief Which Harris profile the sheet carries, as an enum: the reference's `model`, minus what it refuses.
    ///
    /// The reference offers four choices -- `off`, `harris`, `flaring`, `kan` -- and this port offers two, for
    /// reasons that are each a decision rather than a simplification:
    ///
    ///   - **`off` is not ported.** It answers with `(0, 0, 0)`, and a zero field is a legal physical state: a node
    ///     whose "switched off" and "the tail is zero" look the same is reporting an unconfigured value as a
    ///     measurement. A graph that wants no tail deletes the node, or masks it.
    ///   - **`kan` is not ported as a third name.** The reference's own comment says it and `flaring` are *the same
    ///     expression*, so offering both would be two names for one model -- a document that said `kan` would load
    ///     as `flaring` and nobody would know which one they had chosen.
    ///
    /// The default is `harris`, which is the model this node already was: a graph written before this port existed
    /// keeps its field **bit for bit**, and the case asserts that rather than assuming it.
    static constexpr qp::graph::PortNumber kPortSheetModel = 14;

    /// @brief The northward component the sheet carries, in tesla: the reference's `Bz0`.
    ///
    /// A Harris sheet has no `B_z` at all, and a real tail's field lines are partly **closed** across it, so the
    /// reference adds a uniform component. Zero is this port's default and is what the node baked before the port
    /// existed; the Kp socket supplies `1.5 + 0.3 Kp` nanotesla when one is wired.
    static constexpr qp::graph::PortNumber kPortSheetBz = 15;

    /// @brief The hinge distance, in earth radii: how far downtail the sheet stops following the dipole.
    static constexpr double kHingeDistanceRe = 10.0;
    /// @brief The hinge's rounding length, in earth radii.
    ///
    /// The reference writes `16` inside the square root, which is `4^2` in earth radii -- a length, not a number.
    /// Writing it as `4 R_E` here is what keeps the expression dimensionally checkable: a bake that added
    /// `16 m^2` to a metre-squared quantity would be wrong by twelve orders of magnitude and would still produce a
    /// smooth, plausible sheet (the `16` only rounds the corner at `x = -Rc`; the slope far down the tail does not
    /// depend on it at all, which is why the case asserts the slope separately from the corner).
    static constexpr double kHingeHalfWidthRe = 4.0;

    /// @brief The default lobe field, in tesla: five nanotesla, the quiet-time tail's order.
    static constexpr double kDefaultSheetB0 = 5.0e-9;
    /// @brief The default half-thickness, in metres: two earth radii.
    /// @brief The unflared half-thickness the reference's index-driven tail uses, in earth radii.
    ///
    /// The reference writes `L0 = 1.5` and this kit's own default is two, so the two are close and were chosen
    /// independently -- which is worth stating rather than quietly reconciling: the parameter is what an author
    /// types, and the index-driven value is the model's.
    static constexpr double kReferenceTailHalfThicknessRe = 1.5;
    /// @brief The distance over which the reference's tail flares, in earth radii: the `15` in `xt / 15`.
    static constexpr double kFlareDistanceRe = 15.0;
    /// @brief How the half-thickness grows downwind: the reference's exponent `0.6`.
    static constexpr double kFlareThicknessExponent = 0.6;
    /// @brief How the lobe field decays downwind: the reference's exponent `0.5`.
    static constexpr double kFlareFieldExponent = 0.5;
    static constexpr double kDefaultSheetThicknessM = 2.0 * kEarthRadiusM;

    /// @brief What a current-sheet node's parameters say.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   `half_thickness_m > 0`
    /// @errors      noexcept
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_current_sheet_carries_the_current_it_implies
    struct SheetSpec final {
        /// The lobe field the sheet saturates to, in tesla.
        double b0_tesla = kDefaultSheetB0;
        /// The half-thickness, in metres.
        double half_thickness_m = kDefaultSheetThicknessM;
        /// The northward component, in tesla, applied uniformly. Zero is a **pure Harris sheet** and is what the
        /// sheet baked before `kPortSheetBz` existed -- asserted bit for bit rather than assumed.
        double bz_tesla = 0.0;
        /// Whether the sheet flares downwind: the reference's `flaring` model.
        ///
        /// A `bool` in the spec rather than the enum index it is read from, for the reason `ImfSpec::polarity`
        /// gives: the spec is the model's vocabulary and the panel's stops at the reader.
        bool flaring = false;
        /// The hinge tilt, in degrees, from `kPortSheetHinge`. Zero when nothing is wired, which is the
        /// unhinged sheet and is what a graph written before that port existed carries.
        ///
        /// In the spec rather than passed alongside the bake because it **is** part of what the model says:
        /// the two numbers above describe a sheet in a plane, and this one says which plane. The Kp socket on
        /// `field.magnetopause` is the other arrangement (an override applied in the evaluator) and the
        /// difference is real: there the socket *replaces* a parameter, here it supplies something no
        /// parameter could express -- a tilt that belongs to the dipole the date driver also feeds.
        double hinge_degrees = 0.0;
    };

    /// @brief A current-sheet node's parameters, read from the node itself.
    ///
    /// @param node The node. Borrowed.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The three numbers the node carries, or the defaults for the ones it does not -- including a
    ///              hinge of zero, because a socket is not a parameter and a node read on its own has no wire
    /// @invariant   One reader, two sources, as every other parameter reader in this kit
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_current_sheet_carries_the_current_it_implies
    [[nodiscard]] static SheetSpec read_sheet(const graph::Node& node) noexcept;

    /**
     * @brief What an IMF node's ports say.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        none
     * @invariant   `angle_degrees` lies in `[kMinImfAngleDegrees, kMaxImfAngleDegrees]` and `kp` in
     *              `[SourceNodes::kMinKp, SourceNodes::kMaxKp]`, because the reader clamps both
     * @errors      noexcept
     * @frozen      no
     * @tests       magnetosphere.field_nodes.the_imf_is_a_parker_spiral
     */
    struct ImfSpec final {
        /// The activity index the magnitude follows: the reference's `3 + 0.5 Kp`.
        double kp = 2.0;
        /// The spiral angle, in degrees from the Sun-Earth line.
        double angle_degrees = kDefaultImfAngleDegrees;
        /// `-1` for the standard sector (the field pointing sunward along the spiral), `+1` for the reverse.
        ///
        /// A **number** rather than the enum index it is read from, because the sign is what the arithmetic
        /// wants: the spec is the model's vocabulary, and the panel's vocabulary stops at `read_imf_from`.
        double polarity = -1.0;
    };

    /**
     * @brief An IMF node's ports, read from the node itself.
     *
     * @param node The node. Borrowed.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The three values the node carries, or the defaults for the ones it does not -- including the
     *              quiet-time index, because the socket is not a parameter and a node read on its own has no wire
     * @invariant   One reader, two sources, as every other parameter reader in this kit
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.the_imf_is_a_parker_spiral
     */
    [[nodiscard]] static ImfSpec read_imf(const graph::Node& node) noexcept;

    /**
     * @brief The same reader for an evaluator's own inputs -- the one through which the Kp socket is seen.
     *
     * @param inputs The evaluator's inputs. Borrowed for the call.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The same values `read_imf` gives for the same ports, plus the index when one is wired
     * @invariant   One reader, two sources
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.field_nodes.the_imf_is_a_parker_spiral
     */
    [[nodiscard]] static ImfSpec read_imf_from(const graph::InputView& inputs) noexcept;

    /// @brief The same reader for an evaluator's own inputs.
    ///
    /// The difference from the reader above is the hinge: an `InputView` carries the values that arrived on
    /// wires as well as the node's own parameters, so this is the reader through which a wired tilt reaches the
    /// bake. `read_sheet` cannot see one, which is the honest limitation of reading a node instead of inputs.
    ///
    /// @param inputs The evaluator's inputs. Borrowed for the call.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The same values `read_sheet` gives for the same ports, plus the hinge when one is wired
    /// @invariant   One reader, two sources
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_current_sheet_carries_the_current_it_implies
    [[nodiscard]] static SheetSpec read_sheet_from(const graph::InputView& inputs) noexcept;

    /// @brief Mixes two fields along `x` **without opening a divergence**: the port of the reference's
    /// `tail_blend` and `internal_blend` nodes, which differ in one number.
    ///
    /// ## Why this is a type rather than two wires
    ///
    /// `(1 - w) A + w B` with `w = w(x)` can be built from `field.mul` and `field.sum` the moment something
    /// publishes `1 - w`, so the arithmetic is not the reason this node exists. The reason is that a straight
    /// blend of two divergence-free fields **is not divergence-free**:
    ///
    ///     B = (1 - w) A + w C,  w = w(x)   =>   div B = (1 - w) div A + w div C + w'(x) (C_x - A_x),
    ///
    /// and the last term is a source layer as thick as the transition. On a lattice it is a monopole sheet across
    /// the blend: the picture still looks like a magnetosphere, the field lines end in mid-air, and a traced line
    /// that stops for no reason is exactly the failure this kit exists to make loud.
    ///
    /// ## The fix: blend the flux function, not the field
    ///
    /// Where nothing depends on `y`, a divergence-free field is the curl of a single component, `B = curl(psi
    /// y_hat)`: `B_x = -dpsi/dz`, `B_z = dpsi/dx`. Blending `psi = (1 - w) psi_a + w psi_c` and taking the curl of
    /// the result gives
    ///
    ///     B_x = (1 - w) A_x + w C_x,        B_y = (1 - w) A_y + w C_y,
    ///     B_z = (1 - w) A_z + w C_z + w'(x) (psi_c - psi_a),
    ///
    /// which is **exactly** the curl of the blended `psi`, so its divergence is zero wherever the inputs' is. The
    /// extra term is not a repair bolted on: it is what the product rule contributes, and it is the whole content
    /// of this type.
    ///
    /// `psi` is read back from each input by integrating `-B_x` in `z` from the lattice's lowest `z` -- the
    /// **same anchor for both inputs**, so the integration constant, a function of `x` that no table determines,
    /// cancels in the difference `psi_c - psi_a` instead of being chosen twice. That is also why the correction
    /// needs no model parameters: `B0` and `L` of a Harris sheet are the sheet node's, not the blend's.
    ///
    /// ## What the reference writes, and why the default here is not its number
    ///
    /// The reference computes the same correction from the tail model's own constants, `B0 L ln cosh(z/L) - z
    /// B_x`, which is `-(psi_tail - psi_base)` with the tail's flux in closed form and the base's flux frozen at
    /// `-z B_x`. That is this term. What differs is the factor in front: the reference multiplies it by `0.1`,
    /// which leaves ninety percent of the source layer in place. The factor is a parameter, the reference's value
    /// has a name (`kReferenceBlendCorrection`), and the case measures the residual against it rather than
    /// asserting that either number is right.
    ///
    /// ## One node instead of two
    ///
    /// The reference's two blend nodes differ in the transition's width (`2.5` and `3.0`) and in whether the
    /// tail coordinate is hinged by the dipole tilt. The width is a parameter here; the hinge is **not**, because
    /// it is a coordinate transform belonging to the model that bakes the tail, not to the operation that mixes
    /// two tables -- a node that hinged its inputs would be describing a field it was not given. It arrives with
    /// the tilted tail, as its own node.
    ///
    /// ## What it does not do
    ///
    /// The correction is defined for **poloidal** inputs: the construction is the two-dimensional one, and a
    /// field with a `y` structure has a vector potential this node does not solve for. It is also not a
    /// resampler: both inputs must already share one lattice, and a blend of two lattices is refused for the same
    /// reason `field.sum` refuses it.
    static constexpr const char* kBlendType = "field.blend";
    /// @brief The field that keeps its meaning sunward of the transition (`+x`).
    static constexpr qp::graph::PortNumber kPortBlendInner = 1;
    /// @brief The field that takes over tailward of the transition (`-x`).
    static constexpr qp::graph::PortNumber kPortBlendOuter = 2;
    /// @brief The transition's `x`, in metres: the weight is one half there.
    static constexpr qp::graph::PortNumber kPortBlendTransition = 3;
    /// @brief The transition's width, in metres: the scale over which the weight moves.
    static constexpr qp::graph::PortNumber kPortBlendWidth = 4;
    /// @brief How much of the divergence-free correction to apply: `1` is all of it, `0` is the straight blend.
    static constexpr qp::graph::PortNumber kPortBlendCorrection = 5;
    /// @brief The blend's output.
    static constexpr qp::graph::PortNumber kPortBlendOut = 1;

    /// @brief The default transition, in metres: twenty earth radii downwind, where the reference puts it.
    static constexpr double kDefaultBlendTransitionM = -20.0 * kEarthRadiusM;
    /// @brief The default width, in metres: the reference's `tail_blend` value.
    static constexpr double kDefaultBlendWidthM = 2.5 * kEarthRadiusM;
    /// @brief The default correction factor: all of the term the product rule gives.
    static constexpr double kDefaultBlendCorrection = 1.0;
    /// @brief The reference implementation's factor, kept so that reproducing its fields is one number away.
    static constexpr double kReferenceBlendCorrection = 0.1;

    /// @brief What a blend node's parameters say.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   `width_m > 0` and `correction` in `[0, 1]` for any spec `read_blend` produces
    /// @errors      noexcept
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_blend_does_not_open_a_divergence
    struct BlendSpec final {
        /// Where the weight is one half, in metres.
        double transition_m = kDefaultBlendTransitionM;
        /// The scale over which the weight moves, in metres.
        double width_m = kDefaultBlendWidthM;
        /// The fraction of the divergence-free correction to apply: `1` is the derived term, `0` the straight
        /// blend, and `kReferenceBlendCorrection` what the reference implementation applies.
        double correction = kDefaultBlendCorrection;
    };

    /// @brief A blend node's parameters, read from the node itself.
    ///
    /// @param node The node. Borrowed.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The three numbers the node carries, or the defaults for the ones it does not
    /// @invariant   One reader, two sources, as every other parameter reader in this kit
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_blend_does_not_open_a_divergence
    [[nodiscard]] static BlendSpec read_blend(const graph::Node& node) noexcept;

    /// @brief The same reader for an evaluator's own inputs.
    ///
    /// @param inputs The evaluator's inputs. Borrowed for the call.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The same values `read_blend` gives for the same ports
    /// @invariant   One reader, two sources
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.a_blend_does_not_open_a_divergence
    [[nodiscard]] static BlendSpec read_blend_from(const graph::InputView& inputs) noexcept;

    /// @brief Moves a field onto **another lattice**: the reference's `resample`, and the one node whose whole job is
    /// to change where the samples are.
    ///
    /// ## Why this is the node that makes the lattice rules livable
    ///
    /// Everything in this kit that combines two fields refuses two lattices: a sum, a product and a blend are all
    /// defined on the lattice their inputs share, because fitting one table to another would invent a field neither
    /// model produced. That refusal is honest and, without this node, it is also a dead end -- the kit could not
    /// combine a tail baked on a long thin box with a dipole baked on a cube. `resample` is the explicit step the
    /// refusals point at, and `field.mul`'s own documentation has named it as "another node" since it was written.
    ///
    /// ## The target is nine numbers, not a preset
    ///
    /// The reference takes an enum of three fixed lattices (`legacy`, `coarse`, `fine`) and says why in its own
    /// docstring: its engine had **one lattice per graph**, so a node could only choose among the ones the engine
    /// already knew. This kit has no graph lattice -- every node declares its own grid -- so the target is the same
    /// nine parameter ports every producer here carries. That removes the three hard-coded boxes and makes the node
    /// able to answer the only question that matters: *which* lattice do you want.
    ///
    /// ## The interpolation is the sampler the pusher already uses
    ///
    /// Trilinear, through the same `sample_baked` a kernel reads with, so this node introduces **no new
    /// approximation**: it is the sampler promoted to a node. That is a decision with a cost, and the cost is
    /// measurable rather than hidden: resampling onto a finer grid does not add information, and the case measures
    /// what a resample of a dipole loses and how that loss falls -- a convergence study, five source spacings
    /// halving from one earth radius to a sixteenth of one, whose error ratios came out **4.33, 4.08, 4.02, 4.00**
    /// against the four that second order predicts. The worst node's relative error was 5.9% at one earth radius
    /// and 2.6e-4 at a sixteenth of one, which is the honest size of what a table on that box can say between its
    /// nodes.
    ///
    /// ## A target that reaches outside the source is refused, not clamped
    ///
    /// `sample_baked` clamps out-of-range points to the boundary node, and it is right to: at run time a particle
    /// can leave the modelled box, and a run that grew an unbounded `1/r^3` force there would be worse than one
    /// that freezes the field at the wall. **Here the same situation is a user error**, and clamping would fill a
    /// whole slab of the target with the boundary value and call it a field -- numbers from nowhere, which is the
    /// failure this kit exists to refuse. The reference raises a `ValueError` from scipy in exactly this case; the
    /// same policy in a better channel, which is what a refusal is for.
    ///
    /// The comparison is **exact**: a target box that reaches past the source by one metre is refused, because the
    /// positions are the user's numbers and nothing here is entitled to round them.
    static constexpr const char* kResampleType = "field.resample";
    /// @brief The field to move.
    static constexpr qp::graph::PortNumber kPortResampleField = 1;
    /// @brief Where the **target** grid starts: after the field socket.
    static constexpr qp::graph::PortNumber kPortResampleOrigin0 = 2;
    /// @brief The moved field.
    static constexpr qp::graph::PortNumber kPortResampleOut = 1;

    /// @brief Publishes the magnetopause as a **smooth weight**: 1 inside the Shue surface, 0 outside.
    ///
    /// ## The node `field.mask` said would come
    ///
    /// `kMaskType`'s own documentation says it: *"the boundary that needs a smooth transition in the reference is the
    /// magnetopause model, and that is another node, arriving as one."* This is that node. The mask is an either/or
    /// region test and the case argues why it must stay one -- a mask with intermediate values would be a claim about
    /// a boundary it does not model -- and the smooth boundary arrives as a model of its own.
    ///
    /// ## The surface
    ///
    /// Shue's form: `r_mp(theta) = r0 (2 / (1 + cos theta))^alpha`, with `theta` measured from `+x`, which is the
    /// sunward direction -- the same convention `field.mask`'s `dayside` region and the convection field use. At the
    /// nose the `alpha` vanishes and the surface is exactly `r0`; at the flank (`theta = pi/2`) it is `2^alpha r0`,
    /// which is the flaring; downwind it opens without bound, which is what a paraboloid does and why the tail needs
    /// the current sheet rather than this node to be closed.
    ///
    /// The weight is `w = 1 / (1 + exp((r - r_mp(theta)) / width))`: **one inside, zero outside**, which is the
    /// orientation `field.mask` uses for its regions and the one a wire reads as "this field exists here". The
    /// reference writes the complementary weight (`1 / (1 + exp(-(r - r_mp) / 4))`, one outside) because the only
    /// thing it does with it is blend an inner and an outer field, and a blend multiplies one of them by `1 - w`
    /// anyway. Nothing is lost either way and one of the two has to be chosen; this one matches the mask.
    ///
    /// ## Two internal clamps, and why they are not a user's parameter being rounded
    ///
    /// `r` is floored - not at zero but in effect, by computing `cos theta` only where `r > 0`: at the origin the
    /// angle is undefined, and the weight there is 1 whatever the surface does, which is the model's own answer for
    /// a point at the centre of the cavity. `cos theta` is clamped **on the antipode side only**, because the Shue
    /// form diverges there -- where the fitted surface is not describing anything anyway -- and the clamp makes
    /// `r_mp` large and finite, so the weight is 1 downwind. Clamping the sunward side too would move the nose by
    /// `(2 / 1.9999)^alpha`: five parts in a hundred thousand of the standoff distance, which is small, invisible in
    /// a picture, and exactly the size of error that an assertion of `w == 0.5` **exactly** at the nose catches.
    /// Both clamps are properties of **this formula near its own singularities**, not of a number a user typed,
    /// which is why they are clamped rather than refused: the three parameters below are the user's, and those are
    /// refused when they do not describe a surface.
    static constexpr const char* kMagnetopauseType = "field.magnetopause";
    /// @brief The nose's distance, in metres: where the surface stands on the sunward axis.
    static constexpr qp::graph::PortNumber kPortMagnetopauseStandoff = 1;
    /// @brief The flaring exponent, dimensionless: how fast the surface opens away from the nose.
    static constexpr qp::graph::PortNumber kPortMagnetopauseFlaring = 2;
    /// @brief The transition's thickness, in metres.
    static constexpr qp::graph::PortNumber kPortMagnetopauseWidth = 3;
    /// @brief Where the **magnetopause** node's grid starts: after its three parameters.
    static constexpr qp::graph::PortNumber kPortMagnetopauseOrigin0 = 4;

    /// @brief The optional **driver socket**: wire a Kp index here and it decides the surface.
    ///
    /// Numbered after the nine grid ports because it was added after them, and that is the rule this file follows
    /// everywhere: a type's ports are its own, and a new one goes at the end so that a saved document keeps meaning
    /// what it meant. Wire a `source.kp` here and both the standoff distance and the flaring come from the
    /// reference's Kp model (`SourceNodes::standoff_re_for_kp`, `flaring_for_kp`); leave it empty and the two
    /// parameters below are the model, exactly as before this socket existed. **The wired value wins**, which is the
    /// answer this kit gives to "can a parameter be wired?" -- the parameter stays typed and keeps its meaning
    /// alone, the socket is optional, and the choice is visible on the canvas rather than buried in a node.
    static constexpr qp::graph::PortNumber kPortMagnetopauseKp = 13;

    /// @brief The default standoff distance, in earth radii: the textbook ten at ordinary solar wind pressure.
    ///
    /// The reference computes it from `Kp` (`r0 = 10 / pdyn^(1/3)` with `pdyn = 2 + Kp/2`), and that computation is
    /// a **driver's** job: when this kit has one, `Kp` becomes a socket and this parameter becomes the fallback.
    /// Writing the formula here instead would put a solar-wind model inside a boundary model, which is the
    /// composition mistake every other node in this file is arranged to avoid.
    static constexpr double kDefaultMagnetopauseStandoffRe = 10.0;
    /// @brief The default flaring exponent: Shue's 0.58 at zero IMF `B_z`, the reference's 0.59 at `Kp = 2`.
    static constexpr double kDefaultMagnetopauseFlaring = 0.58;
    /// @brief The default transition thickness, in metres: one earth radius.
    ///
    /// The reference uses four, because in its model this number is also the width of the **draping** layer that
    /// carries the magnetosheath field. A boundary that smears over forty percent of its own standoff distance is
    /// not a surface any more, so the default here is a boundary's thickness and the draping, when it arrives, is a
    /// different node's job with its own width.
    static constexpr double kDefaultMagnetopauseWidthM = kEarthRadiusM;
    /// @brief The lowest `cos theta` the surface is asked about: within a ten-thousandth of the antipode.
    static constexpr double kMagnetopauseMinCosine = -0.9999;

    /// @brief What a magnetopause node's parameters say.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   `standoff_m > 0`, `flaring >= 0` and `width_m > 0` for any spec `read_magnetopause` produces
    /// @errors      noexcept
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.the_magnetopause_is_a_surface_with_a_nose
    struct MagnetopauseSpec final {
        /// The nose's distance, in metres.
        double standoff_m = kDefaultMagnetopauseStandoffRe * kEarthRadiusM;
        /// The flaring exponent.
        double flaring = kDefaultMagnetopauseFlaring;
        /// The transition's thickness, in metres.
        double width_m = kDefaultMagnetopauseWidthM;
    };

    /// @brief A magnetopause node's parameters, read from the node itself.
    ///
    /// @param node The node. Borrowed.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The three numbers the node carries, or the defaults for the ones it does not
    /// @invariant   One reader, two sources, as every other parameter reader in this kit
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.the_magnetopause_is_a_surface_with_a_nose
    [[nodiscard]] static MagnetopauseSpec read_magnetopause(const graph::Node& node) noexcept;

    /// @brief The same reader for an evaluator's own inputs.
    ///
    /// @param inputs The evaluator's inputs. Borrowed for the call.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The same values `read_magnetopause` gives for the same ports
    /// @invariant   One reader, two sources
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.field_nodes.the_magnetopause_is_a_surface_with_a_nose
    [[nodiscard]] static MagnetopauseSpec read_magnetopause_from(const graph::InputView& inputs) noexcept;

    /// @brief Mixes two fields by **any** weight field, with the correction that keeps the result divergence-free.
    ///
    /// ## Why there are two blends, and what each one is for
    ///
    /// `field.blend` interpolates between its two sockets along `x` with a sigmoid it owns, and it can do that
    /// **exactly** because it owns the weight and therefore owns the weight's derivative. This node takes a weight
    /// **table** -- whatever a mask, a magnetopause or a future model publishes -- and pays for that freedom: the
    /// derivative of a table is a difference, so the correction it can apply is the same term built from a
    /// difference rather than from the closed form.
    ///
    /// That is the reopening condition `field.blend` wrote down, at the point where it is cashed: a weight that
    /// arrives with its derivative -- a pair, or a model -- makes the correction exact again, and the node that
    /// publishes such a weight is the one that knows it analytically.
    ///
    /// ## The correction, and where it is exact
    ///
    /// A blend of two fields is `(1 - w) A + w C`, whose divergence carries `grad w . (C - A)` -- a source layer
    /// wherever the weight changes. The cure is to blend the **vector potentials** instead: for `A_a` and `A_c` with
    /// `curl A_a = A` and `curl A_c = C`,
    ///
    ///     curl (w A_c + (1 - w) A_a) = (1 - w) A + w C + grad w x (A_c - A_a),
    ///
    /// which is a curl and therefore exactly divergence-free **whatever the weight is**. So the whole question is
    /// which potentials are available, and a table does not carry one.
    ///
    /// This node builds the potential a **poloidal** pair of fields has: `A = psi y_hat`, with `psi` recovered by
    /// integrating `-B_x` in `z` -- the same rule, the same shared anchor and the same restriction `field.blend`
    /// documents. Where the inputs vary in `y` the construction is the right shape and the wrong value, and the case
    /// says which regime it is measuring rather than claiming the exact one everywhere.
    ///
    /// ## Why not the reference's potential
    ///
    /// The reference uses `A = (B x r) / 2`, which is a genuine potential exactly when `B` is **uniform**, and it
    /// multiplies the resulting term by `clamp(r / 10 R_e, 0, 1)` -- switching the correction off inside ten earth
    /// radii. Measuring that construction here is what settled it: with a dipole on one socket and a uniform field
    /// on the other, mixed by a magnetopause weight, its worst divergence came out **0.74 times the uncorrected
    /// one**, that is, the term made the field worse -- over exactly the region the reference's damping silences.
    /// The damping was a patch for a construction that does not hold there. With the poloidal potential the same
    /// measurement improves instead (both numbers are in the case), so that is what this node builds, and the
    /// reopening condition is a field that publishes its own vector potential: then neither approximation is needed
    /// and the correction is exact everywhere.
    ///
    /// ## The gradient is a difference of the table, and the boundary is said out loud
    ///
    /// `grad w` is a central difference in the interior and a one-sided difference on the boundary faces, where
    /// there is only one side to look at. That makes the correction second order inside and first order in the two
    /// boundary layers, which is a property of differentiating a table rather than a defect of the model -- and it
    /// is the reason a linear weight is used in the exactness case: a central difference reproduces a linear
    /// function exactly, so the only error left there is the potential's own.
    ///
    /// ## What is not ported
    ///
    /// The reference's `clamp(r / 10 R_e, 0, 1)` factor is gone, and the measurement above is why it existed: it
    /// silenced a correction that was making things worse near the Earth. With a construction that does not need
    /// silencing, the damping has nothing left to do -- and if a future model wants one, it comes back as a measured
    /// decision with a number beside it rather than as a copied constant.
    static constexpr const char* kMixType = "field.mix";
    /// @brief The field that wins where the weight is zero.
    static constexpr qp::graph::PortNumber kPortMixA = 1;
    /// @brief The field that wins where the weight is one.
    static constexpr qp::graph::PortNumber kPortMixB = 2;
    /// @brief The weight table, dimensionless: `0` gives the first socket, `1` the second.
    static constexpr qp::graph::PortNumber kPortMixWeight = 3;
    /// @brief How much of the divergence-free correction to apply.
    static constexpr qp::graph::PortNumber kPortMixCorrection = 4;
    /// @brief The mixed field.
    static constexpr qp::graph::PortNumber kPortMixOut = 1;

    /// @brief The default correction factor: all of the term the vector potentials contribute.
    static constexpr double kDefaultMixCorrection = 1.0;

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
 * @brief Bakes the Volland-Stern shielding coefficient onto `grid`: `w = min(1, (r / r0)^2)`.
 *
 * The model and the construction's consequences are argued on `kShieldType`. What belongs here is what the case
 * can hold the arithmetic to, and what is refused.
 *
 * ## The two regimes are exact, and the shell is where they meet
 *
 * Inside `r0` the coefficient is `(r/r0)^2` and outside it is **one**, so a graph that multiplies this by a field
 * leaves that field untouched beyond the shielding radius -- to the last bit, not approximately. On the shell
 * itself the two expressions agree because `r/r0` is one there, which is what makes `r0` a *parameter with a
 * visible meaning* rather than a scale factor. And `(r/r0)^2` is zero at the origin and has no singularity there,
 * so unlike the reference implementation this formula needs no floor, and none is applied.
 *
 * @param r0_m  The radius at which the coefficient reaches one, in metres. Must be positive and finite.
 * @param grid  Where to bake. At least two nodes an axis and a positive spacing; anything else is refused.
 * @param key   Who is publishing it.
 * @param fields The store. Borrowed; the samples are moved into it on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable **scalar** volume of dimensionless coefficients, in
 *              `(0, 1]`, equal at every node to `min(1, (r / r0)^2)`
 * @invariant   On false the store is unchanged
 * @errors      Returns false rather than throwing, for a non-positive or non-finite `r0_m`, or for a grid that
 *              cannot be baked
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_shield_suppresses_convection_inside_its_radius
 */
[[nodiscard]] bool bake_shield(double r0_m, const GridSpec& grid, qp::graph::field::FieldKey key,
                               qp::graph::field::FieldSet& fields);

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
 * @brief Bakes a Harris current sheet `B = (B_x tanh((z - z_shift(x))/L_x), 0, Bz0)` onto `grid`.
 *
 * The model, its parameters and the current it implies are argued on `kCurrentSheetType`; the hinge and its three
 * properties are argued on `kPortSheetHinge`; the two profiles on `kPortSheetModel`, and the activity-driven numbers
 * on `kPortSheetKp`. What belongs here is what the arithmetic decides:
 *
 *   - `tanh` is evaluated per node from the node's own `z` **displaced by the hinge**, and the displacement is a
 *     function of that node's `x` alone -- the sheet stays one-dimensional in the hinged coordinate, which is why
 *     it still carries a current in `y` and nothing else.
 *   - `B_y` is written as an **exact zero** rather than as an expression that happens to be small: a sheet is
 *     one-dimensional in `x` and `z`, and a `B_y` computed from a formula that ought to vanish would leave a number
 *     in the table that is not the model. The case checks it at every node. `B_z` is **not** zero once the model
 *     carries a closed fraction -- see `kPortSheetBz` -- so it is written from the spec instead.
 *   - the flaring profile moves **two** numbers with distance, and they move differently: the half-thickness grows
 *     as `L0 (1 + (xt/15)^0.6)` while the lobe field decays as `B00 / (1 + (xt/15)^0.5)`, which is the tail's flux
 *     spreading over a wider sheet. `xt` is the downtail distance clipped at zero, and the clip is the model saying
 *     that this is a *tail*: sunward of the Earth both factors are exactly one -- `std::pow(0.0, e)` is zero for
 *     any positive `e` -- so the dayside is the unflared Harris profile to the last bit, and the case asserts that
 *     at `x = 0` rather than treating it as an approximation.
 *   - A hinge of exactly zero **skips the transform** rather than evaluating `0.5 tan(0) (u - sqrt(u^2 + a^2))`.
 *     Two reasons, and the second is the one that made it a decision: the product is `0 * inf` -- a NaN -- for
 *     coordinates large enough that `u^2` overflows, and "no hinge" has to be bit-for-bit the same table as the
 *     unhinged sheet rather than merely equal to within a rounding.
 *
 * The displacement is computed in metres from `kHingeDistanceRe` and `kHingeHalfWidthRe` times the earth radius,
 * so the reference's `16` -- four earth radii squared, with `x` in earth radii -- becomes a length here. The
 * quantity inside the square root is a length squared in both readings, which is the check that catches the one
 * wrong way to write it: `+ 16.0` on a metre-scale `u` is twelve orders of magnitude too small and leaves a sheet
 * that still looks smooth, because the constant only rounds the corner and never touches the far-tail slope.
 *
 * @param spec  The lobe field, the half-thickness, the northward component, the hinge in degrees and the profile. A hinge outside the **open**
 *              interval `(-90, 90)` degrees is refused: `tan` has its pole at ninety, so the endpoints themselves
 *              are already a sheet on edge, and a model that accepted them would bake a table of saturated lobe
 *              field out of a number that is really an infinity.
 * @param grid  Where to bake. At least two nodes an axis and a positive spacing.
 * @param key   Who is publishing it.
 * @param fields The store. Borrowed; the samples are moved into it on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable vector volume in tesla whose `x` component is
 *              `B_x tanh((z - z_shift(x))/L_x)` at every node, whose `y` component is zero and whose `z` component
 *              is `Bz0`
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for a non-finite lobe field, a non-finite northward component, a
 *              non-positive or non-finite half-thickness, a non-finite hinge or one outside `(-90, 90)` degrees, or
 *              a grid that cannot be baked
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_current_sheet_carries_the_current_it_implies,
 *              magnetosphere.field_nodes.the_tail_sheet_hinges_with_the_dipole,
 *              magnetosphere.field_nodes.the_tail_flares_and_follows_the_index
 */
[[nodiscard]] bool bake_current_sheet(const FieldNodes::SheetSpec& spec, const GridSpec& grid,
                                      qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Bakes the reference's Parker-spiral IMF onto `grid`: one uniform vector, the same at every node.
 *
 * The model, its two constants and the factor whose meaning is argued are on `kImfType`; what belongs here is what
 * the arithmetic decides:
 *
 *   - the magnitude is `(3 + 0.5 Kp)` nanotesla times that factor, converted to tesla **here**, because the port
 *     says `T` and a table in nanotesla would be a table whose dimension lies;
 *   - the direction is a rotation of that one vector in the equatorial plane, so the field's magnitude cannot
 *     depend on the angle. That is asserted by the case as an angle-independence, which is the property that
 *     distinguishes "rotate the vector" from "scale its components";
 *   - `B_z` is written as an **exact zero**, not as an expression that ought to vanish. The reference's model has no
 *     clock angle, so zero is what it says, and a `1e-30` in a table is a number that is not the model.
 *
 * The angle is expected already clamped by the reader; a **non-finite** one is refused here instead, for the same
 * reason the hinge's is: there is no edge to clamp it to.
 *
 * @param spec  The activity index, the spiral angle in degrees and the polarity sign.
 * @param grid  Where to bake. At least two nodes an axis and a positive spacing.
 * @param key   Who is publishing it.
 * @param fields The store. Borrowed; the samples are moved into it on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable vector volume in tesla whose every node carries
 *              `(sign b_total cos theta, -sign b_total sin theta, 0)`
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for a non-finite index, angle or polarity, or a grid that cannot
 *              be baked
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_imf_is_a_parker_spiral
 */
[[nodiscard]] bool bake_imf(const FieldNodes::ImfSpec& spec, const GridSpec& grid, qp::graph::field::FieldKey key,
                            qp::graph::field::FieldSet& fields);

/**
 * @brief Blends two fields along `x` with the correction that keeps the result divergence-free.
 *
 * The model, the derivation and the reference's `0.1` are argued on `kBlendType`. What belongs here is how the
 * term is computed and what the caller must already have checked.
 *
 * ## The flux is integrated from the tables, not from the models
 *
 * `psi_outer - psi_inner` is built by integrating `-B_x` in `z` on the lattice, **the two inputs differenced
 * before the integral**: the recurrence is `dpsi_k = dpsi_{k-1} - dz (bx_k + bx_{k-1}) / 2` with `dpsi_0 = 0`, so
 * the anchor's constant never enters. Integrating the two inputs separately and subtracting afterwards would work
 * as well and would invite the question of what each `psi` is anchored to; here the question cannot be asked.
 *
 * That is also why this node takes **no model parameters**: the reference evaluates `B0 L ln cosh(z/L)`, the
 * Harris sheet's flux in closed form, so its blend node had to be told the sheet's two constants. In this kit
 * those belong to the sheet node, and the blend reads whatever was baked.
 *
 * ## The weight's derivative is analytic because the weight is
 *
 * `w = 1 / (1 + exp((x - transition) / width))` and `w' = -w (1 - w) / width`, evaluated from the node's own
 * coordinate rather than differenced from a weight table. This is the decision `bake_convection` records about its
 * potential: a finite difference of a function known in closed form is a second approximation stacked on the
 * interpolation, and it would make the exactness this case rests on impossible to state. A blend that took an
 * arbitrary weight *table* would have to differentiate it numerically, and the reopening condition for that is a
 * weight that arrives with its derivative -- a pair, or a model -- rather than a table alone.
 *
 * ## The restriction, said where a caller will read it
 *
 * The correction is the two-dimensional poloidal one. It is exact for fields with no `y` structure; for a field
 * with one it is a term of the right shape and the wrong value. That cannot be detected from a table, so it is
 * documented rather than checked -- and everything the tables *can* answer is checked.
 *
 * @param inner  The field that keeps its meaning sunward of the transition. A readable f64 volume of vectors.
 * @param outer  The field that takes over downwind of it. Same lattice **and same dimension**: a blend of tesla
 *               with volts per metre is refused rather than added, and the port types cannot catch that because
 *               both are vector fields.
 * @param grid   The geometry the tables were baked on: origin, spacing and counts, because `abi::LatticeDesc`
 *               carries counts and not positions. Counts that disagree with `inner`'s description are refused --
 *               two answers to "where are the samples" is one answer too many.
 * @param spec   The transition, the width and the correction factor. A non-positive width, a correction outside
 *               `[0, 1]`, or a non-finite number in any of the three is refused rather than clamped: a blend that
 *               quietly became a straight one would look like a field and be a bug.
 * @param key    Who is publishing, for the store's key.
 * @param fields The store. Mutated on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is the convex combination in `x` and `y`, plus `w' * dpsi * correction`
 *              in `z`, described exactly as `inner` is
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for an unreadable or non-volume input, a scalar or f32 input,
 *              lattices whose counts disagree, a grid that disagrees with them, dimensions that differ, and the
 *              spec refusals above
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_blend_does_not_open_a_divergence
 */
[[nodiscard]] bool bake_blend(const qp::graph::field::FieldValue& inner, const qp::graph::field::FieldValue& outer,
                              const GridSpec& grid, const FieldNodes::BlendSpec& spec,
                              qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Resamples a published field onto another lattice, trilinearly, refusing a target the source cannot cover.
 *
 * The model and the two policies are argued on `kResampleType`. What belongs here is the shape of the call and the
 * one check that has no counterpart in the other bakes.
 *
 * ## Two grids, and the source's is the caller's business
 *
 * `abi::LatticeDesc` carries counts and not positions, so a table cannot say where its samples are. The **target**
 * is the node's own declaration and arrives as a `GridSpec`; the **source**'s geometry has to come from the node
 * that baked it, which is what `resolve_field_origin` is for. The counts in `source_grid` are compared against the
 * source's description: a caller that resolved the wrong node's grid would otherwise resample a box that is not
 * where the samples are -- a plausible field from the wrong place, which is the failure `plan.hpp` warns about at
 * length.
 *
 * ## Refuse, do not clamp
 *
 * Every target node must lie inside the source box, and the test is exact rather than tolerant: `origin + (n - 1) *
 * spacing` on each axis, compared with `<=`. A tolerance here would decide *where to refuse* rather than what the
 * numbers mean, and the refusals in this kit are about meaning.
 *
 * @param source      The field to move. A readable f64 volume of vectors.
 * @param source_grid Where the source's samples are: origin, spacing and counts.
 * @param target_grid The lattice to publish on. At least two nodes an axis, and inside the source's box.
 * @param key         Who is publishing, for the store's key.
 * @param fields      The store. Mutated on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is the source sampled trilinearly at every target node, described
 *              exactly as the source is -- same dimension, the target's lattice
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for an unreadable, non-volume, non-vector or f32 source, for a
 *              `source_grid` whose counts disagree with the source's description, for a target that cannot be
 *              baked, and for a target that reaches outside the source on any axis
 * @complexity  O(target points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_resample_moves_the_samples_and_adds_no_information
 */
[[nodiscard]] bool bake_resample(const qp::graph::field::FieldValue& source, const GridSpec& source_grid,
                                 const GridSpec& target_grid, qp::graph::field::FieldKey key,
                                 qp::graph::field::FieldSet& fields);

/**
 * @brief Bakes the magnetopause weight onto `grid`: `1` inside the Shue surface, `0` outside.
 *
 * The surface and the two clamps are argued on `kMagnetopauseType`. What belongs here is what the case can hold the
 * arithmetic to, and what is refused.
 *
 * ## The two points where the closed form is exact
 *
 * `r_mp(0) = r0` because the exponent multiplies a factor of one, and `r_mp(pi/2) = 2^alpha r0`. At both, the
 * weight is `1 / (1 + exp(0)) = 0.5` **exactly** -- not to a tolerance -- so a grid with a node on the nose and a
 * node on the flank turns the model into two exact assertions. Everything else about the surface is checked by
 * **finding** it: the case bisects the baked weight along a direction to locate the half level and compares that
 * radius with the closed form, which tests the whole shape rather than the values at chosen nodes.
 *
 * ## What is refused, and what is clamped
 *
 * A non-positive standoff, a negative flaring exponent, a non-positive width and a non-finite number anywhere are
 * refused: each of them describes something that is not a boundary, and clamping one would hand back a surface the
 * user did not ask for. The **internal** singularities of the formula -- the undefined angle at the origin and the
 * diverging factor at the antipode -- are clamped, because they are properties of the model rather than of a
 * parameter, and both clamps leave the weight at the value the model gives: one.
 *
 * @param spec  The standoff distance, the flaring exponent and the transition's thickness, in SI.
 * @param grid  Where to bake. At least two nodes an axis and a positive spacing; anything else is refused.
 * @param key   Who is publishing it.
 * @param fields The store. Borrowed; the samples are moved into it on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is a readable **scalar** volume of dimensionless weights, in `[0, 1]`,
 *              equal at every node to `1 / (1 + exp((r - r_mp(theta)) / width))`
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for a non-positive standoff or width, a negative or non-finite
 *              flaring exponent, and a grid that cannot be baked
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.the_magnetopause_is_a_surface_with_a_nose
 */
[[nodiscard]] bool bake_magnetopause(const FieldNodes::MagnetopauseSpec& spec, const GridSpec& grid,
                                     qp::graph::field::FieldKey key, qp::graph::field::FieldSet& fields);

/**
 * @brief Blends two fields by a published weight table, with the vector-potential divergence correction.
 *
 * The construction and the reopening condition are argued on `kMixType`. What belongs here is the call's shape, the
 * two things it must be told that a table cannot say, and what it refuses.
 *
 * ## Everything comes from tables and the geometry comes from the caller
 *
 * Three tables in, one out, and **no grid of its own**: a mix is defined on the lattice its inputs share, like a sum
 * or a product, so `abi::LatticeDesc`'s missing positions are supplied by the caller exactly as they are for those
 * nodes. The two fields must carry the same dimension -- mixing tesla with volts per metre is not a mixed quantity
 * -- and the port types cannot catch that, because both sockets hold vector fields.
 *
 * ## Refusals
 *
 * Unreadable or non-volume inputs, a field where the weight belongs or a weight where a field belongs, counts that
 * disagree between any two of the three tables, a grid that disagrees with them, dimensions that differ, and a
 * correction outside `[0, 1]` are all refused rather than approximated: a mix that quietly became a straight blend
 * would look like a field and be a bug.
 *
 * @param a        The field that wins where the weight is zero. Readable f64 volume of vectors.
 * @param b        The field that wins where the weight is one. Same lattice and dimension as `a`.
 * @param weight   The weight table. A readable f64 volume of **scalars**, on the same lattice.
 * @param grid     Where the samples are: origin, spacing and counts.
 * @param correction The fraction of the vector-potential term to apply: `1` is the construction above, `0` the
 *                 straight blend the case measures against.
 * @param key      Who is publishing, for the store's key.
 * @param fields   The store. Mutated on success.
 *
 * @ownership   owns the samples it publishes on success
 * @thread      main
 * @pre         none
 * @post        On true, `fields.view(key)` is `(1 - w) a + w b + correction * (grad w x (A_b - A_a))` at every node,
 *              described exactly as `a` is
 * @invariant   On false the store is unchanged
 * @errors      Returns false -- never throws -- for the refusals listed above
 * @complexity  O(points)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.field_nodes.a_mix_blends_the_potentials_not_the_fields
 */
[[nodiscard]] bool bake_mix(const qp::graph::field::FieldValue& a, const qp::graph::field::FieldValue& b,
                            const qp::graph::field::FieldValue& weight, const GridSpec& grid, double correction,
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
