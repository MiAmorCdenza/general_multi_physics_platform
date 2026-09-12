/**
 * @file collision.cpp
 * @brief Implementation of the perfectly inelastic merge operator.
 *
 * Two sums and a division, written so the conservation property is visible in the code
 * rather than only in the comment. The momentum sum is accumulated as `m * v` **before**
 * anything is written, because a single-pass version that overwrote velocities as it went
 * would divide by a partial mass total and produce a number that looks plausible and
 * conserves nothing.
 */
#include <qp/plugins/mechanics/collision.hpp>

#include <qp/graph/field/field.hpp>

#include <cmath>
#include <cstdint>

namespace qp::plugins::mechanics {
namespace {

namespace kernels = graph::kernels;
namespace field = graph::field;

// The ABI admits strides of 1 (scalar) and 3 (vector) for f64 data, so the three
// components below are the whole particle and there is no reserved slot to zero. An
// earlier version of this file had the state as four doubles with a spare; that layout
// is not expressible, and the operator's own stride check refused every batch built
// from it -- which is the check doing its job.
constexpr std::uint32_t kPosition = 0;
constexpr std::uint32_t kVelocity = 1;
constexpr std::uint32_t kMass = 2;

constexpr std::size_t kRestitutionSlot = 0;

/// @brief A writable f64 element pointer, or null when the data is not f64.
[[nodiscard]] double* writable_elements(const field::FieldValue& v) noexcept {
    if (v.desc.element != qp::abi::ElementType::f64) return nullptr;
    return static_cast<double*>(const_cast<void*>(v.data));
}

}  // namespace

kernels::ClampPolicy MergeCollision::clamp_policy() const noexcept {
    // See the header: a weighted average cannot exceed its inputs, so the only thing worth
    // refusing here is a non-finite value.
    return kernels::ClampPolicy::finite();
}

std::string_view MergeCollision::name() const noexcept { return "merge_collision"; }

kernels::Capability MergeCollision::capabilities() const noexcept {
    return kernels::Capability::none;
}

diag::Result<void> MergeCollision::prepare(const kernels::ParamBlock& params) {
    const double restitution = params.real(kRestitutionSlot);

    // NaN is refused by the range comparison being written as a positive test: a NaN
    // fails `>= 0.0` and would otherwise slip through a `!(x < 0.0 || x > 1.0)` form.
    if (!std::isfinite(restitution)) return diag::ErrorCode::invalid_argument;
    if (!(restitution >= 0.0) || restitution > 1.0) return diag::ErrorCode::invalid_argument;

    // Only the perfectly inelastic case is expressible here. A bounce needs the collision
    // normal -- which way the two bodies were moving relative to each other at contact --
    // and a flat batch has no such information. Approximating it would return a wrong
    // number under a right-sounding name.
    if (restitution != 0.0) return diag::ErrorCode::not_implemented;

    prepared_ = true;
    clamps_fired_ = 0;
    return {};
}

diag::Result<void> MergeCollision::advance(const kernels::BatchView& batch,
                                           kernels::AdvanceContext& ctx) {
    if (!prepared_) return diag::ErrorCode::invalid_argument;
    if (!batch.valid()) return diag::ErrorCode::invalid_argument;

    if (writable_elements(*batch.in) == nullptr) return diag::ErrorCode::type_mismatch;
    if (writable_elements(*batch.out) == nullptr) return diag::ErrorCode::type_mismatch;

    double* in = writable_elements(*batch.in);
    double* out = writable_elements(*batch.out);

    const std::uint64_t stride =
        qp::abi::expected_spacing(batch.in->desc) / qp::abi::element_size(batch.in->desc.element);

    // The stride must have room for the three components this operator reads. A batch
    // described as a scalar lattice has a stride of one, and reading component 2 of it
    // would walk into the next particle's position -- which is how a "collision" ends up
    // depending on array layout.
    if (stride < kMass + 1) return diag::ErrorCode::type_mismatch;

    // Pass one: accumulate. Nothing is written yet, because the divisor is not known
    // until every mass has been seen.
    double total_mass = 0.0;
    double total_momentum = 0.0;
    for (std::size_t i = 0; i < batch.count; ++i) {
        const std::uint64_t base = static_cast<std::uint64_t>(i) * stride;
        const double m = in[base + kMass];
        const double v = in[base + kVelocity];

        // A mass that is not strictly positive is refused rather than skipped. Skipping a
        // zero mass would silently remove a particle from the collision; a negative one
        // would give a common velocity outside the input range. Both are arithmetically
        // well-defined and physically meaningless, so neither is a result worth returning.
        if (!std::isfinite(m) || !(m > 0.0)) return diag::ErrorCode::invalid_argument;
        if (!std::isfinite(v)) return diag::ErrorCode::invalid_argument;

        total_mass += m;
        total_momentum += m * v;
    }

    // `total_mass > 0.0` follows from every individual mass being positive, but the sum of
    // many small positives can be inf or can lose the smaller terms entirely; the check is
    // written anyway because the division below is the one place a bad value becomes a
    // wrong answer for every particle at once.
    if (!(total_mass > 0.0) || !std::isfinite(total_mass)) {
        return diag::ErrorCode::invalid_argument;
    }

    const double common = total_momentum / total_mass;

    // Pass two: write. Positions are copied through unchanged -- see the header for why a
    // coalescence does not move the positions it started from.
    for (std::size_t i = 0; i < batch.count; ++i) {
        const std::uint64_t base = static_cast<std::uint64_t>(i) * stride;
        // Clamped through the foundation's policy. The input components are copied through
        // the same path: a NaN that arrived in a position would otherwise be written onward
        // uncounted, which is exactly the silent case C8 exists to prevent.
        const kernels::ClampPolicy policy = clamp_policy();
        const kernels::ClampResult rx = kernels::apply(policy, in[base + kPosition]);
        const kernels::ClampResult rv = kernels::apply(policy, common);
        const kernels::ClampResult rm = kernels::apply(policy, in[base + kMass]);
        if (rx.changed) ++clamps_fired_;
        if (rv.changed) ++clamps_fired_;
        if (rm.changed) ++clamps_fired_;

        out[base + kPosition] = rx.value;
        out[base + kVelocity] = rv.value;
        out[base + kMass] = rm.value;
    }

    (void)ctx.step;
    return {};
}

}  // namespace qp::plugins::mechanics
