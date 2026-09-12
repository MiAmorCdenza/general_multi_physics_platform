/**
 * @file test_particles.cpp
 * @brief The bridge: a plan of kernels driven over a batch, checked with kernels whose answers are known.
 *
 * Test case ids match the @tests fields in `core/graph/particles`.
 *
 * ## Why every kernel here is a stub
 *
 * Because the thing under test is the **loop**, not the physics. A kernel with a closed-form answer -- identity,
 * a constant acceleration, a scheme that is exactly reversible -- turns each property of the executor into an
 * assertion that a wrong loop would fail and a wrong integrator could not. That separation is the whole reason
 * this module is its own directory: if the first test of the bridge had been "does Boris conserve energy", a
 * failure would have had two possible causes and no way to tell them apart.
 *
 * ## The ways this loop could be silently wrong, and the case for each
 *
 *   1. **The slot order.** A kernel that reads index 2 as the charge-to-mass ratio and finds the status there
 *      computes a plausible acceleration from a state code. `a_kernel_reads_the_slots_it_was_told_about` asserts
 *      the order through the one thing that can see it: the numbers a kernel computes.
 *   2. **In-place versus out-of-place.** A non-in-place kernel reads `in` while writing `out`; handing it the
 *      same slots makes the answer depend on the order of the reads inside the kernel.
 *      `in_place_and_out_of_place_agree` runs one plan both ways.
 *   3. **The clamp.** C2 says a run never explodes, and the project says the platform never clamps silently.
 *      Both are asserted, including the case that matters most: a NaN compares false against every bound, so a
 *      clamp written in the obvious order lets it through.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/particles/executor.hpp>
#include <qp/graph/particles/particle_state.hpp>

#include <qp/graph/kernels/kernel.hpp>

#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace qp::graph::particles;

namespace {

namespace pk = qp::graph::kernels;
namespace gfield = qp::graph::field;

/// @brief A kernel whose behaviour is a lambda, so each case can state its own.
///
/// A test fixture rather than a mock library: the interface is six methods and a scheme that needed a seventh
/// would be a scheme the platform cannot drive.
class StubKernel final : public pk::IBatchAdvancer {
public:
    /// What one `advance` does to the batch.
    using Body = std::function<qp::diag::Result<void>(const pk::BatchView&, pk::AdvanceContext&)>;

    StubKernel(std::string name, pk::Capability caps, Body body)
        : name_(std::move(name)), caps_(caps), body_(std::move(body)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] pk::Capability capabilities() const noexcept override { return caps_; }

    [[nodiscard]] qp::diag::Result<void> prepare(const pk::ParamBlock& params) override {
        ++prepares;
        if (refuse_prepare) return qp::diag::ErrorCode::invalid_argument;
        last_params = params;
        return {};
    }

    [[nodiscard]] qp::diag::Result<void> advance(const pk::BatchView& batch,
                                                 pk::AdvanceContext& ctx) override {
        ++advances;
        last_ctx_dt = ctx.dt;
        last_ctx_step = ctx.step;
        last_batch_count = batch.count;
        return body_(batch, ctx);
    }

    /// How many times `prepare` was called. The plan's contract is exactly once per kernel.
    int prepares = 0;
    /// How many times `advance` was called.
    int advances = 0;
    /// Whether `prepare` should refuse, so the plan can be seen to come up short.
    bool refuse_prepare = false;
    /// The block the kernel was last prepared with, so a case can assert what was handed over.
    pk::ParamBlock last_params{};
    /// What the last `advance` was told. The loop owns these, so a case asserts them.
    double last_ctx_dt = 0.0;
    std::uint64_t last_ctx_step = 0;
    std::size_t last_batch_count = 0;

private:
    std::string name_;
    pk::Capability caps_;
    Body body_;
};

/// @brief Whether an in-place kernel may read and write the same buffers.
constexpr pk::Capability kInPlace = pk::Capability::is_in_place;

/**
 * @brief A vector field bound to `values`, with `count` points of three components.
 *
 * The values must outlive the plan: a `FieldValue` is a description and a pointer, and it owns nothing. Every
 * case therefore owns its array and binds afterwards, which is the same discipline a caller in the tree follows.
 */
[[nodiscard]] gfield::FieldValue vector_field(const double* values, std::size_t count) {
    gfield::FieldValue out;
    out.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::vector,
                                     qp::abi::ElementType::f64, qp::abi::FieldDim{},
                                     static_cast<std::uint32_t>(count));
    out.data = values;
    out.bytes = qp::abi::data_bytes(out.desc);
    return out;
}

/// @brief A scalar field bound to `values`, with `count` points.
[[nodiscard]] gfield::FieldValue scalar_field(const double* values, std::size_t count) {
    gfield::FieldValue out;
    out.desc = qp::abi::make_lattice(qp::abi::LatticeKind::line, qp::abi::ComponentKind::scalar,
                                     qp::abi::ElementType::f64, qp::abi::FieldDim{},
                                     static_cast<std::uint32_t>(count));
    out.data = values;
    out.bytes = qp::abi::data_bytes(out.desc);
    return out;
}

/// @brief The raw slot array a kernel is handed, for a kernel body that wants to write into it.
[[nodiscard]] double* mutable_slot(const pk::BatchView& batch, std::size_t slot) {
    return const_cast<double*>(static_cast<const double*>(batch.out[slot].data));
}

/// @brief The raw input slot array, for a body that wants to read what it was given.
[[nodiscard]] const double* readable_slot(const pk::BatchView& batch, std::size_t slot) {
    return static_cast<const double*>(batch.in[slot].data);
}

/// @brief A batch of `n` particles at the origin, all live, with a charge-to-mass ratio of `qm`.
[[nodiscard]] ParticleState batch_of(std::size_t n, double qm = 0.0) {
    ParticleState state{n};
    for (std::size_t i = 0; i < n; ++i) state.set(i, 0, ParticleState::Slot::charge_mass, qm);
    return state;
}

}  // namespace

TEST_CASE("particles.state.layout_is_declared_not_assumed", "[particles]") {
    // A batch's four slots are the contract between the executor and every kernel, so each one is asserted for
    // what it **describes** rather than only for existing: a velocity slot described as a length would make a
    // kernel that trusts the description integrate the wrong quantity.
    ParticleState state{4};
    REQUIRE(state.count() == 4);
    REQUIRE(state.is_consistent());

    const gfield::FieldValue position = state.slot(ParticleState::Slot::position);
    REQUIRE(gfield::is_readable(position));
    REQUIRE(position.is_vector());
    REQUIRE(position.point_count() == 4);
    REQUIRE(position.desc.element == qp::abi::ElementType::f64);
    REQUIRE(position.desc.dimension.L == 1);
    REQUIRE(position.desc.dimension.T == 0);

    const gfield::FieldValue velocity = state.slot(ParticleState::Slot::velocity);
    REQUIRE(gfield::is_readable(velocity));
    REQUIRE(velocity.is_vector());
    // A velocity is a length over a time, and the description has to say so: the two slots differ in exactly this
    // exponent, and a slot table with a copy-paste error would be invisible without this assertion.
    REQUIRE(velocity.desc.dimension.L == 1);
    REQUIRE(velocity.desc.dimension.T == -1);
    REQUIRE(velocity.desc.dimension.T != position.desc.dimension.T);

    const gfield::FieldValue qm = state.slot(ParticleState::Slot::charge_mass);
    REQUIRE(gfield::is_readable(qm));
    REQUIRE(qm.is_scalar());
    REQUIRE(qm.desc.dimension.I == 1);
    REQUIRE(qm.desc.dimension.T == 1);
    REQUIRE(qm.desc.dimension.M == -1);

    const gfield::FieldValue status = state.slot(ParticleState::Slot::status);
    REQUIRE(gfield::is_readable(status));
    REQUIRE(status.is_scalar());
    // Dimensionless, not given a made-up unit: `is_valid_field` is entitled to be asked what a buffer holds and
    // to get a true answer, and a status code with a dimension would be a quantity.
    REQUIRE(status.desc.dimension.L == 0);
    REQUIRE(status.desc.dimension.M == 0);
    REQUIRE(status.desc.dimension.T == 0);
    REQUIRE(status.desc.dimension.I == 0);

    // An empty batch has **no** valid description: `abi::is_consistent` refuses a line lattice with a zero count,
    // so the four views are invalid and every caller is stopped before a kernel sees them. That is the ABI's rule
    // rather than a choice, and asserting it here is what keeps "zero particles" from reading as a valid field.
    const ParticleState empty{};
    REQUIRE(empty.count() == 0);
    REQUIRE(empty.is_consistent());
    REQUIRE_FALSE(gfield::is_readable(empty.slot(ParticleState::Slot::position)));
}

TEST_CASE("particles.state.slots_are_double_precision", "[particles]") {
    // The decision the platform's owner took rather than this module: a Boris push accumulates hundreds of
    // thousands of steps, and the conservation checks that say whether it is still integrating the right equation
    // are differences between numbers of order one. In f32 those differences are the same order as the rounding,
    // which would leave "energy is conserved" and "energy drifts by the rounding" indistinguishable.
    //
    // Asserted rather than commented, because the field module's default is f32 and a later "consistency" edit
    // that switched these four slots to the default would be invisible in a diff.
    ParticleState state{2};
    for (const ParticleState::Slot which :
         {ParticleState::Slot::position, ParticleState::Slot::velocity, ParticleState::Slot::charge_mass,
          ParticleState::Slot::status}) {
        const gfield::FieldValue view = state.slot(which);
        REQUIRE(view.desc.element == qp::abi::ElementType::f64);
        REQUIRE(view.desc.spacing_bytes == qp::abi::expected_spacing(view.desc));
    }

    // And the precision is real: a value f32 cannot hold exactly survives the round trip through the slot.
    const double precise = 1.0 + 1.0e-12;
    state.set(0, 0, ParticleState::Slot::position, precise);
    REQUIRE(state.at(0, 0, ParticleState::Slot::position) == precise);
    REQUIRE(static_cast<float>(precise) == static_cast<float>(1.0));
}

TEST_CASE("particles.state.a_write_is_visible_through_the_view", "[particles]") {
    // The two access paths -- the direct `at`/`set` and the `FieldValue` a kernel reads -- must be the same
    // memory, or a kernel and a test would be looking at different batches. This is the case that would catch a
    // slot table describing one array while the accessor read another.
    ParticleState state{3};
    state.set(1, 2, ParticleState::Slot::velocity, 7.5);
    REQUIRE(state.at(1, 2, ParticleState::Slot::velocity) == 7.5);

    const gfield::FieldValue velocity = state.slot(ParticleState::Slot::velocity);
    // Component 2 of point 1, in a vector field: three components per point.
    REQUIRE(gfield::get_component(velocity, 1, 2) == 7.5);
    REQUIRE(gfield::get_component(velocity, 1, 0) == 0.0);
    REQUIRE(gfield::get_component(velocity, 2, 2) == 0.0);

    // Out-of-range access is total rather than fatal, in both directions: a caller with a layout mismatch wants a
    // zero that flows through, not a termination inside a step loop.
    REQUIRE(state.at(99, 0, ParticleState::Slot::position) == 0.0);
    REQUIRE(state.at(0, 99, ParticleState::Slot::position) == 0.0);
    state.set(99, 0, ParticleState::Slot::position, 1.0);
    state.set(0, 99, ParticleState::Slot::position, 1.0);
    REQUIRE(state.at(0, 0, ParticleState::Slot::position) == 0.0);

    // A resize discards what was there, and the statuses more visibly than the rest: a reallocation that kept the
    // previous run's `absorbed` flags would make the new batch start half dead, and the count a report quotes
    // would be about particles that no longer exist.
    state.set_status(0, Status::absorbed);
    state.resize(3);
    REQUIRE(state.live_count() == 3);
    REQUIRE(state.at(1, 2, ParticleState::Slot::velocity) == 0.0);
}

TEST_CASE("particles.state.status_round_trips_through_the_buffer", "[particles]") {
    // Status is a number in a double slot, and the count a report quotes depends on it. The three states are
    // distinct findings -- absorbed by the body, escaped the region, still running -- and a report that merged
    // them could not say which happened, which is the same defect as reporting an unknown uncertainty as zero.
    ParticleState state{5};
    REQUIRE(state.live_count() == 5);
    REQUIRE(state.count_with(Status::live) == 5);
    REQUIRE(state.count_with(Status::absorbed) == 0);

    state.set_status(0, Status::absorbed);
    state.set_status(1, Status::escaped);
    REQUIRE(state.live_count() == 3);
    REQUIRE(state.count_with(Status::absorbed) == 1);
    REQUIRE(state.count_with(Status::escaped) == 1);
    REQUIRE(state.count_with(Status::live) + state.count_with(Status::absorbed) +
                state.count_with(Status::escaped) ==
            state.count());

    REQUIRE(std::string{to_string(Status::live)} == "live");
    REQUIRE(std::string{to_string(Status::absorbed)} == "absorbed");
    REQUIRE(std::string{to_string(Status::escaped)} == "escaped");
    REQUIRE(state.status_of(0) == Status::absorbed);
    REQUIRE(state.status_of(1) == Status::escaped);

    // An out-of-range read is `escaped`, not `live`: a particle that does not exist is not one being integrated,
    // and answering `live` would let a loop that ran past the end keep stepping a phantom that looks healthy.
    REQUIRE(state.status_of(99) == Status::escaped);

    // A status outside the three is treated as `escaped` for the same reason: the only way to write one is a
    // corrupt buffer or a kernel writing into a slot it does not own.
    state.set(2, 0, ParticleState::Slot::status, 42.0);
    REQUIRE(state.status_of(2) == Status::escaped);
}

TEST_CASE("particles.executor.a_plan_prepares_every_kernel_once", "[particles]") {
    // `prepare` runs once per kernel, in order, **before** the first step, and that is the interface's own
    // requirement rather than this loop's preference: it is where a kernel validates its parameters ("while the
    // user is still editing the graph") and the only place the scratch requirement can be honoured, because
    // `advance` must not allocate.
    ParticleState state = batch_of(4);
    StubKernel first{"first", pk::Capability::none,
                     [](const pk::BatchView&, pk::AdvanceContext&) { return qp::diag::Result<void>{}; }};
    StubKernel second{"second", pk::Capability::none,
                      [](const pk::BatchView&, pk::AdvanceContext&) { return qp::diag::Result<void>{}; }};

    pk::ParamBlock params;
    params.set_real(0, 0.25);
    params.set_integer(0, 7);

    std::vector<StepPlan> steps(2);
    steps[0].kernel = &first;
    steps[0].param = params;
    steps[1].kernel = &second;

    ParticleExecutor executor{state, std::move(steps)};
    REQUIRE_FALSE(executor.prepared());
    REQUIRE(first.prepares == 0);

    REQUIRE(executor.prepare() == PlanRefusal::ok);
    REQUIRE(executor.prepared());
    REQUIRE(first.prepares == 1);
    REQUIRE(second.prepares == 1);
    // The block reached the kernel, which is the half a count cannot show: a loop that prepared with a default
    // block would pass the count assertion and hand every kernel zero for every parameter.
    REQUIRE(first.last_params.real(0) == 0.25);
    REQUIRE(first.last_params.integer(0) == 7);

    // Nothing has stepped, which is what "prepare is not advance" means.
    REQUIRE(executor.report().steps == 0);
    REQUIRE(first.advances == 0);

    // A kernel that refuses takes the whole plan down, and the refusal means **no** kernel was prepared -- a
    // half-prepared plan is not a plan, and a caller that stepped it would be running one kernel of two.
    ParticleState other = batch_of(2);
    StubKernel refusing{"refusing", pk::Capability::none,
                        [](const pk::BatchView&, pk::AdvanceContext&) { return qp::diag::Result<void>{}; }};
    refusing.refuse_prepare = true;
    std::vector<StepPlan> bad(1);
    bad[0].kernel = &refusing;
    ParticleExecutor refused{other, std::move(bad)};
    REQUIRE(refused.prepare() == PlanRefusal::kernel_refused);
    REQUIRE_FALSE(refused.prepared());
}

TEST_CASE("particles.executor.an_empty_plan_is_refused", "[particles]") {
    // A plan with nothing in it would "succeed" at doing nothing, which is the answer that hides the mistake it
    // was asked about. Same for a batch with no particles: its slots have no valid description, so every step
    // would be handed an invalid `BatchView` and each kernel would have to refuse it -- refusing once, here,
    // names the actual problem instead of a kernel's complaint about a buffer it should never have been shown.
    ParticleState state = batch_of(3);
    StubKernel kernel{"k", pk::Capability::none,
                      [](const pk::BatchView&, pk::AdvanceContext&) { return qp::diag::Result<void>{}; }};

    ParticleExecutor no_steps{state, {}};
    REQUIRE(no_steps.prepare() == PlanRefusal::empty_plan);

    ParticleState empty_batch;
    std::vector<StepPlan> steps(1);
    steps[0].kernel = &kernel;
    ParticleExecutor nothing{empty_batch, std::move(steps)};
    REQUIRE(nothing.prepare() == PlanRefusal::empty_plan);

    // A step with no kernel is refused before any kernel is prepared.
    std::vector<StepPlan> missing(1);
    ParticleExecutor unbound{state, std::move(missing)};
    REQUIRE(unbound.prepare() == PlanRefusal::step_without_kernel);
    REQUIRE(kernel.prepares == 0);

    // And stepping before preparing is refused rather than run: the kernels have not validated their parameters
    // and the scratch does not exist, so running would be a step through an interface nobody finished setting up.
    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    REQUIRE_FALSE(unbound.advance(ctx).has_value());
    REQUIRE(kernel.advances == 0);

    REQUIRE(std::string{to_string(PlanRefusal::empty_plan)} == "empty_plan");
    REQUIRE(std::string{to_string(PlanRefusal::step_without_kernel)} == "step_without_kernel");
    REQUIRE(std::string{to_string(SlotName::magnetic)} == "magnetic");
    REQUIRE(std::string{to_string(SlotName::electric)} == "electric");
    REQUIRE(std::string{to_string(SlotName::drag)} == "drag");
}

TEST_CASE("particles.executor.a_kernel_reads_the_slots_it_was_told_about", "[particles]") {
    // **The case that pins the slot order.** A kernel told that index 2 is the charge-to-mass ratio computes an
    // acceleration from it; if this module put the status there instead, the arithmetic would produce numbers
    // that look like accelerations and the run would be wrong in a way no test of the kernel alone could see.
    //
    // The kernel here writes `position += charge_mass * dt` on component 0 and `position += velocity * dt` on
    // component 1, so both readings are visible in the result and either one being the wrong array shows up as a
    // wrong number rather than as an exception.
    // Particle 0 carries a charge-to-mass ratio and particle 0 a velocity -- the **same** particle, because this
    // case is about the slot order and not about indexing. The two numbers differ (3 and 5) so that reading the
    // wrong slot gives a wrong answer rather than a coincidence.
    ParticleState state = batch_of(2);
    state.set(0, 0, ParticleState::Slot::charge_mass, 3.0);
    state.set(0, 0, ParticleState::Slot::velocity, 5.0);

    StubKernel probe{"probe", kInPlace, [](const pk::BatchView& batch, pk::AdvanceContext& ctx) {
        const double* qm = readable_slot(batch, 2);
        const double* velocity = readable_slot(batch, 1);
        double* position = mutable_slot(batch, 0);
        // Element 0 of each vector field is particle 0's x. Under the wrong slot order `qm[0]` would be a status
        // code and `velocity[0]` a charge-to-mass ratio, and both results would be visibly different numbers.
        position[0] = qm[0] * ctx.dt;
        position[1] = velocity[0] * ctx.dt;
        return qp::diag::Result<void>{};
    }};

    std::vector<StepPlan> steps(1);
    steps[0].kernel = &probe;
    ParticleExecutor executor{state, std::move(steps)};
    REQUIRE(executor.prepare() == PlanRefusal::ok);

    pk::AdvanceContext ctx;
    ctx.dt = 2.0;
    REQUIRE(executor.advance(ctx).has_value());

    REQUIRE(state.at(0, 0, ParticleState::Slot::position) == 6.0);
    REQUIRE(state.at(0, 1, ParticleState::Slot::position) == 10.0);
    // The loop filled in the counters it owns, rather than trusting the caller: `step` is how many steps this
    // executor has taken, and a kernel handed a stale counter would take a different branch and, if stochastic,
    // draw a different stream.
    REQUIRE(probe.last_ctx_step == 0);
    REQUIRE(probe.last_ctx_dt == 2.0);
    // And the batch was described as an array of **slots**, not of particles: a count of 2 here would tell the
    // kernel to read past the end of the slot array. It is now the full layout -- four state slots plus one per
    // bindable field, whether or not this plan bound any -- because a kernel indexes the field it wants.
    REQUIRE(probe.last_batch_count == kBatchSlotCount);
}

TEST_CASE("particles.executor.a_bound_field_reaches_the_kernel", "[particles]") {
    // A field slot is how a force gets to a step, and this is the first time in this repository that one is
    // actually carried: `BatchView`'s field vocabulary was written for exactly this and had no caller at all. A
    // binding that did not arrive shows up as an absent view rather than as a zero field, which is the
    // distinction the plan's refusal codes name -- a pusher run with no magnetic field does not move anything,
    // and that is indistinguishable from a field that is genuinely zero everywhere.
    ParticleState state = batch_of(2);
    const double magnetic[6] = {1.0, 2.0, 3.0, -1.0, -2.0, -3.0};

    StubKernel reader{"reader", kInPlace, [](const pk::BatchView&, pk::AdvanceContext&) {
                          return qp::diag::Result<void>{};
                      }};

    std::vector<StepPlan> steps(1);
    steps[0].kernel = &reader;
    steps[0].fields[static_cast<std::size_t>(SlotName::magnetic)] = vector_field(magnetic, 2);

    ParticleExecutor executor{state, std::move(steps)};
    REQUIRE(executor.prepare() == PlanRefusal::ok);
    REQUIRE(executor.step_count() == 1);

    // The binding survived into the plan the executor holds, and carries what was put there. A loop that dropped
    // the fields on the way in would pass every other case in this file.
    const gfield::FieldValue bound = executor.step(0).field(SlotName::magnetic);
    REQUIRE(gfield::is_readable(bound));
    REQUIRE(bound.is_vector());
    REQUIRE(bound.point_count() == 2);
    REQUIRE(gfield::get_component(bound, 0, 0) == 1.0);
    REQUIRE(gfield::get_component(bound, 0, 2) == 3.0);
    REQUIRE(gfield::get_component(bound, 1, 1) == -2.0);

    // An unbound slot is **absent**, not zero-valued: `is_readable` is false, so a caller can tell "no electric
    // field was bound" from "an electric field of zero". Those are the same force and different experiments.
    const StepPlan unbound{};
    REQUIRE_FALSE(gfield::is_readable(unbound.field(SlotName::electric)));
    REQUIRE_FALSE(gfield::is_readable(unbound.field(SlotName::drag)));
    REQUIRE_FALSE(gfield::is_readable(unbound.field(SlotName::magnetic)));

    // A scalar slot holds a drag coefficient, and its shape is asserted because a kernel sampling it by hand
    // needs to know whether it has one or three components per point.
    const double drag[2] = {0.5, 0.25};
    std::vector<StepPlan> scalar_steps(1);
    scalar_steps[0].kernel = &reader;
    scalar_steps[0].fields[static_cast<std::size_t>(SlotName::drag)] = scalar_field(drag, 2);
    ParticleExecutor scalar_plan{state, std::move(scalar_steps)};
    REQUIRE(scalar_plan.prepare() == PlanRefusal::ok);
    const gfield::FieldValue drag_view = scalar_plan.step(0).field(SlotName::drag);
    REQUIRE(gfield::is_readable(drag_view));
    REQUIRE(drag_view.is_scalar());
    REQUIRE(gfield::get_component(drag_view, 1, 0) == 0.25);
    // A scalar slot has no second component, and asking for one is zero rather than a read past the end.
    REQUIRE(gfield::get_component(drag_view, 1, 1) == 0.0);
}

TEST_CASE("particles.executor.the_batch_layout_is_declared_once", "[particles]") {
    // The array a kernel is handed has a **fixed** length: four state buffers, then one slot per bindable field,
    // bound or not. The alternative -- a shorter array meaning "fewer fields are bound" -- is the defect this case
    // exists to refuse: a kernel that read the length as "how many fields are bound" would read a drag
    // coefficient as a magnetic field the first time somebody wired one up, and the arithmetic would still look
    // like physics.
    //
    // The layout is a compile-time fact, so the constants are asserted at compile time. A `REQUIRE` on them would
    // pass just as well and would only be checked when the case runs; `STATIC_REQUIRE` fails the build, which is
    // where a shifted slot index belongs -- the alternative is a run that integrates the wrong quantity.
    STATIC_REQUIRE(slot_index(BatchSlot::position) == 0);
    STATIC_REQUIRE(slot_index(BatchSlot::velocity) == 1);
    STATIC_REQUIRE(slot_index(BatchSlot::charge_mass) == 2);
    STATIC_REQUIRE(slot_index(BatchSlot::status) == 3);
    STATIC_REQUIRE(kFieldSlotBase == 4);
    STATIC_REQUIRE(kBatchSlotCount == 7);
    // The two halves of the layout agree: `ParticleState::Slot` names the same four buffers in the same order, and
    // `SlotName` names the three fields in the same order as the field half of `BatchSlot`.
    STATIC_REQUIRE(slot_index(BatchSlot::position) == static_cast<std::size_t>(ParticleState::Slot::position));
    STATIC_REQUIRE(slot_index(BatchSlot::status) == static_cast<std::size_t>(ParticleState::Slot::status));
    STATIC_REQUIRE(slot_index(SlotName::magnetic) == slot_index(BatchSlot::magnetic));
    STATIC_REQUIRE(slot_index(SlotName::electric) == slot_index(BatchSlot::electric));
    STATIC_REQUIRE(slot_index(SlotName::drag) == slot_index(BatchSlot::drag));
    // And the mask: one bit per slot, distinct, at the position of the enumerator.
    STATIC_REQUIRE(slot_bit(SlotName::magnetic) == 1U);
    STATIC_REQUIRE(slot_bit(SlotName::electric) == 2U);
    STATIC_REQUIRE(slot_bit(SlotName::drag) == 4U);
    STATIC_REQUIRE((slot_bit(SlotName::magnetic) & slot_bit(SlotName::drag)) == 0U);

    ParticleState state = batch_of(3);
    const double magnetic[9] = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0};
    const double drag[3] = {0.5, 0.25, 0.125};

    // What a kernel saw, recorded by the kernel. Readability per slot rather than only the count, because
    // "the array is seven long" and "slot 5 is an absent field" are two different facts and the second is the one
    // a kernel acts on.
    bool magnetic_readable = false;
    bool electric_readable = false;
    bool drag_readable = false;
    double magnetic_x = 0.0;
    double drag_0 = 0.0;
    std::size_t seen_count = 0;

    StubKernel probe{"probe", kInPlace, [&](const pk::BatchView& batch, pk::AdvanceContext&) {
        seen_count = batch.count;
        magnetic_readable = gfield::is_readable(batch.in[slot_index(SlotName::magnetic)]);
        electric_readable = gfield::is_readable(batch.in[slot_index(SlotName::electric)]);
        drag_readable = gfield::is_readable(batch.in[slot_index(SlotName::drag)]);
        magnetic_x = gfield::get_component(batch.in[slot_index(SlotName::magnetic)], 0, 0);
        drag_0 = gfield::get_component(batch.in[slot_index(SlotName::drag)], 0, 0);
        return qp::diag::Result<void>{};
    }};

    std::vector<StepPlan> steps(1);
    steps[0].kernel = &probe;
    steps[0].fields[static_cast<std::size_t>(SlotName::magnetic)] = vector_field(magnetic, 3);
    steps[0].fields[static_cast<std::size_t>(SlotName::drag)] = scalar_field(drag, 3);

    ParticleExecutor executor{state, std::move(steps)};
    REQUIRE(executor.prepare() == PlanRefusal::ok);
    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    REQUIRE(executor.advance(ctx).has_value());

    REQUIRE(seen_count == kBatchSlotCount);
    REQUIRE(magnetic_readable);
    REQUIRE(drag_readable);
    // The slot that was **not** bound is still a slot, and it is unreadable rather than zero. That is the whole
    // point: "no electric field was wired up" and "an electric field of zero" are the same force and different
    // experiments, and only the view can tell them apart.
    REQUIRE_FALSE(electric_readable);
    REQUIRE(magnetic_x == 1.0);
    REQUIRE(drag_0 == 0.5);

    // And the length does not depend on how many fields are bound: the same plan with nothing bound still hands
    // the kernel seven slots, so a kernel's index arithmetic is the same in every run.
    StubKernel bare{"bare", kInPlace, [&](const pk::BatchView& batch, pk::AdvanceContext&) {
                        seen_count = batch.count;
                        magnetic_readable = gfield::is_readable(batch.in[slot_index(SlotName::magnetic)]);
                        return qp::diag::Result<void>{};
                    }};
    std::vector<StepPlan> bare_steps(1);
    bare_steps[0].kernel = &bare;
    ParticleExecutor bare_executor{state, std::move(bare_steps)};
    REQUIRE(bare_executor.prepare() == PlanRefusal::ok);
    REQUIRE(bare_executor.advance(ctx).has_value());
    REQUIRE(seen_count == kBatchSlotCount);
    REQUIRE_FALSE(magnetic_readable);
}

TEST_CASE("particles.executor.a_required_slot_that_is_unbound_is_refused", "[particles]") {
    // `slot_unbound` is the refusal that a pusher with no magnetic field needs and that nothing could produce
    // until a step could **declare** what it reads. It is not derivable from the bindings: an unbound slot is a
    // legitimate state for a kernel that does not read it, and a plan that refused every unbound slot would
    // refuse every plan that has no electric field -- which is every plan this kit ships.
    //
    // The failure it prevents is the quiet one. A Boris push handed no magnetic field treats it as zero, every
    // particle travels in a straight line, and the run finishes with a picture that is wrong and nothing that says
    // so. That is indistinguishable from a field that is genuinely zero everywhere.
    ParticleState state = batch_of(2);
    const double magnetic[6] = {1.0e-5, 0.0, 0.0, 0.0, 1.0e-5, 0.0};
    const double drag[2] = {0.5, 0.25};

    StubKernel pusher{"pusher", kInPlace,
                      [](const pk::BatchView&, pk::AdvanceContext&) { return qp::diag::Result<void>{}; }};

    // Required, not bound: refused, and **before** any kernel was prepared -- a half-prepared plan is not a plan.
    std::vector<StepPlan> missing(1);
    missing[0].kernel = &pusher;
    missing[0].required_slots = slot_bit(SlotName::magnetic);
    ParticleExecutor unbound{state, std::move(missing)};
    REQUIRE(unbound.prepare() == PlanRefusal::slot_unbound);
    REQUIRE_FALSE(unbound.prepared());
    REQUIRE(pusher.prepares == 0);

    // A description with no data is not a binding either. This is the case a check written as "is the descriptor
    // consistent" would pass: the lattice is well formed, the pointer is null.
    std::vector<StepPlan> dangling(1);
    dangling[0].kernel = &pusher;
    dangling[0].required_slots = slot_bit(SlotName::magnetic);
    dangling[0].fields[static_cast<std::size_t>(SlotName::magnetic)] = vector_field(nullptr, 2);
    ParticleExecutor described_only{state, std::move(dangling)};
    REQUIRE(described_only.prepare() == PlanRefusal::slot_unbound);
    REQUIRE(pusher.prepares == 0);

    // A **step with no kernel** is refused first, whatever else the plan is missing: the checks run in the order a
    // reader would ask them, so the refusal names the first thing that is wrong rather than the last one tested.
    std::vector<StepPlan> both(1);
    both[0].required_slots = slot_bit(SlotName::magnetic);
    ParticleExecutor ambiguous{state, std::move(both)};
    REQUIRE(ambiguous.prepare() == PlanRefusal::step_without_kernel);

    // Required and bound: the plan comes up, and the kernel is prepared exactly once.
    std::vector<StepPlan> wired(1);
    wired[0].kernel = &pusher;
    wired[0].required_slots = slot_bit(SlotName::magnetic) | slot_bit(SlotName::drag);
    wired[0].fields[static_cast<std::size_t>(SlotName::magnetic)] = vector_field(magnetic, 2);
    wired[0].fields[static_cast<std::size_t>(SlotName::drag)] = scalar_field(drag, 2);
    ParticleExecutor bound{state, std::move(wired)};
    REQUIRE(bound.prepare() == PlanRefusal::ok);
    REQUIRE(pusher.prepares == 1);

    // Nothing required and nothing bound: **also** fine. A step that reads no field is not a step missing one, and
    // this half of the case is what keeps the check from becoming "every slot must be bound".
    std::vector<StepPlan> plain(1);
    plain[0].kernel = &pusher;
    ParticleExecutor bare{state, std::move(plain)};
    REQUIRE(bare.prepare() == PlanRefusal::ok);
    REQUIRE(pusher.prepares == 2);

    REQUIRE(std::string{to_string(PlanRefusal::slot_unbound)} == "slot_unbound");
}

TEST_CASE("particles.executor.a_step_counts_every_kernel_that_ran", "[particles]") {
    // The counters a report quotes: host steps, kernel calls, and clamps. `kernel_calls` is at least `steps` and
    // more when a plan has several kernels, and the two are separate because a user asking "how far did this get"
    // means samples, while a diagnostic asking "what ran" means calls.
    ParticleState state = batch_of(4);
    StubKernel first{"first", kInPlace,
                     [](const pk::BatchView&, pk::AdvanceContext&) { return qp::diag::Result<void>{}; }};
    StubKernel second{"second", kInPlace,
                      [](const pk::BatchView&, pk::AdvanceContext&) { return qp::diag::Result<void>{}; }};

    std::vector<StepPlan> steps(2);
    steps[0].kernel = &first;
    steps[1].kernel = &second;
    ParticleExecutor executor{state, std::move(steps)};
    REQUIRE(executor.prepare() == PlanRefusal::ok);

    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    for (int i = 0; i < 5; ++i) REQUIRE(executor.advance(ctx).has_value());

    REQUIRE(executor.report().steps == 5);
    REQUIRE(executor.report().kernel_calls == 10);
    REQUIRE(executor.report().clamped == 0);
    REQUIRE(first.advances == 5);
    REQUIRE(second.advances == 5);

    // The step index a kernel sees advances with the run, which is what makes a stochastic kernel reproducible.
    REQUIRE(first.last_ctx_step == 4);

    // A zero or non-finite `dt` is refused before any kernel sees it: every kernel would have to refuse it too,
    // and a zero step would append a sample nobody asked for.
    ctx.dt = 0.0;
    REQUIRE_FALSE(executor.advance(ctx).has_value());
    ctx.dt = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(executor.advance(ctx).has_value());
    REQUIRE(executor.report().steps == 5);
    REQUIRE(first.advances == 5);

    // A refused kernel call leaves the counters as they were: a step that did not happen is not a step.
    StubKernel failing{"failing", kInPlace, [](const pk::BatchView&, pk::AdvanceContext&) {
                           return qp::diag::Result<void>{qp::diag::ErrorCode::fit_failed};
                       }};
    std::vector<StepPlan> bad(1);
    bad[0].kernel = &failing;
    ParticleExecutor refused{state, std::move(bad)};
    REQUIRE(refused.prepare() == PlanRefusal::ok);
    ctx.dt = 0.01;
    REQUIRE_FALSE(refused.advance(ctx).has_value());
    REQUIRE(refused.report().steps == 0);
    REQUIRE(refused.report().kernel_calls == 0);

    // And a run can be reported as its own without being prepared again.
    executor.reset_report();
    REQUIRE(executor.report().steps == 0);
    REQUIRE(executor.prepared());
}

TEST_CASE("particles.executor.in_place_and_out_of_place_agree", "[particles]") {
    // `Capability::is_in_place` is the kernel's declaration, and the executor either honours it or corrupts the
    // input: a non-in-place kernel reads `in` while writing `out`, so handing it the same four slots makes the
    // answer depend on the order of the reads *inside* the kernel -- which is the one thing the executor is
    // supposed to make irrelevant.
    //
    // The kernel below is deliberately order-sensitive: it writes each component from the component before it, so
    // a loop that aliased `in` and `out` for a non-in-place family would feed its own output back in and the two
    // runs would differ on the second component onward.
    const auto cascade = [](const pk::BatchView& batch, pk::AdvanceContext& ctx) {
        const double* in_position = readable_slot(batch, 0);
        double* out_position = mutable_slot(batch, 0);
        const std::size_t n = static_cast<std::size_t>(batch.in[0].point_count());
        for (std::size_t i = 0; i < n; ++i) {
            const double seed = in_position[i * 3 + 0];
            out_position[i * 3 + 0] = seed + ctx.dt;
            out_position[i * 3 + 1] = out_position[i * 3 + 0] * 2.0;
            out_position[i * 3 + 2] = out_position[i * 3 + 1] * 2.0;
        }
        return qp::diag::Result<void>{};
    };

    ParticleState aliased = batch_of(3);
    ParticleState copied = batch_of(3);
    for (std::size_t i = 0; i < 3; ++i) {
        aliased.set(i, 0, ParticleState::Slot::position, static_cast<double>(i) + 1.0);
        copied.set(i, 0, ParticleState::Slot::position, static_cast<double>(i) + 1.0);
    }

    StubKernel in_place_kernel{"in-place", kInPlace, cascade};
    StubKernel out_of_place_kernel{"out-of-place", pk::Capability::none, cascade};

    std::vector<StepPlan> in_place_steps(1);
    in_place_steps[0].kernel = &in_place_kernel;
    ParticleExecutor in_place{aliased, std::move(in_place_steps)};

    std::vector<StepPlan> out_steps(1);
    out_steps[0].kernel = &out_of_place_kernel;
    ParticleExecutor out_of_place{copied, std::move(out_steps)};

    REQUIRE(in_place.prepare() == PlanRefusal::ok);
    REQUIRE(out_of_place.prepare() == PlanRefusal::ok);

    pk::AdvanceContext ctx;
    ctx.dt = 0.5;
    REQUIRE(in_place.advance(ctx).has_value());
    REQUIRE(out_of_place.advance(ctx).has_value());

    // The two must agree, and they only can if the out-of-place path read the untouched input while writing
    // somewhere else. Under aliasing the second component would come out as `(seed+dt)*2` twice over instead of
    // once, so the disagreement is a factor, not a rounding error.
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            REQUIRE(aliased.at(i, c, ParticleState::Slot::position) ==
                    copied.at(i, c, ParticleState::Slot::position));
        }
    }
    REQUIRE(copied.at(0, 0, ParticleState::Slot::position) == 1.5);
    REQUIRE(copied.at(0, 1, ParticleState::Slot::position) == 3.0);
    REQUIRE(copied.at(0, 2, ParticleState::Slot::position) == 6.0);
    REQUIRE(copied.at(2, 0, ParticleState::Slot::position) == 3.5);

    // The non-in-place path also carried the **status** slot back, which is the slot a kernel is most likely to
    // write and the one a copy that only handled the three physical slots would silently drop.
    REQUIRE(out_of_place.state().live_count() == 3);
}

TEST_CASE("particles.executor.a_self_inverse_kernel_round_trips", "[particles]") {
    // **The case that checks the loop is a loop.** A plan whose kernel is an involution -- apply it twice and the
    // state is back where it started -- gives an answer that a wrong step count, a wrong direction or a dropped
    // slot cannot fake: run it forward n times and back n times and require the original numbers, relative error
    // around the rounding. A scheme that merely looked plausible would drift by its truncation error, which is
    // many orders of magnitude larger.
    //
    // The kernel is `v -> -v` and `x -> -x`, which is its own inverse exactly, so the only error the round trip
    // can show is the copying and the clamp. That isolates the loop from the arithmetic on purpose: the physics
    // kernels' own reversibility is measured where the physics lives.
    const auto negate = [](const pk::BatchView& batch, pk::AdvanceContext&) {
        for (std::size_t slot = 0; slot < 2; ++slot) {
            const double* in = readable_slot(batch, slot);
            double* out = mutable_slot(batch, slot);
            const std::size_t total = static_cast<std::size_t>(batch.in[slot].point_count()) *
                                      (slot == 0 ? 3U : 3U);
            for (std::size_t k = 0; k < total; ++k) out[k] = -in[k];
        }
        return qp::diag::Result<void>{};
    };

    ParticleState state = batch_of(4);
    for (std::size_t i = 0; i < 4; ++i) {
        state.set(i, 0, ParticleState::Slot::position, 1.0 + static_cast<double>(i));
        state.set(i, 1, ParticleState::Slot::velocity, -0.5 * static_cast<double>(i) - 0.25);
    }

    StubKernel involutive{"involutive", kInPlace, negate};
    std::vector<StepPlan> steps(1);
    steps[0].kernel = &involutive;
    ParticleExecutor executor{state, std::move(steps)};
    REQUIRE(executor.prepare() == PlanRefusal::ok);

    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    constexpr int kRounds = 37;
    for (int i = 0; i < kRounds; ++i) {
        REQUIRE(executor.advance(ctx).has_value());
        REQUIRE(executor.advance(ctx).has_value());
    }

    // Thirty-seven forward-and-back pairs is an odd number of pairs applied twice, so the state is where it
    // started: an even number of negations is the identity. Asserted **exactly**, because each step is one
    // negation and no arithmetic lies between them, and an exact assertion is the only one that would catch a
    // loop that dropped a step (which would leave one negation unpaired).
    REQUIRE(executor.report().steps == 2 * kRounds);
    REQUIRE(executor.report().kernel_calls == 2 * kRounds);
    REQUIRE(executor.report().clamped == 0);
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(state.at(i, 0, ParticleState::Slot::position) == 1.0 + static_cast<double>(i));
        REQUIRE(state.at(i, 1, ParticleState::Slot::velocity) == -0.5 * static_cast<double>(i) - 0.25);
    }

    // And an odd number leaves every sign flipped, which is the half that shows the loop did not quietly double
    // one of the steps.
    REQUIRE(executor.advance(ctx).has_value());
    for (std::size_t i = 0; i < 4; ++i) {
        REQUIRE(state.at(i, 0, ParticleState::Slot::position) == -(1.0 + static_cast<double>(i)));
    }
}

TEST_CASE("particles.executor.a_clamp_is_counted_not_hidden", "[particles]") {
    // Charter C2 says a run never explodes; the project's rule is that the platform never clamps **silently**,
    // because a wrong answer wearing a right answer's clothes is worse than a reported failure. So the policy is
    // applied to everything a kernel wrote and what it changed is counted.
    //
    // The NaN is the case that matters: a NaN compares false against every bound, so the obvious spelling
    // `if (v > limit) v = limit;` lets it straight through and the run then produces non-finite numbers for the
    // rest of its life with nothing having reported anything.
    const auto misbehave = [](const pk::BatchView& batch, pk::AdvanceContext&) {
        double* position = mutable_slot(batch, 0);
        const double huge = std::numeric_limits<double>::infinity();
        const double not_a_number = std::numeric_limits<double>::quiet_NaN();
        position[0] = huge;
        position[1] = not_a_number;
        position[2] = -huge;
        position[3 * 1 + 0] = 1.0e13;   // past the default magnitude bound, but finite
        position[3 * 1 + 1] = 2.0;      // inside it
        position[3 * 1 + 2] = 3.0;
        return qp::diag::Result<void>{};
    };

    ParticleState state = batch_of(3);
    StubKernel kernel{"misbehaving", kInPlace, misbehave};
    std::vector<StepPlan> steps(1);
    steps[0].kernel = &kernel;
    ParticleExecutor executor{state, std::move(steps)};
    REQUIRE(executor.prepare() == PlanRefusal::ok);

    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    REQUIRE(executor.advance(ctx).has_value());

    // Four values were altered: two infinities, one NaN and one finite value past the bound. The count is the
    // evidence a confidence panel reports, and zero would be the silent clamp this case exists to refuse.
    REQUIRE(executor.report().clamped == 4);

    // Nothing non-finite survived, which is C2's guarantee.
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            REQUIRE(std::isfinite(state.at(i, c, ParticleState::Slot::position)));
        }
    }
    // A NaN becomes zero -- there is no bound to compare it against, which is why `apply` tests finiteness
    // **first** -- while an infinity becomes the policy's **signed** limit. A clamp that flattened `-inf` to
    // `+limit` would turn "this particle is falling out of the bottom of the box" into "it is at the top", which
    // is the same class of mistake as the sign error the reference implementation's own review notes record.
    REQUIRE(state.at(0, 1, ParticleState::Slot::position) == 0.0);
    REQUIRE(state.at(0, 2, ParticleState::Slot::position) == -1.0e12);
    REQUIRE(state.at(1, 0, ParticleState::Slot::position) == 1.0e12);   // the policy's limit
    REQUIRE(state.at(1, 1, ParticleState::Slot::position) == 2.0);      // untouched, bit for bit
    REQUIRE(state.at(1, 2, ParticleState::Slot::position) == 3.0);

    // Particle 0's arithmetic stopped being a number, so it is retired rather than left looking healthy. Particle
    // 1 was merely too large, which is a particle at the edge of the box and **not** a numerical failure -- the
    // distinction `Status` exists to keep.
    REQUIRE(state.status_of(0) == Status::escaped);
    REQUIRE(state.status_of(1) == Status::live);
    REQUIRE(state.live_count() == 2);

    // A policy of `none` is the other legitimate answer, and it is a declaration rather than a default: a
    // measurement wants the non-finite value reported, because a number past the clamp is indistinguishable from
    // one before it.
    ParticleState raw_state = batch_of(1);
    StubKernel raw_kernel{"raw", kInPlace, [](const pk::BatchView& batch, pk::AdvanceContext&) {
                              double* position = mutable_slot(batch, 0);
                              position[0] = std::numeric_limits<double>::quiet_NaN();
                              return qp::diag::Result<void>{};
                          }};
    std::vector<StepPlan> raw_steps(1);
    raw_steps[0].kernel = &raw_kernel;
    ParticleExecutor raw{raw_state, std::move(raw_steps), pk::ClampPolicy::none()};
    REQUIRE(raw.prepare() == PlanRefusal::ok);
    REQUIRE(raw.advance(ctx).has_value());
    REQUIRE(raw.report().clamped == 0);
    REQUIRE(std::isnan(raw_state.at(0, 0, ParticleState::Slot::position)));
}

