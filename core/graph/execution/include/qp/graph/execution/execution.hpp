/**
 * @file execution.hpp
 * @brief Driving a graph: one state buffer, one operator, one recorded trace.
 *
 * ## Why the run loop is not a node
 *
 * The obvious design is a "simulate" node that steps the state. It cannot work, and the reason is
 * already written into the evaluator's own contract: `INodeEvaluator::evaluate` must be a **pure
 * function of its inputs**, because the content-addressed cache keys on `(node, inputs, parameters)`
 * and a node that depends on hidden state "will hit the wrong cache entry". A time-stepping operator
 * is the definition of hidden state.
 *
 * So the graph does what it is good at -- a declarative description whose parameters the evaluator
 * can compute -- and something **above** it does the stepping. That is this file. The graph says
 * "a mass of 0.5 kg on a spring of 12 N/m, integrated with rk4"; `GraphRun` instantiates that
 * operator, walks it, and records what came out.
 *
 * ## What is foundation here and what is content
 *
 * The platform owns exactly two things and neither is physics:
 *
 *   - **`IStateOperator`** -- the shape of "something that advances a state and can say what it
 *     integrates". The stepping loop needs nothing else from an operator, and adding to this
 *     interface is how a run loop becomes coupled to one family of integrators.
 *   - **`StateView`** -- state as an owned, testable buffer with a declared layout.
 *
 * The **mapping** from a node's parameters to an operator's parameter block is content: it belongs
 * to whichever plugin ships the operator, registered through `OperatorBinder`. The alternative --
 * the bridge reading `k`, `c` and `m` by name -- would mean a new node type required a change to the
 * execution engine, which is the coupling the whole plugin split exists to avoid.
 *
 * ## State layout, and why the frequency is part of it
 *
 * One particle is `[position, velocity, omega]`. The first two are the mechanical state; `omega` is
 * there because the operators this project ships read it from the per-particle slot rather than from
 * a global parameter, so a run that recorded only two components would not be able to describe the
 * system it was integrating. It is written once at the start and carried, not integrated.
 *
 * `components_per_particle` is declared rather than assumed: three for the mechanics family, and a
 * different family is free to declare something else.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No physics in this file: the bridge never computes an acceleration
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       execution.loop.records_the_initial_condition,
 *              execution.loop.second_run_replaces_the_trace,
 *              execution.loop.rejects_unusable_arguments,
 *              execution.loop.failure_keeps_earlier_samples,
 *              execution.loop.state_view_is_total
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/ir.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/runtime/store/store.hpp>
#include <qp/runtime/trace/trace.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qp::graph::execution {

/**
 * @brief One particle's state, and the layout that describes it.
 *
 * Owned `double`s rather than a `qp::abi::LatticeDesc` over borrowed memory. The reason is not
 * convenience: the ABI's component kinds are scalar (1) and vector (3) only, and `is_consistent`
 * refuses anything else, so a bridge that built its own descriptor for a different number of
 * components would be building one the platform rejects. Owning the buffer keeps this layer free of
 * a layout contract it does not need, and the operators that read it through the ABI are the ones
 * that already know how.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `values.size() == count * components_per_particle`
 * @errors      noexcept
 * @frozen      no
 * @tests       execution.loop.state_view_is_total
 */
struct StateView final {
    /// Number of particles.
    std::size_t count = 0;
    /// Doubles per particle. Three for the mechanics family: position, velocity, omega.
    std::size_t components_per_particle = 3;
    /// The state, particle-major: particle `i` occupies `[i*c, (i+1)*c)`.
    std::vector<double> values{};

    /// @brief Allocates a zeroed state for `n` particles.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         `c > 0`
    /// @post        `count == n` and every component is `0.0`
    /// @invariant   The size relation holds
    /// @errors      May allocate; allocation failure terminates
    /// @complexity  O(n*c)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] static StateView zeroed(std::size_t n, std::size_t c = 3);

    /// @brief Component `component` of particle `i`, or 0.0 when out of range.
    ///
    /// Total rather than asserting: a caller that asks for a component this layout does not have is
    /// a caller with a layout mismatch, and the useful answer is a zero that flows through rather
    /// than a termination in a run loop.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Never reads outside `values`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] double at(std::size_t i, std::size_t component) const noexcept;

    /// @brief Writes component `component` of particle `i`, ignoring an out-of-range request.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        The addressed component holds `v`, or nothing changed
    /// @invariant   Never writes outside `values`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    void set(std::size_t i, std::size_t component, double v) noexcept;

    /// @brief Whether the size relation holds. Checked before a step rather than assumed.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Equivalent to the invariant above
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] bool is_consistent() const noexcept;
};

/**
 * @brief Something that advances a state and can name what it integrates.
 *
 * The interface is deliberately two methods. A run loop needs to instantiate an operator, step it,
 * and label the columns it produced; anything more would be this layer deciding how an operator
 * works internally, and every addition here narrows which families can be run.
 *
 * @ownership   observes (an operator is owned by whoever provides it)
 * @thread      main (prepare, step)
 * @pre         none
 * @post        none
 * @invariant   `name` is stable for the object's lifetime
 * @errors      `step` returns a Result and never throws
 * @frozen      no
 * @tests       execution.loop.records_the_initial_condition
 */
class IStateOperator {
public:
    IStateOperator() = default;
    virtual ~IStateOperator() = default;
    IStateOperator(const IStateOperator&) = delete;
    IStateOperator& operator=(const IStateOperator&) = delete;

    /// @brief Stable name, used as the trace's provenance and in diagnostics.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        A non-empty name for the object's lifetime
    /// @invariant   Constant
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// @brief Advances every particle by `dt`, in place.
    ///
    /// @ownership   mutates `state`
    /// @thread      main
    /// @pre         `state.is_consistent()`
    /// @post        On success every particle holds its state after one step
    /// @invariant   No allocation, no throw, and no dependence on anything but the state and `dt`
    /// @errors      Returns an error code rather than throwing
    /// @complexity  O(count)
    /// @nondet      none -- an operator that needs randomness must take it from the run's seed
    /// @frozen      no
    [[nodiscard]] virtual diag::Result<void> step(StateView& state, double dt) = 0;
};

/**
 * @brief Turns one node instance into the operator that should drive it.
 *
 * Implementations live in plugins, next to the operator they configure, because the mapping is
 * **content**: it knows which of a node's parameters mean what, and that knowledge changes when a
 * node type changes. Keeping it here would mean a new node type required editing the execution
 * engine.
 *
 * A binder returns `nullptr` rather than an error when it does not recognise the node. Several
 * binders are consulted in turn, and "not mine" is not a failure -- it is the normal answer from all
 * but one of them.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A binder that returns an operator returns one whose `step` accepts the same state
 *              shape it was asked about
 * @errors      May allocate; allocation failure terminates
 * @frozen      no
 * @tests       execution.loop.rejects_unusable_arguments
 */
class IOperatorBinder {
public:
    IOperatorBinder() = default;
    virtual ~IOperatorBinder() = default;
    IOperatorBinder(const IOperatorBinder&) = delete;
    IOperatorBinder& operator=(const IOperatorBinder&) = delete;

    /**
     * @brief Whether this binder handles `type_name` at all, with `layout`.
     *
     * Separate from `bind` because the two questions have different answers, and the difference is what
     * the user reads. A search for something to run needs to know **whether a type is known**, which does
     * not depend on the instance's parameters; whether a particular instance can be honoured is the next
     * question. Collapsing them made a graph holding a spring-damper with damping report "no node in this
     * graph has an operator yet" -- which sends the reader looking for a missing plugin instead of at the
     * two numbers on the node in front of them.
     *
     * Examined with the layout the run will use, so a binder that cannot describe the state shape says so
     * here rather than being counted as recognising a type it could never run.
     *
     * @param type_name The node type, e.g. "demo.spring_damper".
     * @param layout    The state layout the operator would be handed.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The answer depends only on `type_name` and `layout`, never on a node instance
     * @invariant   A binder answering true here answers non-null from `bind` for at least one
     *              parameterisation of that type
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.loop.can_bind_answers_for_the_type_not_the_instance
     */
    [[nodiscard]] virtual bool can_bind(std::string_view type_name,
                                       const StateView& layout) const noexcept = 0;

    /// @brief Builds the operator for `node`, or null when this binder cannot honour it.
    ///
    /// A null here means "this type is mine, but not what this instance asks for" -- damping the kernel
    /// has no term for, an integrator with no implementation, a parameter nobody has filled in. Callers
    /// should have asked `can_bind` first; this is the second question, not the first.
    ///
    ///
    /// @param type_name The node instance's type, e.g. "demo.spring_damper".
    /// @param node      The instance, for reading parameters. Const: a binder must not edit the graph.
    /// @param layout    The state layout the operator will be handed, so a binder that cannot work
    ///                  with it can decline instead of reading the wrong component.
    ///
    /// @ownership   owns the returned operator
    /// @thread      main
    /// @pre         none
    /// @post        Returns null or an operator usable with `layout`
    /// @invariant   Same node and layout produce an equivalent operator
    /// @errors      May allocate; allocation failure terminates
    /// @complexity  O(parameters)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] virtual std::unique_ptr<IStateOperator> bind(
        std::string_view type_name, const Node& node, const StateView& layout) = 0;
};

/**
 * @brief What one run produced, and everything needed to say what it did.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `trace.size()` equals the number of steps requested plus one, on success
 * @errors      noexcept
 * @frozen      no
 * @tests       execution.loop.records_the_initial_condition
 */
struct RunOutcome final {
    /// Whether the run completed every step.
    bool completed = false;
    /// Number of steps actually taken.
    std::size_t steps = 0;
    /// The operator that ran, for the record. Empty when no binder claimed the node.
    std::string operator_name{};
    /// The failure, when `completed` is false.
    diag::ErrorCode error = diag::ErrorCode::ok;

    /// @brief Whether the run finished every step it was asked for.
    [[nodiscard]] bool ok() const noexcept { return completed && error == diag::ErrorCode::ok; }
};

/**
 * @brief A run loop: one operator, stepped, recorded into a trace.
 *
 * Holds no graph and no session. It is handed a node, a binder list, and a trace to fill, so it can
 * be driven from a test with a hand-built node and from the window with a session's.
 *
 * @ownership   owns its trace and its operator
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The trace's channels are declared before the first sample is appended
 * @errors      See each declaration
 * @frozen      no
 * @tests       execution.loop.records_the_initial_condition,
 *              execution.loop.second_run_replaces_the_trace,
 *              execution.loop.run_can_be_named_after_binding
 */
class GraphRun final {
public:
    /// @brief The channel name carrying position.
    static constexpr const char* kPositionChannel = "displacement";
    /// @brief The channel name carrying velocity.
    static constexpr const char* kVelocityChannel = "velocity";

    /**
     * @brief Builds an operator for `node` from the first binder that claims it.
     *
     * @param node    The node instance. Only its type and parameters are read.
     * @param binders The binders to consult, in order. Later ones are tried when earlier ones
     *                decline, so a general binder may precede a specific one.
     * @param run     The run identity the trace belongs to, or a **default-constructed** id meaning
     *                "not known yet". A valid id files the trace under that run; an unknown one leaves
     *                the trace unnamed until `set_run` supplies an identity.
     *
     *                Accepting an unknown identity is a correction rather than a convenience. Opening a
     *                run in the ledger before binding means a refused bind leaves an entry for a run
     *                that never executed, and a ledger full of those is unreadable -- exactly what its
     *                own gap report exists to avoid. A caller that wants to bind first therefore needs
     *                `prepare` to tolerate an unknown identity, and nothing is lost: `set_run` creates
     *                the trace, so it cannot carry an id nobody issued.
     * @param layout  The state layout to use. Defaults to the mechanics family's three components.
     *
     * @ownership   owns the operator
     * @thread      main
     * @pre         none
     * @post        On success `operator_name()` is non-empty and `is_ready()` is true; the trace is
     *              fresh, its channels are declared, and a valid `run` names it
     * @invariant   No binder is consulted twice
     * @errors      `not_implemented` when no binder claims the node, so the caller can say which
     *              type it was rather than reporting a generic failure
     * @complexity  O(binders * parameters)
     * @nondet      none
     * @frozen      no
     * @tests       execution.loop.rejects_unusable_arguments,
     *              execution.loop.run_can_be_named_after_binding
     */
    [[nodiscard]] diag::Result<void> prepare(
        const Node& node, const std::vector<IOperatorBinder*>& binders,
        qp::runtime::RunId run, std::size_t particles = 1);

    /**
     * @brief Names the run this loop's trace belongs to, replacing any previous trace.
     *
     * Called after `prepare` by a caller that wanted to know whether anything would run before opening a
     * ledger entry. It clears the recorded samples, because a trace and a run identity are one record:
     * keeping samples from an unnamed trace and then labelling them would attribute numbers to a run that
     * did not produce them.
     *
     * @param run A valid run identity. An invalid one is ignored, so a caller cannot erase a good run's
     *            trace by passing a default-constructed id.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        For a valid `run`, `trace().run() == run` and the channels are declared
     * @invariant   A caller can append samples immediately afterwards
     * @errors      May allocate; the channel set is fixed, so a channel failure would be a defect and is
     *              asserted rather than returned
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.loop.rejects_unusable_arguments,
     *              execution.loop.run_can_be_named_after_binding
     */
    void set_run(qp::runtime::RunId run);

    /// @brief Sets the initial state of one particle.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         `is_ready()`
    /// @post        The particle's position and velocity hold the given values
    /// @invariant   Does not change `count` or the layout
    /// @errors      `out_of_range` for a particle index past the end
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] diag::Result<void> set_initial(std::size_t particle, double position,
                                                 double velocity);

    /// @brief Sets the frequency component of every particle.
    ///
    /// One call rather than per particle because the frequency is a property of the system, not of a
    /// particle: the layout carries it per particle only because the operators read it from there.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         `is_ready()`
    /// @post        Every particle's third component holds `omega`
    /// @invariant   The state stays consistent
    /// @errors      noexcept
    /// @complexity  O(count)
    /// @nondet      none
    /// @frozen      no
    void set_omega(double omega) noexcept;

    /**
     * @brief Steps `steps` times, appending one sample per step.
     *
     * The first sample is taken **before** the first step, so a trace of `n` steps holds `n+1`
     * samples and the initial condition is in the record. A trace that began after the first step
     * would make the initial state unrecoverable, and the confidence panel's drift is measured
     * against the initial energy.
     *
     * @param steps Number of steps. Zero is legal and records the initial condition alone.
     * @param dt    Step size in seconds. Finite and positive.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `is_ready()` and `dt > 0`
     * @post        On success `trace().size() == steps + 1`
     * @invariant   The trace's samples are in non-decreasing time order
     * @errors      Returns an outcome carrying the code; a failed step stops the run and keeps the
     *              samples taken so far rather than discarding them, because a run that diverged at
     *              step 900 is more informative than an empty trace
     * @tests       execution.loop.records_the_initial_condition,
     *              execution.loop.second_run_replaces_the_trace,
     *              execution.loop.rejects_unusable_arguments,
     *              execution.loop.failure_keeps_earlier_samples
     * @complexity  O(steps * count)
     * @nondet      none
     * @frozen      no
 */
    [[nodiscard]] RunOutcome run(std::size_t steps, double dt);

    /// @brief Whether `prepare` succeeded.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   True exactly when an operator was bound
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] bool is_ready() const noexcept { return operator_ != nullptr; }

    /// @brief The bound operator's name, or empty when unprepared.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Non-empty exactly when `is_ready()`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] const std::string& operator_name() const noexcept { return operator_name_; }

    /// @brief The state after the run.
    ///
    /// @ownership   borrows
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   The same buffer every step mutated
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] const StateView& state() const noexcept { return state_; }

    /// @brief The trace the run recorded into.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        none
    /// @invariant   Channels are declared before the first sample
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    [[nodiscard]] const qp::runtime::Trace& trace() const noexcept { return trace_; }

private:
    /**
     * @brief Declares the run's channels on the trace.
     *
     * Called from `prepare` and `set_run` alone, because the channels name quantities that only exist once
     * an operator is bound: a loop with no operator has no position to record. There is no "declared"
     * flag: the trace's own channel count is the readable answer, and a second copy of it would be a
     * second thing to keep in step.
     *
     * @ownership   owns
     * @thread      main
     * @pre         `trace_` carries the run identity the samples will belong to
     * @post        On success the trace holds exactly the position and velocity channels, in that order
     * @invariant   A caller can append a two-value sample immediately afterwards
     * @errors      `already_exists` propagated from `Trace::add_channel` if the channel is duplicated
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.loop.records_the_initial_condition,
     *              execution.loop.run_can_be_named_after_binding
     */
    [[nodiscard]] diag::Result<void> declare_channels();

    StateView state_{};
    std::unique_ptr<IStateOperator> operator_{};
    std::string operator_name_{};
    qp::runtime::Trace trace_{qp::runtime::RunId{}};
};

}  // namespace qp::graph::execution
