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
 * ## Why the interface mentions no field type
 *
 * A provider owns everything its run needs, including the baked field store: the view layer asks for a run and
 * gets back a report and a snapshot of positions, and it never sees a `FieldValue`, a lattice or a bake. That is
 * what keeps this header in a module that knows nothing about fields -- and it is a decision with a cost worth
 * stating: a view that wants to **draw the field itself** (field lines, a colour map) cannot ask this interface
 * for it. The reopening condition is a render domain that declares "draw the field of node X", at which point the
 * store becomes part of the contract rather than a private detail of one kit.
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
#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
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

    /// @brief Whether every particle is accounted for.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        True exactly when the three status counts sum to `particles`
    /// @invariant   An empty report is consistent
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       execution.run_provider.a_provider_names_its_own_refusal
    [[nodiscard]] bool is_consistent() const noexcept {
        return live + absorbed + escaped == particles;
    }
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

    /// @brief What the run has done so far.
    ///
    /// @ownership   owns (the returned report carries a string)
    /// @thread      main
    /// @pre         none
    /// @post        A consistent report; all zeros before the first step
    /// @invariant   Monotone in every count except `live`, which falls as particles retire
    /// @errors      noexcept
    /// @complexity  O(particles)
    /// @nondet      none
    /// @frozen      no
    /// @tests       execution.run_provider.a_provider_names_its_own_refusal
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

    /// @brief Whether a run was built.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        Equivalent to `run != nullptr`
    /// @invariant   Never true together with a non-empty refusal
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       execution.run_provider.a_provider_names_its_own_refusal
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

    /// @brief Stable short name, for a status line and for the ledger.
    ///
    /// @ownership   observes
    /// @thread      any
    /// @pre         none
    /// @post        Non-empty
    /// @invariant   Constant for the object's lifetime
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       execution.run_provider.a_provider_names_its_own_refusal
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
