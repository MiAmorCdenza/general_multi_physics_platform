/**
 * @file run_controller.hpp
 * @brief The window's Run action: find the node, run it, put the numbers where they belong.
 *
 * ## Where this sits
 *
 * `core/graph/execution` owns the loop and `plugins/mechanics` owns the binding; neither knows about a
 * session, a ledger or a panel. This is the piece that does, and it belongs in the view layer precisely
 * because it is the only place all of them meet.
 *
 * ## What one Run does, in order
 *
 *   1. Finds the node to run in the session's graph. The first node whose type any binder claims --
 *      not "the selected node", because a run should not depend on a selection the user may have
 *      changed for unrelated reasons.
 *   2. Binds an operator to it. Binding comes **before** anything is written down, so a node no
 *      operator can honour leaves no trace in the ledger: a gap that is recorded as an event is not a
 *      gap, and the record's value is that a missing entry means something happened.
 *   3. Opens a run in **its own ledger** and hands the id to the loop, so the trace it returns carries
 *      an identity the ledger issued. A trace whose `RunId` nobody issued is a record that cannot be
 *      looked up, which defeats the point of recording it. The window hands that same id to the
 *      measurement session, so the panel's trace and this ledger agree about which run the numbers
 *      belong to.
 *   4. Seeds the state, steps, and returns the trace.
 *   5. Reports honestly: no node to run, a node no binder claims, damping the kernel cannot integrate.
 *
 * ## Why the steps are fixed rather than a parameter at a time
 *
 * A run's step count and step size decide whether the answer is right, and a control that let them be
 * anything would let a user produce a diverged trajectory and believe it. They are constants, reported
 * through `description()` so the UI can state what it will do before doing it. Making them editable is
 * a real feature, and it needs the confidence panel's drift figure visible **while** choosing.
 *
 * @ownership   observes
 * @thread      ui
 * @pre         The session outlives this object
 * @post        none
 * @invariant   Never mutates the graph
 * @errors      See each declaration
 * @frozen      no
 * @tests       run.controller.refuses_a_graph_with_nothing_to_run,
 *              run.controller.damping_is_reported_not_hidden
 */
#pragma once

#include <qp/graph/execution/execution.hpp>

#include <qp/authoring/commands/session.hpp>
#include <qp/runtime/run/run.hpp>
#include <qp/runtime/trace/trace.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace qp::views::model {

/**
 * @brief What one Run attempt produced.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   `message` is empty exactly when `ok` is true
 * @errors      noexcept
 * @frozen      no
 * @tests       run.controller.refuses_a_graph_with_nothing_to_run
 */
struct RunReport final {
    /// Whether a trace was produced.
    bool ok = false;
    /// Steps actually taken.
    std::size_t steps = 0;
    /// Samples recorded, which is `steps + 1` on success.
    std::size_t samples = 0;
    /// The operator that ran.
    std::string operator_name{};
    /// The node type that was run, for the record.
    std::string node_type{};
    /// The run identity the trace carries, so a caller can look it up in the ledger.
    qp::runtime::RunId run{};
    /// What to tell the user. A sentence for the status line, not an error code: the codes are already
    /// in the log, and a student reading the window needs to know what to change.
    std::string message{};
};

/**
 * @brief A run's outcome together with the trace it produced.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   `trace` holds samples exactly when `report.ok`
 * @errors      noexcept
 * @frozen      no
 * @tests       run.controller.refuses_a_graph_with_nothing_to_run
 */
struct RunResult final {
    RunReport report{};
    qp::runtime::Trace trace{qp::runtime::RunId{}};
};

/**
 * @brief Runs a graph through the binders and into a trace.
 *
 * @ownership   observes
 * @thread      ui
 * @pre         `session` outlives this object
 * @post        none
 * @invariant   The session's graph is never modified
 * @errors      See each declaration
 * @frozen      no
 * @tests       run.controller.refuses_a_graph_with_nothing_to_run
 */
class RunController final {
public:
    /// @brief Steps per run.
    ///
    /// 4096 with `kDt` covers a few periods of a typical lab oscillator at a step small enough that
    /// RK4's dissipation stays under the confidence panel's note threshold -- so a clean run reports no
    /// drift note, and a drift note means something is genuinely wrong.
    static constexpr std::size_t kSteps = 4096;
    /// @brief Step size in seconds.
    static constexpr double kDt = 1.0e-4;
    /// @brief Initial displacement of the first particle, in metres.
    ///
    /// One centimetre: a lab-scale amplitude, and one that keeps the trajectory in a range where RK4's
    /// error stays under the confidence panel's note threshold. A run at this amplitude that still shows
    /// drift is telling the user something real.
    static constexpr double kInitialDisplacement = 0.01;
    /// @brief The angular frequency the **frequency component** of the state carries.
    ///
    /// A separate value from whatever the binder derived, and that is deliberate rather than an
    /// oversight: the binder computes `sqrt(k/m)` for the *kernel's* parameter block, while this fills
    /// the per-particle `omega` slot that a state operator's layout carries. They agree for a
    /// spring-damper, and the layout value is what a different operator family would read. A run whose
    /// two disagree is a real defect, and it shows up as a trace that does not match its own node -- so
    /// the values are named here where the disagreement would be visible in one place.
    static constexpr double kOmega = 20.0;

    /**
     * @brief A controller over one session and one binder list.
     *
     * @param session The editing session, read-only. Borrowed, not copied: a controller over a copy of
     *                the graph would run something the user is not looking at.
     * @param binders Consulted in order; the first that claims a node runs it.
     *
     * @ownership   observes `session`
     * @thread      ui
     * @pre         `session` outlives this object
     * @post        none
     * @invariant   The borrowed reference is never written through
     * @errors      noexcept
     * @complexity  O(binders)
     * @nondet      none
     * @frozen      no
     * @tests       run.controller.refuses_a_graph_with_nothing_to_run
     */
    RunController(const qp::authoring::Session& session,
                  std::vector<graph::execution::IOperatorBinder*> binders);

    /**
     * @brief The sentence a UI can show before the user presses Run.
     *
     * States the step count and step size, because a run's resolution is part of what it claims and a
     * button that does not say what it will do is a button that surprises.
     *
     * @ownership   owns the result
     * @thread      ui
     * @pre         none
     * @post        Non-empty
     * @invariant   Describes the values this controller actually uses
     * @errors      May allocate; allocation failure terminates
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       run.controller.description_states_what_it_will_do
     */
    [[nodiscard]] std::string description() const;

    /**
     * @brief The ledger of runs this controller has started.
     *
     * The ids in `RunReport::run` are issued by it, so a caller that wants to show a run's provenance
     * reads it here. One ledger per controller rather than per caller, because two ledgers in one
     * session is how the window's status line and a panel came to disagree about how many runs had
     * happened.
     *
     * @ownership   borrows from this object
     * @thread      ui
     * @pre         none
     * @post        none
     * @invariant   Every reported `run` is a valid id in this ledger
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       run.controller.a_second_run_is_a_second_entry,
     *              run.controller.runs_a_node_and_records_its_trace
     */
    [[nodiscard]] const qp::runtime::RunLedger& ledger() const noexcept { return ledger_; }

    /**
     * @brief Binds the first runnable node, runs it, and returns its trace.
     *
     * The trace is returned **by value** rather than written into the session: a run does not edit the
     * graph, and a trace is its output rather than part of the document. The caller decides what to do
     * with it, which is what lets the window hand it to the measurement session and a test inspect it
     * without either owning the other.
     *
     * @ownership   owns the result
     * @thread      ui
     * @pre         none
     * @post        On success the returned trace holds `steps + 1` samples and a valid run id
     * @invariant   The graph is unchanged
     * @errors      Never throws; every failure is a sentence in the report
     * @complexity  O(steps)
     * @nondet      none
     * @frozen      no
     * @tests       run.controller.refuses_a_graph_with_nothing_to_run,
     *              run.controller.damping_is_reported_not_hidden,
     *              run.controller.runs_a_node_and_records_its_trace,
     *              run.controller.a_second_run_is_a_second_entry
     */
    [[nodiscard]] RunResult run() const;

private:
    /// @brief The spec a run is opened with, so every run records the same inputs.
    [[nodiscard]] qp::runtime::RunSpec spec_for() const;

    const qp::authoring::Session* session_;
    std::vector<graph::execution::IOperatorBinder*> binders_;
    /// Mutable because `run()` is const: the controller does not change what it runs, but starting a run
    /// is an event the ledger records. Marking the method non-const would say the graph might change,
    /// which is the property worth keeping.
    mutable qp::runtime::RunLedger ledger_{};
};

}  // namespace qp::views::model
