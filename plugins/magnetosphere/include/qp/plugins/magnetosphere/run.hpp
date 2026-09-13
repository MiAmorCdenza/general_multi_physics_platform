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
 *              magnetosphere.run.a_run_without_a_field_is_refused,
 *              magnetosphere.run.a_provider_builds_a_run_from_a_graph,
 *              magnetosphere.render.a_field_becomes_a_family_of_curves,
 *              magnetosphere.run.the_recorded_channels_are_the_ones_a_report_names,
 *              magnetosphere.run.the_cadence_resolves_the_gyration,
 *              magnetosphere.run.an_atmosphere_takes_the_speed_away
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
#include <qp/runtime/trace/trace.hpp>

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
    /// A pusher reached the particle plan and nothing feeds it: an emitter and a declared output with nothing
    /// between them. **Not** the same as a graph with no pusher at all, which is a field-only run and succeeds.
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

    /// @brief The field tables this run reads, so a view item can trace them.
    ///
    /// Returns the store the run was built over -- borrowed or owned, whichever it is -- and falls back to
    /// `IGraphRun`'s shared empty set when there is none. Calling the base rather than holding a second empty set
    /// here is deliberate: two empty stores are two answers to "how many fields are there", and the only reason
    /// to have one would be to avoid a virtual call in a function that runs once per drawing.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        The built store, or an empty set before a successful `build`
    /// @invariant   The reference stays valid until this object is destroyed or rebuilt
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.a_provider_builds_a_run_from_a_graph
    [[nodiscard]] const qp::graph::field::FieldSet& fields() const noexcept override;

    /// @brief The channels this run records, by name.
    ///
    /// Four quantities, and each one is here for a different reason -- which is the whole argument for a trace
    /// rather than a single "diagnostic" number:
    ///
    ///   - **`speed`** is the pusher's own numerical-error diagnostic. A magnetic force does no work, so the speed
    ///     of every particle is **exactly** conserved by the motion and any change in it is the integrator's. A
    ///     run whose speed drifts is a run whose answer is wrong, and this channel is the number that says so
    ///     without the reader having to know what Boris does.
    ///   - **`mu`** is the physics: the first adiabatic invariant `m v_perp^2 / 2B` is conserved when the field
    ///     varies slowly over a gyro-orbit and **not** otherwise, so its drift measures how adiabatic this
    ///     experiment actually is. That is a real, measurable property of the configuration -- not an error -- and
    ///     it is the quantity a course on magnetospheric motion is about.
    ///   - **`energy`** is `m v^2 / 2`, which for a static magnetic field is the same statement as `speed` in
    ///     different units; it is here because a report is written in joules and because its dimension is what
    ///     makes it usable by the analysis plugins.
    ///   - **`radius`** is the distance from the origin, in **metres**, and it is the channel the measurement
    ///     chain reads: the window's measurement session measures a length, so a reading taken from this trace
    ///     needs a length channel in SI. It is also what an orbit is drawn against.
    ///
    /// Every value is **SI**, converted at this kit's boundary like `positions` -- and `radius` in metres is the
    /// one that makes the difference visible, because a session whose dataset is in metres cannot take a reading
    /// from a channel in earth radii and should not be able to.
    static constexpr const char* kSpeedChannel = "speed";
    /// @brief The kinetic energy of the recorded particle, in joules. See `kSpeedChannel`.
    static constexpr const char* kEnergyChannel = "energy";
    /// @brief The first adiabatic invariant `m v_perp^2 / 2B`, in joules per tesla. See `kSpeedChannel`.
    static constexpr const char* kMuChannel = "mu";
    /// @brief The distance from the origin, in metres. See `kSpeedChannel`.
    static constexpr const char* kRadiusChannel = "radius";

    /// @brief Names the run's record, so its samples carry the identity the ledger issued.
    ///
    /// Also where the channels are declared, which is deliberate: the count has to exist before the first sample
    /// or `Trace::append` refuses a sample whose width does not match, and a run that recorded before being named
    /// would append into a trace nobody can look up. Called after a successful build and before the first step.
    ///
    /// @param run The id the ledger issued. `RunId{}` records nothing but still declares the channels.
    ///
    /// @ownership   value
    /// @thread      main
    /// @pre         none
    /// @post        `trace().run()` is `run`, and every step after this appends one sample
    /// @invariant   Declaring the channels twice does not double them: the trace is rebuilt
    /// @errors      noexcept
    /// @complexity  O(channels)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.the_recorded_channels_are_the_ones_a_report_names
    void set_run(qp::runtime::RunId run) noexcept override;

    /// @brief What this run recorded: one sample per host step, plus the initial condition.
    ///
    /// The **first particle** is the one recorded, which is the convention `GraphRun` already sets and for the same
    /// reason: a trace is a time series about something, and a channel that silently averaged a population would
    /// be a different quantity with the same name. A population aggregate is a legitimate channel; it is a
    /// different one, and it arrives with a different name when a course needs it.
    ///
    /// @ownership   borrows from this object
    /// @thread      main
    /// @pre         none
    /// @post        Empty of samples before `set_run`, and one sample per step after it
    /// @invariant   Every sample's width is four, whatever a step did
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.the_recorded_channels_are_the_ones_a_report_names
    [[nodiscard]] const qp::runtime::Trace& trace() const noexcept override;

    /// @brief How many steps of the preferred cadence make one gyro-period.
    ///
    /// A named constant rather than a number inside the arithmetic, because it is the one free choice in the
    /// cadence and a reader has to be able to argue with it: 32 steps per gyration is eleven degrees of rotation
    /// per step, which is where a second-order rotation stops visibly losing amplitude, and it costs nothing --
    /// the run's length in gyro-periods is `steps / 32` either way.
    static constexpr std::size_t kStepsPerGyration = 32;

    /// @brief How many steps one run of this kit proposes: 128 gyrations at `kStepsPerGyration` steps each.
    ///
    /// A budget this kit can justify rather than a copy of the window's: 128 periods is long enough for the ring to
    /// be a ring (the gyration is the fastest process, and the bounce is longer) and short enough that pressing Run
    /// stays a button press rather than a wait. It is the same number the window uses, arrived at from this side.
    static constexpr std::size_t kDefaultRunSteps = 4096;

    /// @brief How long one run of this kit is: enough steps to resolve the gyration, and no more.
    ///
    /// ## The measurement that made this necessary
    ///
    /// The window's own cadence -- 4096 steps of `1e-4` seconds, chosen and measured for a laboratory oscillator --
    /// is **8.7 milliseconds** in this kit's units, because one normalized time unit is the light crossing time of
    /// an earth radius (0.0213 s). A proton's gyro-period at six earth radii is **0.63 seconds**. So the ring the
    /// kit launches completed one seventy-third of a single gyration over a whole run: the particles were drawn
    /// almost exactly where they started, and every dynamic feature -- gyration, bounce, drift, convection -- was
    /// invisible. Nothing in the kit was wrong; the run was simply too short to be about anything.
    ///
    /// ## What this answers, and what it deliberately does not
    ///
    /// `steps` stays at the window's own count, because a step count is a **budget** and the budget is the
    /// caller's; what this run knows is the **time scale**. So `dt` is `1/32` of a gyro-period at the launch
    /// radius, computed from the emitted species' charge-to-mass ratio and the field actually sampled there. That
    /// resolves the fastest process by a comfortable margin (about eleven degrees of gyration per step, with the
    /// kernel's own sub-stepping as the safety net for a particle that drifts into a stronger field) and makes the
    /// run 4096/32 = 128 gyro-periods long -- 81 seconds at six earth radii.
    ///
    /// **It does not reach the drift.** The gradient-curvature drift period at that radius is hours, so no fixed
    /// step of a Boris push can show it: resolving it needs either millions of steps or a guiding-centre model, and
    /// the kit has neither. That limitation is stated here rather than discovered by a user who wonders why the
    /// ring does not move.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        A cadence whose `dt` resolves the gyration, or nothing when there is no field to compute one
    ///              from -- in which case the caller's default applies and its own limits are the honest answer
    /// @invariant   Depends only on the launched species and the field, never on the caller
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.run.the_cadence_resolves_the_gyration
    [[nodiscard]] std::optional<qp::graph::execution::RunCadence> preferred_cadence() const noexcept override;

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
    /// True for a **field-only** run as well: a graph that declared a field and no particles has been built, its
    /// bake is in the store, and `advance` is a legal call that does nothing. The alternative -- reporting false
    /// because there is no executor -- would make the one state a caller can test say "this run does not work"
    /// about a run whose whole purpose is to have baked.
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
    /// @tests       magnetosphere.run.a_rebuild_replaces_the_run,
    ///              magnetosphere.render.a_field_becomes_a_family_of_curves
    [[nodiscard]] bool built() const noexcept { return executor_ != nullptr || field_only_; }

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
    /// Whether this run baked a field and has no particles to step. See `built()` and `build_with_own_fields`.
    bool field_only_ = false;
    /// The record of this run: its channels, and one sample per step. See the channel names in the class body.
    qp::runtime::Trace trace_{qp::runtime::RunId{}};
    /// Whether `set_run` has declared the channels, which is what makes appending legal.
    bool recording_ = false;
    /// The magnetic field the recorded particle moves in, and where its samples are.
    ///
    /// Resolved at build time by the same `resolve_field` the pusher's own socket uses, so the `mu` channel samples
    /// **the field the particle actually feels** rather than a second reading of the graph -- a diagnostic computed
    /// from a different field would be a number about a different experiment.
    qp::graph::field::FieldValue recorded_field_{};
    GridSpec recorded_grid_{};
    /// The recorded species' mass, in kilograms. See `EmitterNodes::mass_of` for why the ratio is not enough.
    double recorded_mass_kg_ = 0.0;
    /// The node the recorded samples come from, for the channel's provenance.
    qp::graph::NodeId recorded_source_{};
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
