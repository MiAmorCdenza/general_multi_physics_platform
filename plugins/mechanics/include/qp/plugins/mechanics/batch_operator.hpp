/**
 * @file batch_operator.hpp
 * @brief Presents a mechanics kernel as a state operator the run loop can drive.
 *
 * ## Why an adapter rather than one interface
 *
 * `graph::kernels::IBatchAdvancer` and `graph::execution::IStateOperator` look similar and are not the
 * same contract. `IBatchAdvancer` works on a batch described as **fields** and carries a capability
 * declaration, a scratch context and an injected RNG, because its caller is a plan executor that
 * decides those. `IStateOperator` is two methods, because a run loop only needs to turn a state
 * forward and label the columns.
 *
 * Collapsing them would mean either the run loop carrying plan-execution machinery it does not use, or
 * `IBatchAdvancer` losing the declarations the host relies on. So the plugin -- which owns both the
 * kernel and this mapping -- writes the adapter, and `graph/execution` keeps its two-method interface.
 *
 * ## The ABI bridge, and it is the only interesting part
 *
 * A batch is described by a `qp::abi::LatticeDesc`, and the ABI admits component kinds of scalar (1)
 * and vector (3) only -- `is_consistent` refuses anything else. The mechanics state is exactly three
 * doubles per particle (position, velocity, omega), so it is describable as a **vector** lattice and no
 * conversion is needed: the state's buffer is what the kernel reads and writes.
 *
 * That fit is not a coincidence and it is not a guarantee. `IStateOperator` declares two methods and
 * says nothing about layout, so a different family may use a different component count -- and this
 * adapter **checks** rather than assumes, refusing a layout it cannot describe instead of building one
 * `is_consistent` would reject.
 *
 * @ownership   owns the advancer it wraps
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The wrapped advancer is prepared before the first step
 * @errors      See each declaration
 * @frozen      no
 * @tests       execution.binding.end_to_end_against_a_closed_form,
 *              execution.binding.binder_comes_from_the_plugin
 */
#pragma once

#include <qp/graph/execution/execution.hpp>

#include <qp/graph/kernels/kernel.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace qp::plugins::mechanics {

/**
 * @brief Drives one `IBatchAdvancer` over a `StateView`.
 *
 * @ownership   owns the advancer
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The advancer is never re-prepared with different parameters
 * @errors      See each declaration
 * @frozen      no
 * @tests       execution.binding.end_to_end_against_a_closed_form
 */
class BatchOperator final : public graph::execution::IStateOperator {
public:
    /// @brief The state components this adapter can describe: the ABI's vector kind.
    static constexpr std::size_t kSupportedComponents = 3;

    /**
     * @brief Wraps an advancer that has **already** been prepared.
     *
     * Preparation is the caller's step, and that is a correction rather than a preference. The first
     * version took the parameters and called `prepare` itself, returning
     * `diag::Result<std::unique_ptr<IStateOperator>>` -- which cannot be unwrapped: `Result::value()`
     * returns `const T&` by design (taking a value unchecked is a programming error, so it is not
     * handed out mutable) and `value_or` copies its fallback. Extracting a move-only payload therefore
     * has no spelling, and the compiler's complaint about a deleted copy constructor was the type
     * saying the API shape was wrong rather than that a `const_cast` was missing.
     *
     * With `prepare` outside, `make` has no failure to report and returns the operator directly.
     *
     * @ownership   owns `advancer`
     * @thread      main
     * @pre         `advancer != nullptr` and already prepared
     * @post        The operator can step a state with three components per particle
     * @invariant   The advancer is not re-prepared here, so a caller cannot change its parameters
     *              behind the operator's back
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.binding.end_to_end_against_a_closed_form
     */
    [[nodiscard]] static std::unique_ptr<graph::execution::IStateOperator> make(
        std::unique_ptr<graph::kernels::IBatchAdvancer> advancer);

    /**
     * @brief The wrapped advancer's name.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        Non-empty
     * @invariant   Constant
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.binding.end_to_end_against_a_closed_form
     */
    [[nodiscard]] std::string_view name() const noexcept override;

    /**
     * @brief What the wrapped kernel declares, mapped into the run loop's vocabulary.
     *
     * The mapping is where this adapter earns its keep a second time. `IBatchAdvancer` carries a
     * **capability** set -- in-place, scratch, stochastic, neighbourhood -- and the run loop asks a
     * different question: is the step replayable, what happens with a negative `dt`, does it respect the
     * node's units, can two of them coexist. Neither vocabulary is a subset of the other, and collapsing
     * them was rejected for the same reason collapsing the two interfaces was.
     *
     * Two of the four answers are **derived** rather than restated:
     *
     *   - `is_pure` is exactly "not stochastic". A kernel that consumes the injected RNG is a function of
     *     `(state, dt, rng)`, so replaying it needs the stream as well, and the honest declaration is that
     *     the step alone does not determine the next state.
     *   - `reversibility` is `approximate` for every scheme this build wraps. A scheme could declare
     *     `exact`, and the place for it is the kernel's own description -- the adapter must not guess
     *     "self-inverse" from a name.
     *
     * The other two are declared here, because they are properties of the mapping rather than of the
     * scheme: this adapter hands the kernel a batch aliasing the caller's buffer and accumulates a step
     * index, and neither is shared between instances.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        None of the four fields is left at "unknown"
     * @invariant   `is_pure` is false exactly when the advancer declares `is_stochastic`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.binding.end_to_end_against_a_closed_form,
     *              execution.binding.binder_comes_from_the_plugin
     */
    [[nodiscard]] graph::execution::SimModelDesc describe() const noexcept override;

    /**
     * @brief Advances the state by `dt` through the wrapped advancer.
     *
     * Builds a `BatchView` that aliases the state's own buffer -- `in` and `out` are the same
     * `FieldValue`, which the kernels accept: they read every stage into locals before writing, so an
     * aliased batch is safe and is what a host with one buffer per block does.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `state.components_per_particle == kSupportedComponents`
     * @post        On success every particle holds its state after one step
     * @invariant   Allocates nothing: the `FieldValue` is a local and the buffer is borrowed
     * @errors      `type_mismatch` for a layout the ABI cannot describe; otherwise the advancer's own
     *              code
     * @complexity  O(count)
     * @nondet      none
     * @frozen      no
     * @tests       execution.binding.end_to_end_against_a_closed_form
     */
    [[nodiscard]] diag::Result<void> step(graph::execution::StateView& state, double dt) override;

private:
    BatchOperator(std::unique_ptr<graph::kernels::IBatchAdvancer> advancer,
                  std::string name) noexcept;

    std::unique_ptr<graph::kernels::IBatchAdvancer> advancer_;
    std::string name_;
    std::uint64_t step_index_ = 0;
};

}  // namespace qp::plugins::mechanics
