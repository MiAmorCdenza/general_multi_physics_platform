/**
 * @file run_provider.hpp
 * @brief The second kind of run: a whole graph, driven by content, reported to whoever asked.
 *
 * ## The gap this closes
 *
 * `GraphRun` is the platform's run loop, and it is **one operator bound to one node**: it looks for a node a
 * binder claims, builds an `IStateOperator` for it, steps it, and records the samples into a trace. That covers
 * every domain whose state is a handful of doubles per particle.
 *
 * It does not cover a run whose *setup* is a computation. A magnetosphere graph has to be **baked** (the field
 * domain evaluated onto a lattice, which may allocate and may block), then **launched** (an emitter node expanded
 * into a particle batch), and only then stepped -- and none of those three phases is something `GraphRun` can be
 * asked for. The kit that knows how to do them is content, so the view layer cannot name it; and the kit cannot
 * name the view layer either, because content that depends on a view is the inversion this project forbids.
 *
 * ## The same inversion `IOperatorBinder` uses, one level up
 *
 * `IOperatorBinder` answers "which operator drives this node". This answers "which run drives this graph", and
 * the wiring is the same: the interface lives in the module whose subject it is -- here, the run loop's contracts
 * -- the implementation lives in the plugin that knows how, and the **application** is the one place that knows
 * which plugins exist and therefore the one place that fills the list. Nothing in `views/` names a kit.
 *
 * ## Why the interface mentions no field type -- and the condition that has now fired
 *
 * A provider owns everything its run needs, including the baked field store: the view layer asks for a run and
 * gets back a report and a snapshot of positions, and it never sees a `FieldValue`, a lattice or a bake. That is
 * what keeps this header in a module that knows nothing about fields -- and it is a decision with a cost worth
 * stating: a view that wants to **draw the field itself** (field lines, a colour map) cannot ask this interface
 * for it.
 *
 * That cost was written down as a reopening condition -- "a render domain that declares *draw the field of node
 * X*" -- and the kit now has exactly that node (`render.field_lines`, whose `data` socket names the field to
 * trace). So `fields()` is below, and this module depends on `field`.
 *
 * **Why the new edge points down.** `field` depends on `units`, `diag` and `abi` only: it holds no algorithm and
 * knows nothing about nodes and edges, so `execution -> field` cannot cycle and the layer table gains one line
 * rather than the design gaining a wrinkle. What is deliberately **not** here is as much a part of the decision
 * as what is: no lattice description, no seed point, no step size, no geometry. A run reports the tables it
 * baked. Deciding which of them is interesting, where to start tracing and what shape comes out is the view
 * item's business, and an interface that carried any of those would be the run making a drawing decision.
 *
 * ## Why a refusal is a sentence and not an error code
 *
 * `build` can refuse for reasons that are the provider's own and that a user has to act on differently: a graph
 * whose magnetic socket nobody wired, a socket wired to a node whose field was never baked, two emitters feeding
 * two chains. `diag::ErrorCode` has no names for those, and collapsing them into one code would throw away the
 * only part a reader can use -- the same argument `PlanRefusal` and `RunRefusal` are built on. So a refusal is a
 * **string the provider wrote**, empty on success.
 *
 * @ownership   pure (describes two contracts; owns nothing)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A provider that answers `claims` false is never asked to build
 * @errors      See each declaration
 * @frozen      no
 * @tests       execution.run_provider.a_provider_names_its_own_refusal
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/field/field_set.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>
#include <qp/runtime/trace/trace.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qp::graph::execution {

/**
 * @brief What a whole-graph run has done, in the numbers a status line quotes.
 *
 * @ownership   owns (`note` allocates)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `live + absorbed + escaped == particles` for a report a run produced
 * @errors      noexcept
 * @frozen      no
 * @tests       execution.run_provider.a_provider_names_its_own_refusal
 */
struct GraphRunReport final {
    /// Host steps taken since the run began.
    std::size_t steps = 0;
    /// How many particles the run started with.
    std::size_t particles = 0;
    /// How many are still being integrated.
    std::size_t live = 0;
    /// How many reached the body.
    std::size_t absorbed = 0;
    /// How many left the modelled region, or stopped being numbers.
    std::size_t escaped = 0;
    /// Values a clamp policy altered, summed over the run. Never silently zero: see the kernel contract.
    std::size_t clamped = 0;
    /// Particles whose speed a kernel limited. A configuration finding rather than a numerical one.
    std::uint64_t speed_clamps = 0;
    /// One line for the status bar, in the provider's own words. May be empty.
    std::string note{};

    /**
     * @brief Whether every particle is accounted for.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        True exactly when the three status counts sum to `particles`
     * @invariant   An empty report is consistent
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] bool is_consistent() const noexcept {
        return live + absorbed + escaped == particles;
    }
};

/**
 * @brief How long one run of a given kind is: a step count and a step size, in the run's own units.
 *
 * ## Why this is the run's opinion and not the caller's decision
 *
 * The window used to decide, with two constants chosen for a laboratory oscillator: 4096 steps of `1e-4` seconds.
 * For that experiment they are right, and the kit they were chosen for is the one they were measured against. For
 * a magnetosphere they are **not wrong so much as meaningless**: this kit's time unit is the light crossing time of
 * an earth radius, so 4096 steps of `1e-4` is 8.7 **milliseconds**, while a proton's gyro-period at six earth radii
 * is 0.63 seconds. The run therefore covers one seventy-third of a single gyration, and every dynamic feature the
 * kit models -- gyration, bounce, drift, convection -- is invisible at every step count the pusher could afford.
 *
 * The caller cannot fix that, because the caller does not know what a gyro-period is. The **run** does: it has the
 * launched particles, the field they move in, and therefore the fastest time scale that must be resolved for its
 * own output to mean anything. So a run may state its cadence, and a run with no opinion leaves the caller's
 * default in place -- which is what keeps every existing run behaving exactly as it did.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `steps > 0` and `dt > 0` when a cadence is given
 * @errors      noexcept
 * @frozen      no
 * @tests       execution.run_provider.a_run_may_state_its_own_cadence
 */
struct RunCadence final {
    /// How many host steps one run should take.
    std::size_t steps = 0;
    /// The step size, in the units the run's own `advance` expects.
    double dt = 0.0;
};

/**
 * @brief One whole-graph run, built by a provider and stepped by its caller.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `advance` never makes the report inconsistent
 * @errors      See each declaration
 * @frozen      no
 * @tests       execution.run_provider.a_provider_names_its_own_refusal
 */
class IGraphRun {
public:
    IGraphRun() = default;
    virtual ~IGraphRun() = default;
    IGraphRun(const IGraphRun&) = delete;
    IGraphRun& operator=(const IGraphRun&) = delete;

    /**
     * @brief Advances the run by `steps` host steps of `dt`.
     *
     * A loop rather than one step, for the reason `GraphRun` takes a count: a caller that wrote the loop itself
     * would have to decide what to do about the counters and about the first refusal, and every caller would
     * decide differently.
     *
     * @param steps How many host steps. Zero is legal and does nothing.
     * @param dt    The step, in the units the provider's own content uses.
     *
     * @ownership   value (the run's own state is modified through this object)
     * @thread      main
     * @pre         none
     * @post        On success the run has advanced by exactly `steps` steps
     * @invariant   A refused step leaves the counters as they were, and the first refusal ends the loop
     * @errors      The provider's own code; `invalid_argument` for a zero or non-finite `dt`
     * @complexity  O(steps x particles)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] virtual diag::Result<void> advance(std::size_t steps, double dt) = 0;

    /**
     * @brief What the run has done so far.
     *
     * @ownership   owns (the returned report carries a string)
     * @thread      main
     * @pre         none
     * @post        A consistent report; all zeros before the first step
     * @invariant   Monotone in every count except `live`, which falls as particles retire
     * @errors      Allocates the report's sentence; a failure to allocate propagates
     * @complexity  O(particles)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] virtual GraphRunReport report() const = 0;

    /**
     * @brief Where the particles are, as `x, y, z` triples in the provider's own units.
     *
     * **A snapshot, not a trail.** A canvas that wants to draw an orbit keeps successive snapshots -- which is a
     * view decision, and one this interface deliberately does not make: a run that accumulated every position of
     * every particle would hold steps x particles x 3 doubles, and a caller that wanted only the final frame
     * would pay for all of them.
     *
     * An empty vector is a legitimate answer for a run with nothing to draw.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        `size()` is a multiple of three, or zero
     * @invariant   One triple per particle, in the order the run's own state holds them
     * @errors      Cannot fail: a snapshot of state the run already holds
     * @complexity  O(particles)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] virtual std::vector<double> positions() const = 0;

    /**
     * @brief The field tables this run baked, so that a view item can draw them.
     *
     * **Empty for a run that baked nothing**, which is the ordinary answer rather than an edge case: the operator
     * loop binds one operator to one node and never enters the field domain, so the default implementation below
     * is what every existing run in this repository uses. That is why it is virtual with a definition rather than
     * pure -- a pure one would stop every implementation, including the ones in tests, from compiling for a
     * capability most of them do not have and cannot use.
     *
     * The returned reference **borrows from the run**, so it is valid exactly as long as the run is. A caller that
     * outlives the run copies it; `RunResult` in the view layer does, by the rule it already applies to
     * positions. Returning an empty `FieldSet` rather than a pointer keeps "there is nothing to draw" and "there
     * is something to draw and it is empty" the same question, which is the answer `field::is_readable` is built
     * on and the reason a null check is not needed at every call site.
     *
     * @ownership   borrows from this object
     * @thread      main
     * @pre         none
     * @post        A set whose keys this run published, empty for a run that published none
     * @invariant   The reference stays valid until the run is destroyed, and no call ever invalidates a previous
     *              answer: a run bakes before it steps, so its published set does not change while it is advanced
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_run_with_no_field_answers_with_an_empty_set
     */
    [[nodiscard]] virtual const qp::graph::field::FieldSet& fields() const noexcept;

    /**
     * @brief Tells the run which record in the ledger its samples belong to.
     *
     * Called **after** a successful build and before the first step, which is the order the operator path already
     * documents: binding comes before anything is written down, so a refused build leaves no ledger entry, and an
     * entry that exists always has a run that really executed. A run that has not been told answers with an empty
     * trace whose `RunId` is invalid, which is the honest state and the one a caller can test.
     *
     * Why the identity arrives from outside at all: a trace's samples are only lookups if somebody issued the id,
     * and the issuer is the ledger -- which belongs to whoever started the run, not to the run. `Trace` therefore
     * takes its `RunId` at construction and this call is what supplies it.
     *
     * @param run The id the ledger issued. `RunId{}` is legal and means "not recorded".
     *
     * @ownership   value
     * @thread      main
     * @pre         none
     * @post        On an implementation that records, `trace().run()` is `run`
     * @invariant   Does not disturb the report, the positions or the fields
     * @errors      noexcept
     * @complexity  O(channels)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_run_records_under_the_id_it_was_given
     */
    virtual void set_run(qp::runtime::RunId run) noexcept;

    /**
     * @brief What this run recorded, for the measurement chain to read.
     *
     * **Empty for a run that records nothing**, which is the default and is not a failure: a run's job is to
     * advance state, and recording is a service it may offer. The default is a shared empty trace rather than a
     * pure virtual for the reason `fields()` gives -- a pure one would stop every existing implementation from
     * compiling for a capability it does not have.
     *
     * This is the interface the platform's whole closed loop hangs from at this end: without a trace there is no
     * measurement, no uncertainty and no report, which is why the flagship kit records one. The channels are the
     * run's own -- a trace names quantities, and which quantities are worth recording is content.
     *
     * @ownership   borrows from this object
     * @thread      main
     * @pre         none
     * @post        A trace whose channels and samples are the run's own; empty before `set_run`
     * @invariant   Every append during `advance` grows it by at most one sample per step
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_run_records_under_the_id_it_was_given
     */
    [[nodiscard]] virtual const qp::runtime::Trace& trace() const noexcept;

    /**
     * @brief How long this run should be, or nothing when the caller's default applies.
     *
     * Asked **after** a successful build and before the first step, because the answer depends on what was built:
     * a run's step size comes from the time scale of the state it launched, and that state does not exist until
     * the build has succeeded.
     *
     * An empty answer is the default and means "I have no opinion", which is the honest answer for a run whose
     * configuration is the caller's business -- the operator loop's oscillator, for instance, whose cadence the
     * window has measured and chosen. A run that answers must answer with a cadence that resolves its own fastest
     * process; a run that resolves nothing is a run whose numbers are about no experiment.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        A positive step count and step size, or nothing
     * @invariant   The answer depends only on what was built, never on the caller
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_run_may_state_its_own_cadence
     */
    [[nodiscard]] virtual std::optional<RunCadence> preferred_cadence() const noexcept;
};

/**
 * @brief What a provider answered: a run, or the sentence that says why not.
 *
 * @ownership   owns the run and the sentence
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `run == nullptr` exactly when `refusal` is non-empty
 * @errors      noexcept
 * @frozen      no
 * @tests       execution.run_provider.a_provider_names_its_own_refusal
 */
struct RunBuildResult final {
    /// The run, or null. The caller owns it.
    std::unique_ptr<IGraphRun> run{};
    /// Why there is no run, in the provider's own words. Empty on success.
    std::string refusal{};

    /**
     * @brief Whether a run was built.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        Equivalent to `run != nullptr`
     * @invariant   Never true together with a non-empty refusal
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] bool ok() const noexcept { return run != nullptr; }
};

/**
 * @brief Turns a whole graph into a run, or explains why it cannot.
 *
 * Implementations live in plugins, next to the content they drive, and the application fills the list -- the same
 * arrangement `IOperatorBinder` documents at length, and for the same reason: content that the view layer named
 * by type would make a window that cannot be built without that plugin.
 *
 * @ownership   observes (an implementation is owned by whoever registered it)
 * @thread      main (build), and the run it returns is driven by the caller
 * @pre         none
 * @post        none
 * @invariant   `claims` depends only on the graph, never on a previous `build`
 * @errors      See each declaration
 * @frozen      no
 * @tests       execution.run_provider.a_provider_names_its_own_refusal
 */
class IGraphRunProvider {
public:
    IGraphRunProvider() = default;
    virtual ~IGraphRunProvider() = default;
    IGraphRunProvider(const IGraphRunProvider&) = delete;
    IGraphRunProvider& operator=(const IGraphRunProvider&) = delete;

    /**
     * @brief Stable short name, for a status line and for the ledger.
     *
     * @ownership   observes
     * @thread      any
     * @pre         none
     * @post        Non-empty
     * @invariant   Constant for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Whether this graph is one this provider can run.
     *
     * Asked **before** `build`, so a provider is never asked to build a graph it does not want: "not mine" is the
     * normal answer from all but one provider in the list, and it is not a failure. The catalog is deliberately
     * not passed -- a provider recognises its own node types by the name each node carries, and a graph whose
     * type is missing from the catalog is a **validation** finding rather than something a run provider should be
     * deciding.
     *
     * @param graph The graph.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The answer depends only on the graph
     * @invariant   A provider answering true answers non-null from `build` for a complete graph, and a sentence
     *              for an incomplete one
     * @errors      noexcept
     * @complexity  O(nodes)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] virtual bool claims(const Graph& graph) const noexcept = 0;

    /**
     * @brief Bakes, launches and binds whatever the graph needs, and answers with the run.
     *
     * Everything expensive happens here: a bake may allocate and may block, an emitter expands a node into a
     * batch, and a plan is built and prepared. That is why this is a separate phase from `advance` and why the
     * caller is expected to call it once per run rather than per frame.
     *
     * @param graph   The graph to run.
     * @param catalog The node-type catalog, for whatever the provider has to look up.
     *
     * @ownership   owns the returned run
     * @thread      main
     * @pre         `claims(graph)` is true
     * @post        On success the run is prepared and can be advanced
     * @invariant   On refusal nothing was left half built, and the sentence names what was missing
     * @errors      Reports a sentence rather than an error code; see the file comment
     * @complexity  O(nodes + particles + baked points)
     * @nondet      none
     * @frozen      no
     * @tests       execution.run_provider.a_provider_names_its_own_refusal
     */
    [[nodiscard]] virtual RunBuildResult build(const Graph& graph, const INodeCatalog& catalog) = 0;
};

}  // namespace qp::graph::execution
