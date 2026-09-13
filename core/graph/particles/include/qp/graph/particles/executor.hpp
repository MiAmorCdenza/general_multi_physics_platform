/**
 * @file executor.hpp
 * @brief The bridge: a plan of kernels, stepped over a particle batch.
 *
 * ## What was missing, and what this is
 *
 * `graph/kernels` has had a complete contract since it was written -- `IBatchAdvancer`, `BatchView`,
 * `ParamBlock`, `AdvanceContext`, `ClampPolicy`, `KernelRegistry` -- and, until this file, **no production code
 * ever called any of it**. `KernelRegistry` appeared in the host's own test and nowhere else; nothing in the tree
 * had ever constructed a `BatchView` or driven one step. That is the same shape this project keeps finding: a
 * contract that is declared, documented and unit-tested, and unreachable from the running program. It is not a
 * gap in the physics; it is a gap between two layers that were both finished.
 *
 * `graph/domain` decides *which* nodes belong to the particle domain and in what order (`DomainPlan::order`),
 * and stops there. This file is what carries the plan the rest of the way: it holds the kernels a plan operation
 * resolved to, the parameter block each was prepared with, and the field slots each reads, and it drives them
 * over a `ParticleState` one step at a time.
 *
 * ## Why the plan is a value and the executor is not
 *
 * A `StepPlan` is a description -- what runs, in what order, reading what -- and it is built once, from the
 * graph, before a run starts. A `ParticleExecutor` owns the run: it holds the state a step mutates, the scratch
 * the kernels asked for, and the counters a report quotes. The split matters because the first can be inspected
 * and asserted on (`check_plan` answers before anything moves) and the second cannot.
 *
 * ## The step order, and why `prepare` is not optional
 *
 * Every kernel is `prepare`d **once, in order, before the first step**, and a plan whose kernel refuses is
 * refused as a plan rather than halfway through a run. That is the interface's own requirement -- "called before
 * any step runs, so that a parameter block the operator cannot use is rejected while the user is still editing
 * the graph" -- and it is also the only way the scratch requirement can be honoured, because
 * `Capability::needs_scratch` is read from `capabilities()` and the buffer has to exist before `advance`.
 *
 * ## Clamping is counted, never silent
 *
 * `ClampPolicy` exists because charter C2 requires that a run never explodes and the project's own rule is that
 * the platform must never clamp **silently** -- a wrong answer wearing a right answer's clothes. The executor
 * therefore applies the policy to every component a kernel wrote and counts what it changed, and those counts
 * are what a confidence panel reports. A kernel that writes a NaN gets a zero and a tally, not a blank screen
 * and not a question nobody can answer later.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main (build, prepare) / eval (advance)
 * @pre         none
 * @post        none
 * @invariant   No kernel is called before it has been prepared
 * @errors      See each declaration
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       particles.executor.a_plan_prepares_every_kernel_once,
 *              particles.executor.a_step_counts_every_kernel_that_ran,
 *              particles.executor.in_place_and_out_of_place_agree,
 *              particles.executor.a_self_inverse_kernel_round_trips,
 *              particles.executor.a_clamp_is_counted_not_hidden,
 *              particles.executor.an_empty_plan_is_refused
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/graph/field/field.hpp>
#include <qp/graph/kernels/kernel.hpp>
#include <qp/graph/particles/particle_state.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qp::graph::particles {

/**
 * @brief Where a field slot's data comes from.
 *
 * The three names are the reference implementation's `ForceTables` -- a magnetic field, an electric field, and a
 * drag coefficient -- and they are the slots a charged-particle pusher reads. A slot is **optional** (a run with
 * no electric field is the ordinary case, and an absent field is zero force rather than an error) and it is
 * **bound by the executor's owner**, not discovered here: this file does not know that a graph has field-domain
 * nodes, and it must not, because the field it is handed may equally have come from a baked table, a constant, or
 * a test.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One enumerator per force a step can read
 * @errors      noexcept
 * @frozen      no
 * @tests       particles.executor.a_bound_field_reaches_the_kernel
 */
enum class SlotName : std::uint8_t {
    /// The magnetic field, a vector field in tesla. The one slot a magnetic pusher requires.
    magnetic = 0,
    /// The electric field, a vector field in volts per metre. Absent means no electric force.
    electric = 1,
    /// The drag coefficient, a scalar field in per second. Absent means no drag.
    drag = 2,
};

/// @brief How many slots a plan can bind.
inline constexpr std::size_t kSlotNameCount = 3;

/**
 * @brief Stable short name of a slot, for a message or a log line.
 *
 * @param name The slot to name.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        One of the three names, never null
 * @invariant   Total: every enumerator has a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       particles.executor.a_bound_field_reaches_the_kernel
 */
[[nodiscard]] const char* to_string(SlotName name) noexcept;

/**
 * @brief The bit that stands for one slot, for a plan that has to declare which slots it needs.
 *
 * A mask rather than three booleans because the plan carries one word and `prepare` answers one question -- "is
 * everything this step needs bound?" -- and a mask makes that a single test that cannot forget a slot. The
 * enumeration order is the bit order, so `slot_bit(SlotName::magnetic)` is a value a plan can be initialised with
 * at compile time.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Exactly one bit is set, and it is the bit of `name`
 * @invariant   Distinct slots give distinct bits
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       particles.executor.a_required_slot_that_is_unbound_is_refused
 */
[[nodiscard]] constexpr std::uint32_t slot_bit(SlotName name) noexcept {
    return 1U << static_cast<unsigned>(name);
}

/// @brief A plan that requires no field at all -- an operator that only moves what it is given.
inline constexpr std::uint32_t kNoSlotsRequired = 0;

/**
 * @brief Where each state buffer sits in the array a kernel is handed.
 *
 * ## Why this is one enumeration and not six literals
 *
 * A kernel sees a `BatchView`: an array of described buffers with no names on them. Which index holds the
 * charge-to-mass ratio, and which holds the magnetic field, is therefore a fact that **two** components have to
 * agree on -- this module, which fills the array, and the operator, which reads it. A literal `4` in both is two
 * chances to disagree, and the disagreement is invisible: an operator reading the status where it expected a
 * charge computes a plausible acceleration from a state code.
 *
 * So the layout is declared **here**, once, and the operator is expected to name the slots it reads through this
 * enumeration rather than through arithmetic on `SlotName`'s position. The two halves are checked against each
 * other by `static_assert` immediately below, because the one thing a rule like this cannot survive is a later
 * edit to one of them.
 *
 * ## Why the fields follow the state instead of getting a channel of their own
 *
 * `IBatchAdvancer::advance` is handed a batch and nothing else, and that is deliberate: the contract is one
 * virtual call per step with no callback into the host per particle, so there is no second argument to pass a
 * bound field through. Appending the field slots to the batch keeps the contract intact and costs an operator
 * that reads none of them nothing -- it loops over `count` slots it was told to loop over, which for a plan with
 * three bound fields is seven instead of four.
 *
 * The field slots carry **borrowed, read-only** data: `FieldValue::data` is `const void*`, and an operator that
 * wrote through it would be writing into the field model's own storage. That is a real hazard and it is stated
 * rather than enforced, because enforcing it would mean a second type in the batch and the batch is deliberately
 * one type.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `position` to `status` match `ParticleState::Slot` one for one
 * @errors      noexcept
 * @frozen      no
 * @tests       particles.executor.the_batch_layout_is_declared_once
 */
enum class BatchSlot : std::uint8_t {
    /// Positions, three f64 components per particle.
    position = 0,
    /// Velocities, three f64 components per particle.
    velocity = 1,
    /// Charge-to-mass ratio, one f64 component per particle.
    charge_mass = 2,
    /// `Status` as a number, one f64 component per particle.
    status = 3,
    /// The magnetic field, a vector lattice in tesla. The first field slot, at `kFieldSlotBase`.
    magnetic = 4,
    /// The electric field, a vector lattice in volts per metre.
    electric = 5,
    /// The drag coefficient, a scalar lattice in per second.
    drag = 6,
};

/// @brief How many slots a batch has: the four state buffers, then one per bindable field.
inline constexpr std::size_t kBatchSlotCount = ParticleState::kSlotCount + kSlotNameCount;

/// @brief Where the first field slot sits -- immediately after the state.
inline constexpr std::size_t kFieldSlotBase = ParticleState::kSlotCount;

/**
 * @brief The array index of a batch slot, for indexing a `BatchView`.
 *
 * @param slot Which slot.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The index this slot occupies in `BatchView::in` and `BatchView::out`
 * @invariant   `slot_index(BatchSlot::position)` is zero and the indices are consecutive
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       particles.executor.the_batch_layout_is_declared_once
 */
[[nodiscard]] constexpr std::size_t slot_index(BatchSlot slot) noexcept {
    return static_cast<std::size_t>(slot);
}

/**
 * @brief The batch index a bound field occupies.
 *
 * @param name Which field.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        `kFieldSlotBase + name`
 * @invariant   The order of `SlotName` is the order of the field slots
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       particles.executor.the_batch_layout_is_declared_once
 */
[[nodiscard]] constexpr std::size_t slot_index(SlotName name) noexcept {
    return kFieldSlotBase + static_cast<std::size_t>(name);
}

// The two halves of the layout, checked against each other rather than trusted. A later edit that inserted a
// state slot without moving the field slots would shift every field an operator reads by one, and the result
// would be a run that integrates a magnetic field as if it were a drag coefficient.
static_assert(slot_index(BatchSlot::position) == static_cast<std::size_t>(ParticleState::Slot::position));
static_assert(slot_index(BatchSlot::velocity) == static_cast<std::size_t>(ParticleState::Slot::velocity));
static_assert(slot_index(BatchSlot::charge_mass) == static_cast<std::size_t>(ParticleState::Slot::charge_mass));
static_assert(slot_index(BatchSlot::status) == static_cast<std::size_t>(ParticleState::Slot::status));
static_assert(kFieldSlotBase == ParticleState::kSlotCount);
static_assert(kBatchSlotCount == 7);
static_assert(slot_index(SlotName::magnetic) == slot_index(BatchSlot::magnetic));
static_assert(slot_index(SlotName::drag) == slot_index(BatchSlot::drag));
static_assert(kBatchSlotCount - kFieldSlotBase == kSlotNameCount);

/**
 * @brief One kernel, prepared, with the fields it reads.
 *
 * ## Why a step declares what it needs
 *
 * `fields` says what a step **has**; `required_slots` says what it **cannot run without**, and the second is not
 * derivable from the first. The executor cannot look at a kernel and know that a Boris push is meaningless
 * without a magnetic field -- `IBatchAdvancer` has no method that answers it, and adding one would put the
 * particles module's vocabulary inside the kernel contract. What *can* answer it is whoever assembled the plan:
 * the code that instantiated the kernel, read its documented slot list and wired a field node to it. That code
 * knows, and `required_slots` is where it says so.
 *
 * The value of asking is `PlanRefusal::slot_unbound`, and the reason it is worth a word in the plan is that the
 * alternative is not a crash. A pusher handed no magnetic field treats it as zero and every particle travels in a
 * straight line; the user sees a run that finished and a picture that is wrong, and there is no way to tell it
 * from a field that is genuinely zero everywhere. A refusal at load time names the missing slot instead.
 *
 * @ownership   observes (the kernel and the field data outlive the step)
 * @thread      main (build, prepare) / eval (advance)
 * @pre         `kernel != nullptr` for a step that can run
 * @post        none
 * @invariant   `param` is the block the kernel was prepared with
 * @errors      noexcept
 * @frozen      no
 * @tests       particles.executor.a_plan_prepares_every_kernel_once,
 *              particles.executor.a_required_slot_that_is_unbound_is_refused
 */
struct StepPlan final {
    /// The kernel to advance with. Not owned: a plugin owns its own implementation and the registry borrows it.
    kernels::IBatchAdvancer* kernel = nullptr;
    /// What the kernel was prepared with, carried so a report can say what the run was configured as.
    kernels::ParamBlock param{};
    /// Per-slot bindings, indexed by `SlotName`. A null `data` means the slot is absent.
    ///
    /// A `FieldValue` rather than a `FieldValue*`, because the data is not owned and a pointer to it would need a
    /// lifetime rule of its own; a copy is four small integers and a pointer.
    field::FieldValue fields[kSlotNameCount]{};
    /// Which slots this step cannot run without, as a mask of `slot_bit`s.
    ///
    /// Set by whoever assembled the plan, because that is the only code that knows: the executor cannot ask a
    /// kernel which fields it needs without putting this module's vocabulary into the kernel contract. `prepare`
    /// refuses the plan with `slot_unbound` when a required slot has no readable binding, so the missing field is
    /// named while the user is still editing the graph rather than discovered from a straight-line trajectory.
    std::uint32_t required_slots = kNoSlotsRequired;

    /**
     * @brief The binding for one slot. A default-constructed view when it is absent.
     *
     * **This accessor exists because there are two index spaces that both look like "the slot number", and they
     * do not agree.** `BatchView::in` is what a *kernel* indexes, and a field sits there at
     * `slot_index(name)` -- 4, 5 and 6, after the state's own slots. `fields[]` above is what the *plan builder*
     * fills, and it is indexed by the enumerator's own value: 0, 1 and 2, because the array holds exactly
     * `kSlotNameCount` entries and nothing else lives in it.
     *
     * Both original callers are right: `BorisAdvancer` reads `batch.in[slot_index(...)]` and the plan builder
     * writes `fields[static_cast<std::size_t>(slot)]`. A **third** caller wrote the first form against the second
     * array, and the machine reported it as one would hope -- the value it produced had `data = 0x0b`, an address
     * eleven bytes into a three-element array of sixteen-byte structs. That read happened to be visibly wrong; the
     * same mistake a few bytes further from the end is a plausible pointer to something else entirely.
     *
     * @param name Which slot.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        The bound view, or an invalid one
     * @invariant   Never reads outside `fields`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_bound_field_reaches_the_kernel
     */
    [[nodiscard]] const field::FieldValue& field(SlotName name) const noexcept;
};

/**
 * @brief Why a plan will not run.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   One code per distinct reason; `ok` is zero
 * @errors      noexcept
 * @frozen      yes -- tags frozen, the set may gain a reason
 * @tests       particles.executor.an_empty_plan_is_refused
 */
enum class PlanRefusal : std::uint8_t {
    ok = 0,
    /// No steps. A plan with nothing in it would "succeed" at doing nothing, which is the answer that hides the
    /// mistake it was asked about.
    empty_plan = 1,
    /// A step holds no kernel.
    step_without_kernel = 2,
    /// A kernel refused the parameter block it was prepared with. Reported by the kernel, not by this layer.
    kernel_refused = 3,
    /// A step **requires** a field the plan does not bind -- a magnetic pusher with no magnetic field.
    ///
    /// Refused rather than defaulted to zero, and that is the decision worth stating: a pusher run with an absent
    /// field does not move the particles, which a user reads as "the simulation is broken" -- there is no way to
    /// tell it from a field that is genuinely zero everywhere. Naming the missing slot is actionable.
    ///
    /// It is the step's own declaration (`StepPlan::required_slots`) that is checked, not a guess: this layer has no
    /// way to know that a Boris push needs a magnetic field, and inventing one -- a field slot that happens to be
    /// bound is a field the step may well not read -- would refuse plans that are perfectly fine.
    slot_unbound = 4,
    /// A step needs scratch and the plan cannot provide it. Reported here rather than left to `advance`, which
    /// must not allocate.
    scratch_unavailable = 5,
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
 * @tests       particles.executor.an_empty_plan_is_refused
 */
[[nodiscard]] const char* to_string(PlanRefusal refusal) noexcept;

/**
 * @brief What one advance did. The numbers a report quotes.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `steps` counts host steps, not sub-steps: a kernel that takes twenty sub-steps inside one
 *              `advance` is one step here, because that is the number of samples the trace will hold
 * @errors      noexcept
 * @frozen      no
 * @tests       particles.executor.a_step_counts_every_kernel_that_ran
 */
struct AdvanceReport final {
    /// Host steps taken since the run began.
    std::size_t steps = 0;
    /// Kernel `advance` calls made since the run began. At least `steps`, more when a plan has several kernels.
    std::size_t kernel_calls = 0;
    /// Values the clamp policy altered, summed over every kernel and every step.
    ///
    /// **The evidence C8's panel reports.** Zero is a real answer meaning "nothing was clamped"; the count only
    /// ever grows, so a run that clamped once and then behaved says so for the rest of its life -- which is what
    /// the reference implementation's `clamps_fired()` is for one layer down.
    std::size_t clamped = 0;
};

/**
 * @brief Drives a plan over a batch, one step at a time.
 *
 * @ownership   owns the scratch and the counters; borrows the state and the kernels
 * @thread      main (build, prepare) / eval (advance)
 * @pre         The bound kernels and field data outlive this object
 * @post        none
 * @invariant   A step never allocates
 * @errors      See each declaration
 * @frozen      no
 * @tests       particles.executor.a_step_counts_every_kernel_that_ran
 */
class ParticleExecutor final {
public:
    /**
     * @brief The clamp policy a run applies unless its caller says otherwise.
     *
     * The default `ClampPolicy` -- finite only, magnitude bound 1e12 -- and the bound is the interesting half:
     * a particle that reaches 1e12 metres has left the solar system and one that reaches 1e12 m/s is not a
     * speed, so both are retired rather than carried. A run that wants the other answer asks for
     * `ClampPolicy::none()` and gets a non-finite value it can report.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   Constant
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_clamp_is_counted_not_hidden
     */
    [[nodiscard]] static constexpr kernels::ClampPolicy default_clamp() noexcept {
        return kernels::ClampPolicy{};
    }

    /**
     * @brief Builds an executor over `state` and `steps`. Nothing runs yet.
     *
     * @param state The batch to advance, **borrowed**: it outlives this object.
     * @param steps The kernels to run, in order. Copied, because a plan is a value and a caller that kept the
     *              vector would be able to change the run under it.
     * @param clamp The policy applied to every component a kernel writes.
     *
     * @ownership   observes `state`, owns the plan's scratch
     * @thread      main
     * @pre         `state` outlives this object
     * @post        `prepared()` is false
     * @invariant   The plan is copied once and never re-read from the caller
     * @errors      noexcept
     * @complexity  O(steps)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_plan_prepares_every_kernel_once
     */
    ParticleExecutor(ParticleState& state, std::vector<StepPlan> steps,
                     kernels::ClampPolicy clamp = default_clamp()) noexcept;

    ParticleExecutor(const ParticleExecutor&) = delete;
    ParticleExecutor& operator=(const ParticleExecutor&) = delete;
    ~ParticleExecutor() = default;

    /**
     * @brief The batch this executor advances.
     *
     * @ownership   borrows
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   The object passed to the constructor
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_step_counts_every_kernel_that_ran
     */
    [[nodiscard]] ParticleState& state() noexcept { return *state_; }

    /**
     * @brief Whether `prepare` has succeeded.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        True after a successful `prepare`, false before it and after a failed one
     * @invariant   `advance` refuses while this is false
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_plan_prepares_every_kernel_once
     */
    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    /**
     * @brief What the run has done so far.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        none
     * @invariant   Monotone in every field
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_step_counts_every_kernel_that_ran
     */
    [[nodiscard]] const AdvanceReport& report() const noexcept { return report_; }

    /**
     * @brief How many steps the plan holds, and therefore how many kernel calls one `advance` makes.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        The number of `StepPlan`s the executor was built with
     * @invariant   Constant for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_step_counts_every_kernel_that_ran
     */
    [[nodiscard]] std::size_t step_count() const noexcept { return steps_.size(); }

    /**
     * @brief The plan's step at `index`, so a caller can assert what a run was configured as.
     *
     * Exposed because the bindings are the part of a plan that cannot be seen from anywhere else: a field
     * reached the executor or it did not, and this is the only way to ask.
     *
     * @param index Which step.
     *
     * @ownership   borrows from this object
     * @thread      main
     * @pre         `index < step_count()`
     * @post        A reference to that step's plan
     * @invariant   Never reads outside the plan
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_bound_field_reaches_the_kernel
     */
    [[nodiscard]] const StepPlan& step(std::size_t index) const noexcept { return steps_[index]; }

    /**
     * @brief Checks the plan and calls `prepare` on every kernel, in order.
     *
     * Called before any step. The checks come first and in the order a reader would ask them, so the refusal names
     * the first thing that is wrong rather than whichever happened to be tested last.
     *
     * **Returns a `PlanRefusal` rather than a `Result`**, and that is a correction rather than a style. The first
     * version returned `diag::Result<void>`, which can carry only an `ErrorCode` -- so `empty_plan`,
     * `step_without_kernel` and `kernel_refused` would all have collapsed into one generic failure, and the
     * caller could no longer say which of them happened. The distinction is the whole reason the codes exist; a
     * refusal that has to be reported by name must not travel in a channel that cannot name it.
     *
     * @ownership   observes
     * @thread      main
     * @pre         none
     * @post        On `ok`, `prepared()` is true and every kernel has been prepared exactly once
     * @invariant   On failure no kernel has been prepared: a half-prepared plan is not a plan
     * @errors      Reports a `PlanRefusal`; `empty_plan`, `step_without_kernel`, `slot_unbound`,
     *              `kernel_refused`
     * @complexity  O(steps)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_plan_prepares_every_kernel_once,
     *              particles.executor.an_empty_plan_is_refused,
     *              particles.executor.a_required_slot_that_is_unbound_is_refused
     */
    [[nodiscard]] PlanRefusal prepare();

    /**
     * @brief Advances the batch by one host step.
     *
     * Each step in the plan runs once, in order, and every component written is put through the clamp policy.
     * Scratch is allocated in `prepare`, never here: `advance` must not allocate.
     *
     * **The batch a kernel is handed has `kBatchSlotCount` slots**: the four state buffers in
     * `ParticleState::Slot` order, then the step's bound fields in `SlotName` order. An unbound field is still a
     * slot and its view is invalid, so a kernel asks `field::is_readable` rather than assuming a shorter array
     * means an absent field -- the array length is a constant of this module, and a kernel that read it as "how
     * many fields are bound" would start reading a drag coefficient as a magnetic field.
     *
     * @param ctx The step context. Only `dt` is read by this layer; `step`, `seed`, `rng` and `scratch` are
     *            filled in from the run's own bookkeeping and handed to each kernel, so a kernel that is
     *            stochastic gets the stream the run declared rather than one it invented.
     *
     * @ownership   observes
     * @thread      eval
     * @pre         `prepared()` and `state().count() > 0`
     * @post        On success every live particle holds its state after one step and `report().steps` is one
     *              larger
     * @invariant   No allocation, no throw, and a kernel's refusal leaves the counters as they were
     * @errors      `invalid_argument` for a zero or non-finite `dt`; the kernel's own code otherwise
     * @complexity  O(steps * particles)
     * @nondet      only through the kernel, and only through `ctx`
     * @frozen      no
     * @tests       particles.executor.a_step_counts_every_kernel_that_ran,
     *              particles.executor.a_self_inverse_kernel_round_trips,
     *              particles.executor.a_kernel_reads_the_slots_it_was_told_about,
     *              particles.executor.the_batch_layout_is_declared_once,
     *              particles.executor.an_empty_plan_is_refused
     */
    [[nodiscard]] diag::Result<void> advance(kernels::AdvanceContext& ctx);

    /**
     * @brief Forgets the counters, so a second run is reported as its own.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        `report()` is default-constructed
     * @invariant   The prepared state is untouched: a run that is restarted does not need preparing again
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       particles.executor.a_step_counts_every_kernel_that_ran
     */
    void reset_report() noexcept { report_ = AdvanceReport{}; }

private:
    /// @brief Fills the state half of `out` from `state`, in `ParticleState::Slot` order.
    ///
    /// The order is the contract between this file and the state: a kernel reads `batch.in[i]` knowing that `i`
    /// is a slot, so the two must agree. There is one function that builds this array and one that reads it,
    /// which is the only reason the agreement is checkable.
    static void state_slots_of(ParticleState& state,
                               field::FieldValue (&out)[kBatchSlotCount]) noexcept;

    /// @brief Fills the field half of `out` from a step's bindings, in `SlotName` order.
    ///
    /// An unbound slot is written as a **default-constructed** `FieldValue`, which is not readable: the array
    /// length never changes, so "absent" has to be said by the view rather than by the count.
    static void field_slots_of(const StepPlan& step, field::FieldValue (&out)[kBatchSlotCount]) noexcept;

    /// @brief Puts every component a kernel wrote through the clamp policy, counting what changed and retiring
    /// any particle whose arithmetic stopped being a number.
    void clamp_state(ParticleState& state) noexcept;

    ParticleState* state_;
    std::vector<StepPlan> steps_;
    kernels::ClampPolicy clamp_{};
    AdvanceReport report_{};
    bool prepared_ = false;

    /// The output slots for a kernel that is not in place, and the scratch the kernels asked for. Both are
    /// allocated in `prepare`, because `advance` must not.
    ParticleState out_;
    std::vector<double> scratch_;
    std::size_t scratch_bytes_ = 0;
};

}  // namespace qp::graph::particles
