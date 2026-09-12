/**
 * @file rk4_oscillator.cpp
 * @brief Implementation of the RK4 harmonic oscillator stepper.
 *
 * The arithmetic is the whole file, and it is deliberately spelled out rather than
 * looped over four generic stages. A loop would need an array of stage coefficients
 * per particle, which is either a heap allocation on the step path -- forbidden by
 * the contract -- or a fixed four-element local that indexing prevents the compiler
 * from keeping in registers. Written out, each stage's derivative feeds the next
 * with no memory traffic at all.
 */
#include <qp/plugins/mechanics/rk4_oscillator.hpp>

#include <qp/graph/field/field.hpp>

#include <cmath>
#include <cstdint>

namespace qp::plugins::mechanics {
namespace {

namespace kernels = graph::kernels;
namespace field = graph::field;

/// Component index of position within one particle's state.
constexpr std::uint32_t kPosition = 0;
/// Component index of velocity within one particle's state.
constexpr std::uint32_t kVelocity = 1;

/// Parameter slot holding `omega`.
constexpr std::size_t kOmegaSlot = 0;

/**
 * @brief A writable double pointer into a `FieldValue`.
 *
 * `FieldValue::data` is `const void*` because a field is normally something a
 * reader samples. The batch contract makes the output side writable, and the
 * established pattern in this repository's kernel tests is the same cast. It is
 * confined to one function so there is exactly one place to audit, and it returns
 * null rather than an invalid pointer when the field is not f64 -- the caller then
 * refuses the batch instead of writing through a misaligned pointer.
 */
[[nodiscard]] double* writable_elements(const field::FieldValue& v) noexcept {
    if (v.desc.element != qp::abi::ElementType::f64) return nullptr;
    return static_cast<double*>(const_cast<void*>(v.data));
}

}  // namespace

std::string_view Rk4Oscillator::name() const noexcept { return "rk4_oscillator"; }

kernels::Capability Rk4Oscillator::capabilities() const noexcept {
    return kernels::Capability::none;
}

diag::Result<void> Rk4Oscillator::prepare(const kernels::ParamBlock& params) {
    const double omega = params.real(kOmegaSlot);
    // `isfinite` and the sign are checked separately rather than as `omega > 0`
    // alone, because `omega > 0` is false for NaN as well and the two want different
    // messages: a NaN usually means an upstream computation failed, while a zero
    // usually means a parameter was left at its default.
    if (!std::isfinite(omega)) return diag::ErrorCode::invalid_argument;
    if (!(omega > 0.0)) return diag::ErrorCode::invalid_argument;

    omega_ = omega;
    prepared_ = true;
    return {};
}

diag::Result<void> Rk4Oscillator::advance(const kernels::BatchView& batch,
                                          kernels::AdvanceContext& ctx) {
    if (!prepared_) return diag::ErrorCode::invalid_argument;
    if (!batch.valid()) return diag::ErrorCode::invalid_argument;
    // A field that is not f64 cannot be written by this operator. Refusing is the
    // only honest answer: the alternative is to widen on the way in, integrate in
    // double, and narrow on the way out, which silently rounds state the caller
    // asked to keep in single precision and makes the result depend on the storage
    // type rather than on the physics.
    if (writable_elements(*batch.out) == nullptr) return diag::ErrorCode::type_mismatch;
    if (writable_elements(*batch.in) == nullptr) return diag::ErrorCode::type_mismatch;

    // Hoisted out of the loop. The contract forbids allocation and host callbacks on
    // the step path, and re-deriving an element pointer or a stride per particle is
    // the same mistake in cheaper clothing: it puts work proportional to `count` on a
    // path that only needs it once.
    const double* in = writable_elements(*batch.in);
    double* out = writable_elements(*batch.out);

    // The stride comes from the ABI's own definition of a description's per-point
    // size, not from a number this operator chose.
    //
    // An earlier version computed `component_count(v.components())`, which is **1 for
    // a scalar lattice** -- and then read a two-double state with a stride of one. The
    // first particle moved a little, the second particle's position was read as the
    // first particle's velocity, and everything past the first two doubles was
    // accumulated into a buffer position that no longer corresponded to a particle.
    //
    // `abi::expected_spacing` is the same expression `abi::is_consistent` checks a
    // description against, so an operator that uses it cannot disagree with the
    // layout the host validated. A stride is a property of the buffer, never of the
    // operator's idea of the physics.
    const std::uint64_t stride =
        qp::abi::expected_spacing(batch.in->desc) / qp::abi::element_size(batch.in->desc.element);

    const double dt = ctx.dt;
    if (!std::isfinite(dt) || dt <= 0.0) return diag::ErrorCode::invalid_argument;

    // `omega^2` once per call rather than once per particle per stage. The
    // multiplication is cheap either way; what this avoids is the square root and
    // multiply inside the loop that a `k`/`m` parameterisation would have forced.
    const double w2 = omega_ * omega_;

    for (std::size_t i = 0; i < batch.count; ++i) {
        const std::uint64_t base = static_cast<std::uint64_t>(i) * stride;

        const double x = in[base + kPosition];
        const double v = in[base + kVelocity];

        // The state is `y = (x, v)` and the right-hand side is
        //   f(y) = (v, -omega^2 * x)
        // which is autonomous, so no stage takes a time argument.
        //
        // Every stage evaluates **both** components of `f` at its own intermediate
        // point. Writing the second component as `-w2 * (intermediate x)` and the
        // first as the intermediate `v` is the whole method; getting it wrong is easy
        // and quiet, because applying the second-order rule `-w2 * y` to both
        // components still produces a bounded circle -- just one a quarter turn out of
        // phase, with the wrong velocity and the same energy. That version passed the
        // energy test and failed only because the closed-form orbit was checked.
        const double k1x = v;
        const double k1v = -w2 * x;

        const double x2 = x + 0.5 * dt * k1x;
        const double v2 = v + 0.5 * dt * k1v;
        const double k2x = v2;
        const double k2v = -w2 * x2;

        const double x3 = x + 0.5 * dt * k2x;
        const double v3 = v + 0.5 * dt * k2v;
        const double k3x = v3;
        const double k3v = -w2 * x3;

        const double x4 = x + dt * k3x;
        const double v4 = v + dt * k3v;
        const double k4x = v4;
        const double k4v = -w2 * x4;

        const double sixth = dt / 6.0;
        out[base + kPosition] = x + sixth * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
        out[base + kVelocity] = v + sixth * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);

        // The third component is reserved rather than free. Particle state travels as
        // a lattice vector because the ABI fixes a description's stride at
        // `component_count x element_size`, so a two-double state has no legal
        // description of its own -- it is a vector whose third slot nothing reads.
        //
        // Writing it is not tidiness. The host is free to hand over a buffer whose
        // reserved slot holds a previous run's values, and leaving it alone would make
        // the contents of a state buffer depend on its history: two runs of the same
        // graph would produce buffers that differ in a component nobody integrates,
        // and a byte-comparison regression test would fail for a reason that has
        // nothing to do with the physics.
        if (stride > kVelocity + 1) {
            out[base + kVelocity + 1] = 0.0;
        }
    }

    // The host advances the step counter, not the operator: two operators in one
    // plan must agree on what step it is, and an operator that incremented it would
    // make the number depend on how many operators ran before it.
    (void)ctx.step;
    return {};
}

}  // namespace qp::plugins::mechanics
