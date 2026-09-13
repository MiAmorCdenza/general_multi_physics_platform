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

#include <qp/plugins/magnetosphere/geometry.hpp>

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
                                qp::graph::field::FieldSet& fields);

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
