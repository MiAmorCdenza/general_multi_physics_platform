/**
 * @file plan.hpp
 * @brief The composition root's content half: a graph's pusher nodes become a `StepPlan` per node, with the field
 *        a node is **wired to** bound to the slot it reads.
 *
 * ## Why this is content and not `core/`
 *
 * `core/graph/particles` deliberately does not depend on `core/graph/domain`, and its own CMakeLists says why: "A
 * `DomainPlan` says which nodes belong to the particle domain and in what order, and something has to turn that
 * order into a `StepPlan`; that something is the composition root, because the mapping from a node's parameters
 * to a kernel's parameter block is content -- the same reason `IOperatorBinder` lives in the plugin that ships
 * the operator rather than in `execution`." This file is that something, for this kit.
 *
 * ## The binding, and why it needs no new concept
 *
 * The reference implementation's principle -- one node outputs one independent force field, and composition
 * happens by **wiring the graph** -- has a consequence worth stating: a field reaches a pusher through an
 * **edge**, and the port the edge lands on *is* the slot. There is nothing to declare and nothing to configure:
 *
 *     field.dipole:1  --wire-->  particle.boris:1   means   SlotName::magnetic
 *     field.dipole:1  --wire-->  particle.boris:2   means   SlotName::electric
 *     <scalar field>  --wire-->  particle.boris:3   means   SlotName::drag
 *
 * That is why `StepPlan::fields` is indexed by `SlotName` rather than by port number: the pusher's port layout is
 * what gives a wire its meaning, and it is declared once, in `PusherNodes::node_types`. A graph whose user wired
 * a dipole into the drag socket is refused by the **port types** (`kVectorField` into `kScalarField`), one layer
 * up, which is where a type error belongs.
 *
 * ## What "the grid" means here, and why an unknown one is refused rather than defaulted
 *
 * A `field::FieldValue` describes a lattice's **counts** and not where it sits, so the kernel is told the grid's
 * origin and spacing through its parameter block. Those six numbers must describe **the same lattice the field
 * node baked**, or the pusher samples a box that is not where the samples are -- producing a plausible field from
 * the wrong place, which no test of either half would catch. So the grid is read from the node the magnetic input
 * is wired to, through the same `FieldNodes::read_from` the baker uses (one reader, two callers), and a source
 * node whose type this build does not know how to ask is **refused by name** rather than defaulted. Defaulting
 * would be the failure mode this paragraph exists to prevent.
 *
 * ## Order of operations, and what is deliberately not checked here
 *
 * The builder does not call `prepare` on the kernels, does not validate the graph, and does not decide what a run
 * should be. `ParticleExecutor::prepare` is what prepares a kernel -- once, in order, before the first step --
 * and it is also what produces `PlanRefusal::slot_unbound` when a step requires a field nobody bound. A builder
 * that pre-checked that would be a second answer to a question that already has one.
 *
 * @ownership   owns the kernels it builds; borrows the graph and the store
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   One `StepPlan` per pusher node in the order, in the same relative order
 * @errors      See `build_particle_plan`
 * @frozen      no
 * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads,
 *              magnetosphere.plan.an_unwired_pusher_is_refused_by_the_executor
 */
#pragma once

#include <qp/graph/field/field_set.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/kernels/kernel.hpp>
#include <qp/graph/particles/executor.hpp>
#include <qp/graph/structure.hpp>
#include <qp/host/host.hpp>

#include <qp/plugins/magnetosphere/field_nodes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace qp::plugins::magnetosphere {

/**
 * @brief The particle-domain node types this kit ships, and the port numbers its binder reads.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every port number here is one the descriptions in `node_types` declare
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.pusher_nodes.the_type_declares_the_ports_the_builder_reads
 */
class PusherNodes final {
public:
    /// @brief The relativistic Boris push, as a particle-domain node.
    static constexpr const char* kBorisType = "particle.boris";

    /// @brief The magnetic field socket. `kVectorField`; what lands here becomes `SlotName::magnetic`.
    static constexpr qp::graph::PortNumber kPortMagnetic = 1;
    /// @brief The electric field socket, in volts per metre. Optional.
    static constexpr qp::graph::PortNumber kPortElectric = 2;
    /// @brief The drag socket, in per second. Optional.
    static constexpr qp::graph::PortNumber kPortDrag = 3;
    /// @brief The radius at which a particle is retired, in earth radii.
    static constexpr qp::graph::PortNumber kPortMaxRangeRe = 4;
    /// @brief The gravity multiplier: 0 disables it, 1 is the Earth's own field.
    static constexpr qp::graph::PortNumber kPortGravity = 5;
    /// @brief The largest number of sub-steps one step may take.
    static constexpr qp::graph::PortNumber kPortSubstepCap = 6;
    /// @brief The speed limit, as a fraction of c.
    static constexpr qp::graph::PortNumber kPortSpeedLimit = 7;
    /// @brief Whether the drag socket is read at all.
    static constexpr qp::graph::PortNumber kPortUseDrag = 8;
    /// @brief The **state channel**: the particles this pusher advances, wired in from an emitter or a pusher.
    ///
    /// No value crosses it. It exists so the wire can be drawn, type checked and seen by the domain layer's
    /// reachability walk, and so a run can ask the graph which emitter feeds this pusher instead of looking for
    /// an emitter node somewhere. `emitter.hpp` documents the same decision from the other end.
    static constexpr qp::graph::PortNumber kPortStateIn = 9;
    /// @brief The state this pusher produces, after its step: the state channel's other end.
    static constexpr qp::graph::PortNumber kPortStateOut = 1;

    /// @brief The default retirement radius, in earth radii.
    ///
    /// Ten, which is where a dipole picture stops being a picture of the magnetosphere: the magnetopause stands
    /// at about ten earth radii on the sunward side, and beyond it the field this kit models is not the field
    /// that is there. A particle past ten radii is reported as having left, which is honest, rather than
    /// integrated in a field that does not exist.
    static constexpr double kDefaultMaxRangeRe = 10.0;
    /// @brief The default sub-step cap. A step that needs more than this is a configuration finding.
    static constexpr double kDefaultSubstepCap = 4096.0;

    PusherNodes() = delete;

    /**
     * @brief The node types this build ships, ready to register with a host.
     *
     * @ownership   owns the returned descriptions
     * @thread      main
     * @pre         none
     * @post        One description for the pusher, with the port numbers the builder reads
     * @invariant   The type declares no **outputs**: its product is the particle state, not a port value
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.pusher_nodes.the_type_declares_the_ports_the_builder_reads
     */
    [[nodiscard]] static std::vector<qp::graph::NodeDesc> node_types();

    /**
     * @brief Registers the pusher type with `host` as a built-in.
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
     * @tests       magnetosphere.pusher_nodes.the_type_declares_the_ports_the_builder_reads
     */
    static std::size_t mount(qp::host::PluginHost& host) noexcept;

    /**
     * @brief The retirement radius a node's parameters declare.
     *
     * @param node The node instance.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The parameter's value, or `kDefaultMaxRangeRe` when it is unset
     * @invariant   A negative or non-finite parameter takes the default rather than reaching the kernel, which
     *              refuses it and would report the refusal as a refused plan
     * @errors      noexcept
     * @complexity  O(parameters)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads
     */
    [[nodiscard]] static double max_range_of(const qp::graph::Node& node) noexcept;

    /**
     * @brief The parameter block a pusher node's own parameters describe.
     *
     * The six **grid** slots are left as zero: they describe the field's lattice, not the node's behaviour, and
     * the builder fills them from the node the magnetic input is wired to. A function that filled them here
     * would have to be handed the graph as well and would be a second place where the grid is decided.
     *
     * @param node The node instance.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Every slot the kernel reads except the six grid slots is the node's value or a default
     * @invariant   Unset parameters take the runner's defaults rather than zero: a fresh node must run
     * @errors      noexcept
     * @complexity  O(parameters)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.pusher_nodes.the_type_declares_the_ports_the_builder_reads
     */
    [[nodiscard]] static qp::graph::kernels::ParamBlock param_block_of(const qp::graph::Node& node) noexcept;
};

/**
 * @brief Why a particle plan could not be built.
 *
 * Four codes rather than an error code, for the reason `PlanRefusal` gives: a refusal that has to be reported by
 * name must not travel in a channel that cannot name it. "No field is baked for the node you wired in" and "the
 * order names a node that is not there" are different findings with different fixes.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `ok` is zero and every other code means the plan was left empty rather than half built
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.plan.a_grid_it_cannot_ask_about_is_refused
 */
enum class PlanBuildRefusal : std::uint8_t {
    ok = 0,
    /// The order names an occupied slot that holds no node. A stale plan, not an empty one.
    stale_order = 1,
    /// A socket is wired to a node whose field is not in the store -- the bake did not run, or ran for a
    /// different node. Refused rather than treated as zero, which would leave the particle in a straight line.
    field_not_baked = 2,
    /// The magnetic field comes from a node whose bake grid this build does not know how to ask about.
    ///
    /// The alternative is to leave the six grid slots at zero and let the kernel refuse them, which reports a
    /// bad parameter block rather than the actual problem: a field node type this build cannot describe. It is
    /// also the safety property: an unknown grid must never be **defaulted**, because the default is a box in a
    /// different place from the samples.
    grid_unknown = 3,
};

/// @brief Stable short name of a refusal, for a message or a log line.
///
/// @param refusal The refusal to name.
///
/// @ownership   pure
/// @thread      any
/// @pre         none
/// @post        One of the names in this enumerator's list, never null
/// @invariant   Total: every enumerator has a name
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       magnetosphere.plan.a_grid_it_cannot_ask_about_is_refused
[[nodiscard]] const char* to_string(PlanBuildRefusal refusal) noexcept;

/**
 * @brief What one graph became: the steps an executor drives, and the kernels they point at.
 *
 * Non-copyable and non-movable, because `StepPlan::kernel` is a non-owning pointer into `kernels` and a copy
 * would leave two plans pointing at one set of kernels -- or, worse, a moved-from plan whose pointers outlived
 * the kernels they name. Built in place, used, and dropped.
 *
 * @ownership   owns the kernels
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every `steps[i].kernel` equals some `kernels[j].get()`
 * @errors      See `build_particle_plan`
 * @frozen      no
 * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads
 */
class BuiltPlan final {
public:
    BuiltPlan() = default;
    BuiltPlan(const BuiltPlan&) = delete;
    BuiltPlan& operator=(const BuiltPlan&) = delete;
    BuiltPlan(BuiltPlan&&) = delete;
    BuiltPlan& operator=(BuiltPlan&&) = delete;

    /// @brief Forgets everything, so a second build is reported as its own.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `steps` and `kernels` are empty and every count is zero
    /// @invariant   No step is left pointing at a kernel that has been released
    /// @errors      noexcept
    /// @complexity  O(steps)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads
    void clear() noexcept {
        steps.clear();
        kernels.clear();
        pushers = 0;
        skipped = 0;
    }

    /// The steps, one per pusher node, in the order's relative order. The executor copies this vector.
    std::vector<qp::graph::particles::StepPlan> steps{};
    /// The kernels the steps point at. Owned here; must outlive any executor built over `steps`.
    std::vector<std::unique_ptr<qp::graph::kernels::IBatchAdvancer>> kernels{};
    /// How many pusher nodes were built.
    std::size_t pushers = 0;
    /// How many nodes in the order were of some other type -- a dipole node, or anything else in the particle
    /// plan. Counted rather than ignored: "the plan built nothing" and "the plan built one step out of nine
    /// nodes" are different answers to "why is nothing moving".
    std::size_t skipped = 0;
};

/**
 * @brief The field a socket is wired to, together with the grid its source node declares.
 *
 * **One resolver, two callers.** The pusher's magnetic socket and the emitter's both need the same three answers
 * -- is anything wired here, is its field in the store, and where is its grid -- and a second implementation of
 * that lookup is a second answer to "which node's field is this", which is the failure this file's header spends
 * a paragraph on.
 *
 * **A socket with no wire is not a refusal.** It answers `ok` with an unreadable value, and whether a socket is
 * *required* is the caller's question: the pusher's magnetic socket is enforced by the step's own requirement
 * mask (so the refusal is `slot_unbound`, named), and the emitter's is `required` in its port description (so the
 * refusal is the graph validator's, at the layer where the user is looking). Collapsing the two into one answer
 * here would take that choice away from both.
 *
 * @param graph    The graph. Borrowed.
 * @param consumer The node whose socket this is.
 * @param socket   Which input port.
 * @param fields   The baked fields.
 * @param out      Filled with the bound field, or a default-constructed value when nothing is wired.
 * @param grid     Filled with the source node's grid when the lookup succeeds.
 *
 * @ownership   observes `graph` and `fields`, writes through `out` and `grid`
 * @thread      main
 * @pre         none
 * @post        On `ok`, `out` is readable or nothing was wired; `grid` describes the table in `out` when it is
 * @invariant   The grid always comes from the node the field was published by, never from a default
 * @errors      Reports a `PlanBuildRefusal`; `field_not_baked` when a wire names a field the store does not
 *              hold, `grid_unknown` when the source node's type this build cannot ask for a grid
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads,
 *              magnetosphere.emitter.a_ring_is_launched_at_the_pitch_angle_it_asks_for
 */
[[nodiscard]] PlanBuildRefusal resolve_field(const qp::graph::Graph& graph, qp::graph::NodeId consumer,
                                             qp::graph::PortNumber socket,
                                             const qp::graph::field::FieldSet& fields,
                                             qp::graph::field::FieldValue& out, GridSpec& grid) noexcept;

/**
 * @brief Turns the pusher nodes of `order` into prepared-on-demand steps, with their fields bound.
 *
 * @param graph The graph. Borrowed.
 * @param order The nodes to consider, in the order they should run. **This is an argument rather than something
 *              computed here**, so the builder does not have to know how the order was pruned: today a caller
 *              walks the graph's slots, and when this kit ships an output node the order will come from
 *              `graph/domain`'s `build_plan`. Either way the mapping from node to step is the same function.
 * @param fields The baked fields. Borrowed, and must outlive every executor built over `out.steps`.
 * @param out    Filled on `ok`; **cleared first**, so a failed build leaves an empty plan rather than a half one.
 *
 * @ownership   observes `graph` and `fields`, owns the kernels in `out`
 * @thread      main
 * @pre         `fields` and `graph` outlive `out` and everything built from it
 * @post        On `ok`, `out.steps.size() == out.pushers` and each step's bound slots are the fields its sockets
 *              are wired to
 * @invariant   On any refusal `out` is empty
 * @errors      Reports a `PlanBuildRefusal`; `stale_order`, `field_not_baked`, `grid_unknown`
 * @complexity  O(order + wired fields)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads,
 *              magnetosphere.plan.a_grid_it_cannot_ask_about_is_refused,
 *              magnetosphere.plan.a_stale_order_names_a_node_that_is_not_there,
 *              magnetosphere.plan.a_graph_run_gyrates_a_proton_the_way_the_field_points
 */
[[nodiscard]] PlanBuildRefusal build_particle_plan(const qp::graph::Graph& graph,
                                                   const std::vector<qp::graph::NodeId>& order,
                                                   const qp::graph::field::FieldSet& fields,
                                                   BuiltPlan& out);

}  // namespace qp::plugins::magnetosphere
