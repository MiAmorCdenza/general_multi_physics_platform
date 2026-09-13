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
 *              run.controller.damping_is_reported_not_hidden,
 *              run.controller.a_provider_run_records_under_the_ledgers_identity,
 *              views.binders.a_provider_runs_a_graph_the_operators_declined
 */
#pragma once

#include <qp/graph/execution/execution.hpp>
#include <qp/graph/domain/declaration.hpp>
#include <qp/graph/field/field_set.hpp>

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
    /// The node that was run.
    ///
    /// Carried out of a run because a trace and a reading both need to name where they came from, and the
    /// only place that knows is here: `check_run` picks the node and the binder runs it. A caller that
    /// re-derived the choice -- by searching the graph again for "the first node a binder claims" -- would
    /// be running a second selection rule, and the two would disagree the moment the graph changed between
    /// the run and the report. A default-constructed id means no node was chosen, which is the case for
    /// every refused run.
    qp::graph::NodeId node{};
    /// The run identity the trace carries, so a caller can look it up in the ledger.
    qp::runtime::RunId run{};
    /// How many **errors** `graph/validate` found before the run started. Reported rather than enforced: the
    /// kernels do not check dimensions, so a graph with a mismatch still runs and produces numbers that look
    /// fine -- which is exactly why the count is carried out to a status line instead of staying inside the
    /// pre-flight. Zero for a graph with nothing wrong with it.
    std::size_t validation_errors = 0;
    /// The first validation error as a sentence, or empty. Shown when the count is not zero.
    std::string first_problem{};
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
    /// What the graph asked to have **drawn**, as the render domain declared it.
    ///
    /// `graph::ViewRequest` needs this and nothing produced it: a view item is handed the render plan's own
    /// list, and until now nothing in the view layer computed a plan at all -- the operator loop and the run
    /// providers each work from a single node. So the controller computes it here, from the same catalog the
    /// rest of the pre-flight uses, and hands it to whoever draws.
    ///
    /// It is filled whether or not a run succeeded, because what a graph declares is a property of the graph:
    /// a refused run still has something to draw once there is a previous result to draw.
    std::vector<qp::graph::DeclaredOutput> render_declared{};
    /// Where the particles ended up, as `x, y, z` triples, for a run a **provider** produced.
    ///
    /// Empty for the operator loop's own runs. It is here rather than in `RunReport` because a report
    /// is the numbers a status line quotes and this is the one thing a canvas draws; and it is a
    /// snapshot rather than a trail, which is the decision `IGraphRun::positions` argues.
    std::vector<double> particle_positions{};
    /// The **field tables the run baked**, owned here so that a view item can draw them.
    ///
    /// Empty for the operator loop's runs, which bake nothing. It is a copy rather than a pointer into the run
    /// for the reason the line above is: the run is destroyed when `run()` returns, and a view item reads this
    /// after that. The cost is written down where it is paid, in `run_controller.cpp`.
    qp::graph::field::FieldSet fields{};
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
     * @param session The graph to run. Borrowed.
     * @param ledger Where the run is recorded. Borrowed, and **the caller's** rather than this object's: a
     *                session has one history of what happened in it, so the object that runs and the objects
     *                that report must write and read the same one. The window that owns the ledger hands its
     *                own over, which is what makes its status line count the runs it just watched.
     * @param binders Consulted in order; the first that claims a node runs it.
     * @param resolve The catalog and port registry `graph/validate` needs. Passed in rather than reached for,
     *                because a controller that found them itself would be deciding which registry is
     *                authoritative -- and the window already knows.
     *
     * @ownership   observes `session` and `ledger`
     * @thread      ui
     * @pre         `session` and `ledger` outlive this object
     * @post        none
     * @invariant   The borrowed references are never written through, except the ledger, which gains one
     *              record per run that executes
     * @errors      noexcept
     * @complexity  O(binders)
     * @nondet      none
     * @frozen      no
     * @tests       run.controller.refuses_a_graph_with_nothing_to_run,
     *              run.controller.one_ledger_for_the_caller
     */
    RunController(const qp::authoring::Session& session, qp::runtime::RunLedger& ledger,
                  std::vector<graph::execution::IOperatorBinder*> binders,
                  qp::graph::ResolveContext resolve);

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
     * @brief The ledger the runs went into: the one this object was constructed with.
     *
     * The ids in `RunReport::run` are issued by it, so a caller that wants to show a run's provenance reads
     * it here -- and because it is the caller's own ledger, that caller reads the same object it already
     * holds rather than a second history of the same session.
     *
     * **This used to be one ledger per controller, and that was measured to be wrong.** The window's status
     * line counts the runs recorded in *its* ledger, so a controller holding its own made the one line the
     * user looks at say `runs 0 | reproducibility gaps: no run yet` immediately after a successful run whose
     * particles were being drawn in the panel beside it. The comment that used to be here argued for one
     * ledger per controller on the grounds that two ledgers in a session disagree; the disagreement it named
     * was real, and the fix is to have one, not to have the other one.
     *
     * @ownership   borrows from the caller
     * @thread      ui
     * @pre         The ledger outlives this object
     * @post        none
     * @invariant   Every reported `run` is a valid id in this ledger
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       run.controller.a_second_run_is_a_second_entry,
     *              run.controller.runs_a_node_and_records_its_trace,
     *              run.controller.one_ledger_for_the_caller
     */
    [[nodiscard]] const qp::runtime::RunLedger& ledger() const noexcept { return *ledger_; }

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
     *              run.controller.a_second_run_is_a_second_entry,
     *              run.controller.reports_validation_without_refusing,
     *              run.controller.carries_what_the_graph_wants_drawn
     */
    [[nodiscard]] RunResult run() const;

private:
    /// @brief The spec a run is opened with, so every run records the same inputs.
    [[nodiscard]] qp::runtime::RunSpec spec_for() const;

    const qp::authoring::Session* session_;
    std::vector<graph::execution::IOperatorBinder*> binders_;
    qp::graph::ResolveContext resolve_{};
    /// Not `mutable`, and not owned. `run()` is `const` because the controller does not change what it runs,
    /// and writing a record through a pointer does not change that: the record is the caller's, which is the
    /// point of borrowing it.
    qp::runtime::RunLedger* ledger_ = nullptr;
};

}  // namespace qp::views::model
