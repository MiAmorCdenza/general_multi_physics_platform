/**
 * @file mechanics_binder.hpp
 * @brief Turns a mechanics node into the operator that drives it.
 *
 * ## Why this lives in the plugin
 *
 * `graph::execution::IOperatorBinder` says "build me an operator for this node"; what that means for a
 * specific node type is knowledge about that node's parameters, and it changes when the node changes.
 * Putting it in the execution engine would mean a new node type required editing the engine -- the
 * coupling the plugin split exists to prevent. So the binding ships with the operator it configures.
 *
 * ## What it reads, and what it refuses
 *
 * For `demo.spring_damper` the node declares `k` (stiffness, N/m), `c` (damping, N*s/m), `m` (mass,
 * kg), and `integrator` (a choice). The operator this project ships integrates `x'' = -omega^2 x`, so
 * the binder **derives** the frequency:
 *
 *     omega = sqrt(k / m)
 *
 * and refuses the node when `c` is non-zero, because the kernel has no damping term. That refusal is
 * the point rather than a limitation to work around: a graph that says `c = 0.5` and a run that
 * silently integrates `c = 0` produces a decaying-looking result with the wrong decay, and the user has
 * no way to tell. Reporting `not_implemented` lets the window say "this node asks for damping that no
 * operator provides yet", which is a thing the user can act on.
 *
 * ## Why only `rk4` is accepted
 *
 * The node's choice lists `euler`, `rk4` and `verlet`. Only `rk4` has an implementation, and it is the
 * only one with a bit-exact golden case behind it. Accepting `verlet` and running RK4 would be the same
 * silent substitution as above, one level up: the user picks a symplectic integrator precisely to get
 * different behaviour, and handing them a dissipative one contradicts the choice.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Never modifies the node
 * @errors      See the interface
 * @frozen      no
 * @tests       execution.binding.binder_comes_from_the_plugin,
 *              execution.binding.damping_is_refused,
 *              execution.binding.other_integrators_are_refused,
 *              execution.binding.missing_parameter_is_refused
 */
#pragma once

#include <qp/graph/execution/execution.hpp>

#include <memory>
#include <string_view>

namespace qp::plugins::mechanics {

/**
 * @brief Binds the mechanics nodes to the mechanics kernels.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Stateless: the same node always binds to an equivalent operator
 * @errors      See `bind`
 * @frozen      no
 * @tests       execution.binding.binder_comes_from_the_plugin,
 *              execution.binding.damping_is_refused,
 *              execution.binding.other_integrators_are_refused
 */
class MechanicsBinder final : public graph::execution::IOperatorBinder {
public:
    /// @brief The node type this binder handles.
    static constexpr const char* kSpringDamperType = "demo.spring_damper";
    /// @brief State components this binder's operators require: position, velocity, omega.
    static constexpr std::size_t kComponents = 3;

    MechanicsBinder() = default;

    /**
     * @brief Whether `type_name` is the spring-damper type, with a layout this family can use.
     *
     * Answers for the **type**, not for an instance: a spring-damper asking for damping is still a type
     * this binder owns, and saying otherwise would report it as an unknown node rather than as one whose
     * two settings need changing.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        True exactly for `kSpringDamperType` with three components per particle
     * @invariant   Independent of any node instance
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.binding.damping_is_refused,
     *              execution.binding.other_integrators_are_refused
     */
    [[nodiscard]] bool can_bind(std::string_view type_name,
                               const graph::execution::StateView& layout) const noexcept override;

    /**
     * @brief Builds an RK4 oscillator for a spring-damper node, or declines.
     *
     * Returns null -- "not mine" -- when the type is not a spring-damper, when the state layout is not
     * the three-component one this family uses, or when a parameter the operator needs is missing.
     * Those are all ordinary answers from a binder that was asked about something else.
     *
     * There is no error channel on this interface, so a node this binder recognises but cannot honour
     * returns null and the run reports `not_implemented` naming the type. See the file comment for why
     * damping and the other integrators are in that category rather than being approximated.
     *
     * @ownership   owns the returned operator
     * @thread      main
     * @pre         none
     * @post        Null, or an operator that accepts the given layout
     * @invariant   Does not modify `node`
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(parameters)
     * @nondet      none
     * @frozen      no
     * @tests       execution.binding.binder_comes_from_the_plugin,
     *              execution.binding.missing_parameter_is_refused
     */
    [[nodiscard]] std::unique_ptr<graph::execution::IStateOperator> bind(
        std::string_view type_name, const qp::graph::Node& node,
        const graph::execution::StateView& layout) override;
};

}  // namespace qp::plugins::mechanics
