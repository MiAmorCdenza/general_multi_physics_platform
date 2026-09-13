/**
 * @file run.hpp
 * @brief A graph becomes a run: the field baked, the particles launched, the steps bound, the loop driven.
 *
 * ## What was missing, and what this is
 *
 * Every piece of the kit worked on its own before this file: a dipole bakes, an emitter launches, a pusher
 * advances a batch. What no caller could do was **start from a graph**. That is what a run is, and it is the point
 * at which four separately-verified halves have to agree -- the domain layer's plan decides which nodes belong to
 * which domain and in what order, the field domain's bake fills the store, the emitter turns a node into
 * particles, and the composition root turns pushes into steps. Each of those was built to be driven by something
 * else; this is the something.
 *
 * ## The order the run follows, and why it is that order
 *
 *   1. **plan** -- `graph/domain`'s `build_plan` from the declared outputs. Undeclared nodes enter no plan at all,
 *      which is the pruning: a graph with a second field node that nothing reads bakes nothing for it.
 *   2. **find the emitter** -- by walking the state channel **backwards** from each pusher. The wire is what says
 *      which emitter feeds which step, so nothing has to be guessed from a node type appearing somewhere in a
 *      list.
 *   3. **launch** -- the emitter's spec expanded into a `ParticleState`, against the field it is wired to. This
 *      comes before the steps because a state has to exist before an executor can be built over it.
 *   4. **bind** -- `build_particle_plan` over the particle domain's order.
 *   5. **prepare** -- `ParticleExecutor::prepare`, once, which is where a kernel validates its parameters and
 *      where a step that requires a field nobody bound is refused by name.
 *
 * ## What the run borrows, and for how long
 *
 * The `FieldSet` and the graph are **borrowed**: a view that draws the field and the run that integrates it read
 * the same one, which is the whole reason the store is a separate object rather than a member of this one. The
 * particles and the kernels are **owned**, and the executor borrows both, so this object is neither copyable nor
 * movable -- a moved run would leave an executor pointing at the state it moved away from.
 *
 * ## Why the run does not bake
 *
 * Because baking is the field domain's own step, and a caller may bake **once** and build several runs from it: a
 * student who changes the particle count does not want the field recomputed, and one who changes the tilt wants
 * exactly that. The refusal is what keeps the two honest: a run whose field is not in the store says
 * `field_not_baked` rather than integrating zeros.
 *
 * @ownership   owns the particles, the kernels and the executor; borrows the graph and the store
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `built()` is true exactly when an executor exists and has been prepared
 * @errors      See `build`
 * @frozen      no
 * @tests       magnetosphere.run.a_graph_becomes_a_run,
 *              magnetosphere.run.a_run_without_a_field_is_refused
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/domain/build.hpp>
#include <qp/graph/execution/run_provider.hpp>
#include <qp/graph/field/field_set.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/kernels/kernel.hpp>
#include <qp/graph/particles/executor.hpp>
#include <qp/graph/particles/particle_state.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

#include <qp/plugins/magnetosphere/emitter.hpp>
#include <qp/plugins/magnetosphere/plan.hpp>

namespace qp::plugins::magnetosphere {

/**
 * @brief Why a graph did not become a run.
 *
 * Nine codes, and the two that are **not** this layer's -- `plan_rejected` and `executor_rejected` -- carry the
 * refusal they came from, reachable through `plan_build()` and `executor_refusal()`. Collapsing those into one
 * "it failed" would throw away the only part a user can act on: which node is missing a field, and which slot the
 * plan could not bind.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `ok` is zero and every other code means nothing was built
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.run.a_run_without_a_field_is_refused
 */
enum class RunRefusal : std::uint8_t {
    ok = 0,
    /// The declarations reached no particle-domain node: nothing was asked for, so nothing was built.
    empty_plan = 1,
    /// The particle plan holds no pusher. An emitter and a declared output with nothing between them.
    no_pusher = 2,
    /// A pusher's state channel leads to no emitter, so there are no particles to advance.
    no_emitter = 3,
    /// Two pushers are fed by two different emitters. Advancing one batch by two plans is a real feature and it
    /// is not this one: the state is one buffer, so "which particles" would have no answer.
    several_emitters = 4,
    /// A socket that must be wired is not, or the store does not hold what it is wired to.
    field_not_baked = 5,
    /// The emitter's own numbers cannot launch anything: see `EmitterSpec::usable`, and the zero-field case.
    emitter_unusable = 6,
    /// The field node's type is one this build cannot ask for a bake grid.
    grid_unknown = 7,
    /// `build_particle_plan` refused. `plan_build()` says why.
    plan_rejected = 8,
    /// `ParticleExecutor::prepare` refused. `executor_refusal()` says why.
    executor_rejected = 9,
};

/**
 * @brief Stable short name of a refusal, for a message or a log line.
 *
 * @param refusal The refusal to name.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        One of the names in this enumerator's list, never null
 * @invariant   Total: every enumerator has a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.run.a_run_without_a_field_is_refused
 */
[[nodiscard]] const char* to_string(RunRefusal refusal) noexcept;

/**
 * @brief One experiment: a graph's field baked, its particles launched, its steps bound and driven.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   After a successful `build`, `state().count()` is the emitter's count
 * @errors      See `build` and `advance`
 * @frozen      no
 * @tests       magnetosphere.run.a_graph_becomes_a_run
 */
class MagnetosphereRun final : public qp::graph::execution::IGraphRun {
public:
    MagnetosphereRun() = default;
    ~MagnetosphereRun() = default;
    MagnetosphereRun(const MagnetosphereRun&) = delete;
    MagnetosphereRun& operator=(const MagnetosphereRun&) = delete;
    MagnetosphereRun(MagnetosphereRun&&) = delete;
    MagnetosphereRun& operator=(MagnetosphereRun&&) = delete;

    /**
     * @brief Turns a graph into a run, or says why it cannot.
     *
     * Called on an object that may already hold a run: everything is released first, so a rebuild after a
     * parameter change is one call rather than a destruction and a construction. A **failed** build leaves the
     * object empty rather than half built, for the reason `BuiltPlan` clears itself.
     *
     * @param graph      The graph. Borrowed, and must outlive this object.
     * @param declared   What the run is asked for: the declared outputs the domain layer prunes from. A
     *                   declaration naming a pusher's state output is what makes its whole upstream branch --
     *                   field, emitter, pusher -- part of the plan.
     * @param catalog    The node-type catalog. Borrowed.
     * @param fields     The baked fields. Borrowed, and must outlive this object. **Already baked**: the run does
     *                   not bake, because one bake may serve several runs.
     *
     * @ownership   owns what it builds, borrows the rest
     * @thread      main
     * @pre         `graph`, `catalog` and `fields` outlive this object
     * @post        On `ok`, `built()` is true and `state()` holds the emitter's particles
     * @invariant   On any refusal the object is empty and `built()` is false
     * @errors      Reports a `RunRefusal`; see its own list for what each one means
     * @complexity  O(nodes + particles)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.run.a_graph_becomes_a_run,
     *              magnetosphere.run.a_run_without_a_field_is_refused,
     *              magnetosphere.run.a_rebuild_replaces_the_run
     */
    [[nodiscard]] RunRefusal build(const qp::graph::Graph& graph, const qp::graph::Declarations& declared,
                                   const qp::graph::INodeCatalog& catalog,
                                   qp::graph::field::FieldSet& fields);

    /**
     * @brief Advances the whole run by `steps` host steps of `dt`.
     *
     * A loop rather than a single step, because "advance this run by ten thousand steps" is what a caller wants
     * and a caller that wrote the loop itself would have to decide what to do about the counters and the first
     * refusal. The first refusal stops the loop and is returned: a run whose kernel refused a step did not
     * silently take the rest.
     *
     * @param steps How many host steps. Zero is legal and does nothing.
     * @param dt    The step, in normalized time units.
     *
     * @ownership   value (the state is modified through the reference)
     * @thread      main
     * @pre         `built()`
     * @post        On success the state has been advanced by exactly `steps` steps
     * @invariant   A refused step leaves the counters as they were
     * @errors      `not_implemented` when nothing was built; the kernel's own code otherwise
     * @complexity  O(steps * particles * sub-steps)
     * @nondet      only through the kernel, which is deterministic
     * @frozen      no
     * @tests       magnetosphere.run.a_graph_becomes_a_run
     */
    [[nodiscard]] qp::diag::Result<void> advance(std::size_t steps, double dt);

    /// @brief The particles this run owns. Never moved after `build`.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        The batch, empty before a successful `build`
    /// @invariant   Its address is stable for the object's lifetime
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_graph_becomes_a_run
    [[nodiscard]] const qp::graph::particles::ParticleState& state() const noexcept { return state_; }

    /// @brief The steps this run drives, and the counters a report quotes.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        An empty report before a successful `build`
    /// @invariant   The same numbers the executor holds
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_graph_becomes_a_run
    [[nodiscard]] const qp::graph::particles::AdvanceReport& advance_report() const noexcept;

    /// @brief What the run has done, in the vocabulary `IGraphRunProvider` hands a caller.
    ///
    /// The counters the executor keeps, plus the particle census the state knows, in one value. Every field is
    /// derived rather than stored: a second copy of a counter is a second answer to "how far did this get", and
    /// the two would agree until one of them was updated.
    ///
    /// @ownership   owns (the returned report carries a string)
    /// @thread      main
    /// @pre         none
    /// @post        A consistent report; all zeros before a successful `build`
    /// @invariant   `live + absorbed + escaped == particles`
    /// @errors      noexcept
    /// @complexity  O(particles)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
    [[nodiscard]] qp::graph::execution::GraphRunReport report() const override;

    /// @brief Where the particles are, as `x, y, z` triples in **earth radii**.
    ///
    /// Converted from the SI the state holds, because this is the one number a caller draws with and a canvas
    /// whose axes are in metres is a canvas nobody can read. The conversion is here rather than in the view for
    /// the reason every other conversion in this kit is at its boundary: the kit owns the unit system, and a
    /// caller that had to know `R_E` to draw a picture would be a caller that had to know the physics.
    ///
    /// @ownership   owns
    /// @thread      main
    /// @pre         none
    /// @post        `size()` is `3 * state().count()`, or zero
    /// @invariant   One triple per particle, in slot order
    /// @errors      noexcept
    /// @complexity  O(particles)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
    [[nodiscard]] std::vector<double> positions() const override;

    /**
     * @brief Builds a run over a graph, **baking the field domain into a store this object owns**.
     *
     * The difference from `build` is ownership, and it exists because of who calls which. A window or a test that
     * already holds a store -- because it draws the field, or because it wants two runs over one bake -- calls
     * `build` and keeps reading the store it passed. A **run provider** has nobody to borrow from: it is handed a
     * graph and has to answer with a run, so it evaluates the field domain itself and the store lives here.
     *
     * The bake is `evaluate_graph` with this kit's own `DipoleEvaluator`, no cache, and a fresh store: pressing
     * Run means "bake what the graph says now", not "reuse whatever was baked last time". That costs a few
     * milliseconds for the default grid and is the honest answer -- a cache here would be a second answer to
     * "is this field still the one the parameters describe", and the evaluator's own cache key is where that
     * question already has an answer.
     *
     * @param graph    The graph. Borrowed for the duration of the call.
     * @param declared What the run is asked for; see `build`.
     * @param catalog  The node-type catalog.
     *
     * @ownership   owns the store, the particles, the kernels and the executor
     * @thread      main
     * @pre         `catalog` outlives the call
     * @post        On `ok`, `built()` is true and `state()` holds the emitter's particles
     * @invariant   On any refusal the object is empty and `built()` is false
     * @errors      Reports a `RunRefusal`; `field_not_baked` when the bake itself failed, which for this graph
     *              means a field node the evaluator could not honour
     * @complexity  O(nodes + baked points + particles)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
     */
    [[nodiscard]] RunRefusal build_with_own_fields(const qp::graph::Graph& graph,
                                                   const qp::graph::Declarations& declared,
                                                   const qp::graph::INodeCatalog& catalog);

    /// @brief The plan the run was built from, for a caller that wants to see what it became.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        The built plan, empty before a successful `build`
    /// @invariant   Its kernels are alive for as long as this object is
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_graph_becomes_a_run
    [[nodiscard]] const BuiltPlan& plan() const noexcept { return plan_; }

    /// @brief The initial condition the run launched, so a report can say what the experiment was.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        The spec, or a default-constructed one before a successful `build`
    /// @invariant   The particles in `state()` were produced from it
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_graph_becomes_a_run
    [[nodiscard]] const EmitterSpec& emitter() const noexcept { return emitter_; }

    /// @brief The bytes of baked field the run reads, for a report that has to say what it cost.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        Zero before a successful `build`
    /// @invariant   Equals the borrowed store's own total
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_graph_becomes_a_run
    [[nodiscard]] std::uint64_t baked_bytes() const noexcept {
        return fields_ == nullptr ? 0U : fields_->bytes();
    }

    /// @brief Whether a run was built and prepared.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        True exactly when `advance` can be called
    /// @invariant   False after a failed `build`
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_rebuild_replaces_the_run
    [[nodiscard]] bool built() const noexcept { return executor_ != nullptr; }

    /// @brief Why `build_particle_plan` refused, when the refusal was `plan_rejected`.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        `ok` unless the last build was refused by the plan builder
    /// @invariant   The last build's answer, not a guess
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_run_without_a_field_is_refused
    [[nodiscard]] PlanBuildRefusal plan_build() const noexcept { return plan_refusal_; }

    /// @brief Why `ParticleExecutor::prepare` refused, when the refusal was `executor_rejected`.
    ///
    /// @ownership   pure
    /// @thread      any
    /// @pre         none
    /// @post        `ok` unless the last build was refused by the executor
    /// @invariant   The last build's answer, not a guess
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_run_without_a_field_is_refused
    [[nodiscard]] qp::graph::particles::PlanRefusal executor_refusal() const noexcept {
        return executor_refusal_;
    }

private:
    /// @brief Releases everything a build produced, leaving an empty run.
    void reset() noexcept;

    qp::graph::field::FieldSet* fields_ = nullptr;
    /// The store, when this object baked it. Null when the caller owns one and passed it to `build`.
    std::unique_ptr<qp::graph::field::FieldSet> owned_fields_{};
    qp::graph::particles::ParticleState state_{};
    BuiltPlan plan_{};
    std::unique_ptr<qp::graph::particles::ParticleExecutor> executor_{};
    EmitterSpec emitter_{};
    PlanBuildRefusal plan_refusal_ = PlanBuildRefusal::ok;
    qp::graph::particles::PlanRefusal executor_refusal_ = qp::graph::particles::PlanRefusal::ok;
};

/**
 * @brief The kit's answer to "run this graph", for a caller that has no idea what a particle is.
 *
 * It owns nothing between calls: `build` bakes, launches and binds into a fresh `MagnetosphereRun`, and hands it
 * back as an `IGraphRun`. Everything the run needs -- the store, the kernels, the executor -- lives inside that
 * object, which is what lets this header's interface mention no field type at all.
 *
 * @ownership   observes (holds no state; every run is the caller's)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `claims` is true exactly for a graph holding one of this kit's node types
 * @errors      See `build`
 * @frozen      no
 * @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
 */
class MagnetosphereRunProvider final : public qp::graph::execution::IGraphRunProvider {
public:
    /// @brief Stable name, for the status line and for the ledger.
    static constexpr const char* kName = "magnetosphere";

    /// @brief The name a caller reads.
    ///
    /// @ownership   observes
    /// @thread      any
    /// @pre         none
    /// @post        `kName`
    /// @invariant   Constant
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
    [[nodiscard]] std::string_view name() const noexcept override { return kName; }

    /// @brief Whether the graph holds a node of one of this kit's three types.
    ///
    /// Decided by the type name each **node** carries rather than by looking the type up in the catalog: a graph
    /// whose type is missing from the catalog is a validation finding, and a run provider that answered "not
    /// mine" for it would hide that finding behind a "nothing to run" sentence.
    ///
    /// @param graph The graph.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        True when at least one occupied slot holds a field, emitter or pusher node
    /// @invariant   Never inspects parameters or edges
    /// @errors      noexcept
    /// @complexity  O(nodes)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
    [[nodiscard]] bool claims(const qp::graph::Graph& graph) const noexcept override;

    /**
     * @brief Builds the run, declaring every pusher in the graph as an output.
     *
     * **The declarations are synthesized here**, and that is a decision rather than a shortcut. A declared output
     * is "what I want evaluated", and the view layer has no editor for them yet; what a user means by pressing
     * Run on a graph of this kind is "run the pushers that are in it", which is exactly one declaration per
     * pusher node. When a declared-output editor exists, this is the function that stops synthesizing.
     *
     * @param graph   The graph.
     * @param catalog The node-type catalog.
     *
     * @ownership   owns the returned run
     * @thread      main
     * @pre         `claims(graph)` is true
     * @post        On success a prepared `MagnetosphereRun`, owned by the returned result
     * @invariant   On refusal the sentence is this kit's own `to_string(RunRefusal)`
     * @errors      Reports a sentence; see `IGraphRunProvider`
     * @complexity  O(nodes + baked points + particles)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
     */
    [[nodiscard]] qp::graph::execution::RunBuildResult build(const qp::graph::Graph& graph,
                                                             const qp::graph::INodeCatalog& catalog) override;
};

}  // namespace qp::plugins::magnetosphere
