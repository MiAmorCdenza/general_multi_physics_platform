/**
 * @file models_binder.hpp
 * @brief Turns a model node into the operator that integrates it, and declares the node types this build ships.
 *
 * ## The two jobs, and why they are one file
 *
 * A binder and a node description are two halves of one fact: what parameters a type has, and what those
 * parameters mean. Splitting them would put the port numbers in one file and the parameter reads in another, and
 * the failure mode of that split is the one this repository keeps finding -- a declaration that nothing consumes,
 * or a reader that consumes a number the description never declared. So `node_types()` and `bind()` sit side by
 * side, and a change to one is a change to the other in the same place.
 *
 * ## What every model node has in common
 *
 * Three parameters and one output, in the same port numbers, because they answer the same three questions:
 *
 * | port | name | meaning |
 * |---|---|---|
 * | 1 | `initial` | the starting value of the first state component |
 * | 2 | `rate` | the starting value of the second |
 * | 3 | `parameter` | whichever physical parameter that model has one of |
 * | 1 (out) | `state` | the quantity the run traces |
 *
 * The port **numbers** are shared on purpose: the canvas draws ports by number, the property panel builds an
 * editor per declared port, and a family whose members disagree about which port is which is a family whose
 * palette entries cannot be laid out the same way. What differs between the models is the fourth onward -- the
 * oscillator's decay rate, the pendulum's length, the projectile's drag -- and those live at ports 4 and up.
 *
 * ## Why the binder declines a layout rather than adapting
 *
 * `can_bind` is given the state layout the run will use, and each model here accepts exactly one
 * `components_per_particle`. A binder that adapted -- padding a two-component pendulum into a three-component
 * state -- would silently read a component nothing writes, and the first symptom would be a trajectory that is
 * subtly the wrong shape. Declining means the run reports that no operator can honour the node, which names the
 * actual problem.
 *
 * ## Why every member's contract is a block comment and not a run of line comments
 *
 * The contract gate finds a contract by matching a `slash-star-star` block and then looking at the line the block
 * ends against. A run of `slash-slash-slash` lines is invisible to it, and the consequence is not a missing check
 * but a **misattributed** one: the tags land on the file's module-level contract instead, so the test they name is
 * claimed by something that has nothing to do with it and the function they describe is reported as having no
 * tests at all. That is what happened to `mount` here, and the failure looks like an orphan in a plugin that had
 * not changed -- which is the worst shape a gate failure can take, because the message names the wrong file.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Never modifies the node; stateless, so the same node always binds the same way
 * @errors      See the interface
 * @frozen      no
 * @tests       models.the_binder_declares_the_types_it_binds
 */
#pragma once

#include <qp/graph/execution/execution.hpp>
#include <qp/graph/ir/descriptor.hpp>
#include <qp/host/host.hpp>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace qp::plugins::models {

/**
 * @brief Binds the three model types to the three operators in `models.hpp`.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `can_bind` answers for the type and the layout, never for an instance
 * @errors      See `bind`
 * @frozen      no
 * @tests       models.the_binder_declares_the_types_it_binds,
 *              models.a_node_binds_to_the_model_its_parameters_describe
 */
class ModelsBinder final : public graph::execution::IOperatorBinder {
public:
    /// @brief The damped harmonic oscillator: `x'' = -w^2 x - g x'`.
    static constexpr const char* kOscillatorType = "model.damped_oscillator";
    /// @brief The pendulum: `th'' = -(g/L) sin(th)`.
    static constexpr const char* kPendulumType = "model.pendulum";
    /// @brief The projectile: gravity with linear drag, stopped at the ground.
    static constexpr const char* kProjectileType = "model.projectile";
    /// @brief The driven oscillator: `x'' = -w0^2 x - gamma x' + F cos(wd t)`.
    static constexpr const char* kDrivenType = "model.driven_oscillator";

    /// @brief Gravity used where a model's port 3 is `g`, in m/s^2.
    ///
    /// At port 3 rather than an argument with a default, because a constant the platform chose for the user is a
    /// number their report will quote unless they change it, and a value they can see on the node is one they can
    /// check against the apparatus. The number is the standard gravity, to the digits a teaching lab uses.
    static constexpr double kStandardGravity = 9.80665;

    ModelsBinder() = default;

    /**
     * @brief Whether `type_name` is a model this binder owns, at `layout`.
     *
     * @param type_name The node type.
     * @param layout    The state layout the run will use.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        True exactly for one of the three types, at exactly that model's component count
     * @invariant   Independent of any node instance
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       models.the_binder_declares_the_types_it_binds
     */
    [[nodiscard]] bool can_bind(std::string_view type_name,
                                const graph::execution::StateView& layout) const noexcept override;

    /**
     * @brief Builds the operator for a node, or null when the node is not one this binder can honour.
     *
     * @param type_name The node type.
     * @param node      The instance, for reading parameters.
     * @param layout    The state layout the operator will be handed.
     *
     * @ownership   owns the returned operator
     * @thread      main
     * @pre         none
     * @post        Null, or an operator whose `describe()` matches the model the parameters name
     * @invariant   A parameter that is missing or not a number takes the description's own default
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(parameters)
     * @nondet      none
     * @frozen      no
     * @tests       models.a_node_binds_to_the_model_its_parameters_describe
     */
    [[nodiscard]] std::unique_ptr<graph::execution::IStateOperator> bind(
        std::string_view type_name, const graph::Node& node,
        const graph::execution::StateView& layout) override;

    /**
     * @brief The node types this build ships, ready to register with a host.
     *
     * A static function rather than a member's data because the descriptions are constants of the **build**, not
     * of a binder instance: a caller that wanted the palette entries should not have to construct a binder to get
     * them.
     *
     * @ownership   owns the returned descriptions
     * @thread      main
     * @pre         none
     * @post        One description per model, with the port numbers `bind` reads
     * @invariant   Every `type_name` here is one `can_bind` answers true for at that model's layout
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       models.the_binder_declares_the_types_it_binds
     */
    [[nodiscard]] static std::vector<graph::NodeDesc> node_types();

    /**
     * @brief Registers the three types with `host` as built-ins.
     *
     * Through `add_builtin_node_type` and not into the registry directly, for the reason every other built-in
     * takes that path: a contribution that bypassed the ledger would be the one thing `origin_of` could not
     * answer for and the one thing a session could not take back.
     *
     * Returns how many were registered rather than a code, because a partial result is meaningful -- a type
     * whose name was taken is one palette entry missing, not a broken build.
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
     * @tests       models.the_shipped_models_register_with_the_host
     */
    static std::size_t mount(qp::host::PluginHost& host) noexcept;

private:
    /// @brief The number of the first port that is a model-specific physical parameter.
    ///
    /// `4`, because 1 and 2 are the initial state and 3 is the one parameter every model has. Named because
    /// `bind` and `node_types()` must agree about it and a literal in two places is two chances to disagree --
    /// which is exactly the failure this file's own comment warns about.
    static constexpr qp::graph::PortNumber kFirstPhysicalPort = 4;
};

}  // namespace qp::plugins::models
