/**
 * @file executor.cpp
 * @brief The step loop: prepare once, advance per step, clamp and count what a kernel wrote.
 *
 * The three things this file has to get right, and each is a way the bridge could be silently wrong:
 *
 *   1. **The slot order is one fact.** `slots_of` builds the array and the kernels read it, so the agreement
 *      lives in one function rather than in a convention. A kernel told that index 2 is the charge-to-mass ratio
 *      will divide by it; if this file put the status there instead, the arithmetic would produce numbers that
 *      look like accelerations and the run would be wrong in a way no test of the kernel alone could see.
 *   2. **A kernel that is not in place gets its own output.** `Capability::is_in_place` is the kernel's
 *      declaration, and the executor either honours it or corrupts the input: a non-in-place kernel reads `in`
 *      while writing `out`, and handing it the same four slots makes the read order decide the answer.
 *   3. **What a kernel wrote is clamped and counted, and a value that stopped being a number retires its
 *      particle.** C2's guarantee is that a run never explodes; the project's rule is that the platform never
 *      clamps silently. Both are honoured here, at the one place every kernel's output passes through.
 */
#include <qp/graph/particles/executor.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

namespace qp::graph::particles {
namespace {

namespace pk = qp::graph::kernels;

using Slot = ParticleState::Slot;

/// @brief The three physical slots, in the order the clamp and the finiteness sweep visit them.
///
/// `status` is deliberately absent: it is an integer code, and clamping one would turn "not a number" into
/// `live`, which is the one answer a corrupt particle must not be given.
constexpr Slot kPhysicalSlots[] = {Slot::position, Slot::velocity, Slot::charge_mass};

/// @brief How many components a slot holds, known here because this file owns the slot order.
[[nodiscard]] constexpr std::size_t components_of(Slot which) noexcept {
    return which == Slot::charge_mass || which == Slot::status ? 1U : 3U;
}

}  // namespace

const char* to_string(SlotName name) noexcept {
    switch (name) {
        case SlotName::magnetic: return "magnetic";
        case SlotName::electric: return "electric";
        case SlotName::drag: return "drag";
    }
    return "unknown";
}

const char* to_string(PlanRefusal refusal) noexcept {
    switch (refusal) {
        case PlanRefusal::ok: return "ok";
        case PlanRefusal::empty_plan: return "empty_plan";
        case PlanRefusal::step_without_kernel: return "step_without_kernel";
        case PlanRefusal::kernel_refused: return "kernel_refused";
        case PlanRefusal::slot_unbound: return "slot_unbound";
        case PlanRefusal::scratch_unavailable: return "scratch_unavailable";
    }
    return "unknown";
}

const field::FieldValue& StepPlan::field(SlotName name) const noexcept {
    static const field::FieldValue kAbsent{};
    const auto index = static_cast<std::size_t>(name);
    if (index >= kSlotNameCount) return kAbsent;
    return fields[index];
}

ParticleExecutor::ParticleExecutor(ParticleState& state, std::vector<StepPlan> steps,
                                   pk::ClampPolicy clamp) noexcept
    : state_(&state), steps_(std::move(steps)), clamp_(clamp) {}

void ParticleExecutor::slots_of(ParticleState& state,
                                field::FieldValue (&out)[ParticleState::kSlotCount]) noexcept {
    out[0] = state.slot(Slot::position);
    out[1] = state.slot(Slot::velocity);
    out[2] = state.slot(Slot::charge_mass);
    out[3] = state.slot(Slot::status);
}

PlanRefusal ParticleExecutor::prepare() {
    prepared_ = false;

    if (steps_.empty()) return PlanRefusal::empty_plan;
    if (state_->count() == 0) {
        // A batch with no particles has no valid lattice description (see `ParticleState::slot`), so every step
        // would be handed an invalid `BatchView` and each kernel would have to refuse it. Refusing once, here,
        // names the actual problem instead of reporting a kernel's complaint about a buffer it should never have
        // been shown.
        return PlanRefusal::empty_plan;
    }

    // The checks, then the preparation: a plan with a missing kernel is refused before any kernel is prepared, so
    // a failure leaves nothing half-initialised. `prepare` is allowed to allocate and is the one place a kernel
    // validates its parameters, which is why it happens here rather than on the first step.
    scratch_bytes_ = 0;
    for (const StepPlan& step : steps_) {
        if (step.kernel == nullptr) return PlanRefusal::step_without_kernel;
        if (pk::has_capability(step.kernel->capabilities(), pk::Capability::needs_scratch)) {
            // The interface sizes scratch in bytes and leaves the number to the implementation, which is why the
            // reservation is per particle and generous rather than exact: a kernel that needs more reports it by
            // failing to advance, and a reservation that guessed low would be a buffer overrun instead.
            scratch_bytes_ += state_->count() * 64 * sizeof(double);
        }
    }

    out_.resize(state_->count());
    scratch_.assign(scratch_bytes_ / sizeof(double) + 1, 0.0);

    for (const StepPlan& step : steps_) {
        const auto prepared = step.kernel->prepare(step.param);
        // The kernel's own code is not propagated: `PlanRefusal` is this layer's vocabulary, and a caller that
        // wants the kernel's reason asks the kernel. What matters here is that the plan did not come up.
        if (!prepared.has_value()) return PlanRefusal::kernel_refused;
    }

    prepared_ = true;
    return PlanRefusal::ok;
}

void ParticleExecutor::clamp_state(ParticleState& state) noexcept {
    const std::size_t n = state.count();
    for (std::size_t i = 0; i < n; ++i) {
        bool stopped_being_a_number = false;
        for (const Slot which : kPhysicalSlots) {
            for (std::size_t c = 0; c < components_of(which); ++c) {
                const double raw = state.at(i, c, which);
                const pk::ClampResult applied = pk::apply(clamp_, raw);
                if (!applied.changed) continue;
                ++report_.clamped;
                // `apply` returns zero for a value that is not a number and the policy's limit for one that is
                // merely too large. Only the first retires the particle: a clamped position is a particle at the
                // edge of the box, while a NaN is a particle whose arithmetic has stopped describing anything.
                // Retiring both would make a boundary artefact look like a numerical failure.
                if (!std::isfinite(raw)) stopped_being_a_number = true;
                state.set(i, c, which, applied.value);
            }
        }
        if (stopped_being_a_number && state.status_of(i) == Status::live) {
            state.set_status(i, Status::escaped);
        }
    }
}

diag::Result<void> ParticleExecutor::advance(pk::AdvanceContext& ctx) {
    if (!prepared_) return diag::ErrorCode::not_implemented;
    if (!std::isfinite(ctx.dt) || ctx.dt == 0.0) return diag::ErrorCode::invalid_argument;
    if (state_->count() == 0 || !state_->is_consistent()) return diag::ErrorCode::invalid_argument;

    // The run's own bookkeeping, filled in here rather than trusted from the caller: `step` is how many steps
    // this executor has taken, and a kernel handed a stale counter would take a different branch and, if it is
    // stochastic, draw a different stream -- which is exactly the unreproducible run charter R2 forbids.
    ctx.step = report_.steps;
    ctx.scratch = scratch_.empty() ? nullptr : scratch_.data();
    ctx.scratch_bytes = scratch_bytes_;

    for (const StepPlan& step : steps_) {
        const bool in_place = pk::has_capability(step.kernel->capabilities(), pk::Capability::is_in_place);

        field::FieldValue in_slots[ParticleState::kSlotCount]{};
        field::FieldValue out_slots[ParticleState::kSlotCount]{};
        slots_of(*state_, in_slots);
        if (in_place) {
            // Aliased on purpose: the kernel declared that it reads and writes the same buffers, and the
            // executor's job is to believe the declaration rather than to protect against it.
            for (std::size_t i = 0; i < ParticleState::kSlotCount; ++i) out_slots[i] = in_slots[i];
        } else {
            slots_of(out_, out_slots);
        }

        pk::BatchView batch;
        batch.in = in_slots;
        batch.out = out_slots;
        // The number of **slots**, not particles. A batch is an array of described buffers, and the particle
        // count lives inside each buffer's lattice; passing the particle count here would tell the kernel there
        // are N slots and it would read past the end of a four-element array.
        batch.count = ParticleState::kSlotCount;

        const auto advanced = step.kernel->advance(batch, ctx);
        if (!advanced.has_value()) {
            // The counters are left as they were: a step that did not happen is not a step, and a report that
            // counted a refused call would overstate how far the run got.
            return advanced.error();
        }
        ++report_.kernel_calls;

        if (!in_place) {
            // Copied slot by slot through the byte views, so the copy cannot disagree with `slots_of` about
            // which array is which, and so a kernel that wrote a status has it carried forward. `out_` was
            // resized to this batch's count in `prepare`, so the two descriptions have identical geometry and
            // the copy is a straight span.
            field::FieldValue source[ParticleState::kSlotCount]{};
            slots_of(out_, source);
            for (std::size_t slot_index = 0; slot_index < ParticleState::kSlotCount; ++slot_index) {
                const std::size_t bytes = static_cast<std::size_t>(source[slot_index].required_bytes());
                if (bytes == 0) continue;
                std::memcpy(const_cast<void*>(in_slots[slot_index].data), source[slot_index].data, bytes);
            }
        }
    }

    clamp_state(*state_);
    ++report_.steps;
    return {};
}

}  // namespace qp::graph::particles
