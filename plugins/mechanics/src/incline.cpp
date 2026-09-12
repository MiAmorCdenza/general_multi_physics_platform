/**
 * @file incline.cpp
 * @brief Implementation of the incline-with-friction stepper.
 *
 * The whole operator is one decision per particle per step, and the decision is written
 * as three explicit cases rather than as a clamped force. The clamp form --
 * `a = a_g - clamp(a_g, -mu_s*g*cos, +mu_s*g*cos)` for the static case, `mu_k` for the
 * moving one -- is shorter and is what most textbook implementations use, but it computes
 * a force that is then thrown away, and the two branches it hides are exactly the two
 * behaviours a student is asked to distinguish.
 */
#include <qp/plugins/mechanics/incline.hpp>

#include <qp/graph/field/field.hpp>

#include <cmath>
#include <cstdint>

namespace qp::plugins::mechanics {
namespace {

namespace kernels = graph::kernels;
namespace field = graph::field;

// `(position, velocity, angle)` is the whole particle: the ABI admits strides of 1 and 3
// for f64 data, so a three-component vector is exactly full and there is no reserved slot.
constexpr std::uint32_t kPosition = 0;
/// Component holding the along-slope velocity, positive downhill.
constexpr std::uint32_t kVelocity = 1;
/// Component holding the incline angle, in radians.
constexpr std::uint32_t kAngle = 2;

/// Parameter slots.
constexpr std::size_t kGravitySlot = 0;
constexpr std::size_t kStaticSlot = 1;
constexpr std::size_t kKineticSlot = 2;

/// @brief A writable f64 element pointer, or null when the data is not f64.
[[nodiscard]] double* writable_elements(const field::FieldValue& v) noexcept {
    if (v.desc.element != qp::abi::ElementType::f64) return nullptr;
    return static_cast<double*>(const_cast<void*>(v.data));
}

/// @brief Whether a coefficient is usable: finite and not negative.
[[nodiscard]] bool usable_coefficient(double c) noexcept {
    return std::isfinite(c) && c >= 0.0;
}

}  // namespace

kernels::ClampPolicy InclineFriction::clamp_policy() const noexcept {
    // See the header: the bound catches divergence, not the end of a slope.
    return kernels::ClampPolicy::bounded(1.0e12);
}

std::string_view InclineFriction::name() const noexcept { return "incline_friction"; }

kernels::Capability InclineFriction::capabilities() const noexcept {
    return kernels::Capability::none;
}

diag::Result<void> InclineFriction::prepare(const kernels::ParamBlock& params) {
    const double g = params.real(kGravitySlot);
    const double mu_s = params.real(kStaticSlot);
    const double mu_k = params.real(kKineticSlot);

    // `!(g > 0.0)` rather than `g <= 0.0` so that NaN is refused by the same test that
    // refuses zero. A comparison against NaN is false, which is the kind of accident
    // that lets a failed upstream computation through as a valid parameter.
    if (!std::isfinite(g) || !(g > 0.0)) return diag::ErrorCode::invalid_argument;
    if (!usable_coefficient(mu_s)) return diag::ErrorCode::invalid_argument;
    if (!usable_coefficient(mu_k)) return diag::ErrorCode::invalid_argument;

    // Refused rather than swapped. See the header: a surface where breaking loose makes
    // sliding harder is not a surface, and quietly reinterpreting the parameters would
    // hide the confusion instead of surfacing it.
    if (mu_k > mu_s) return diag::ErrorCode::invalid_argument;

    gravity_ = g;
    mu_static_ = mu_s;
    mu_kinetic_ = mu_k;
    prepared_ = true;
    // A parameter change is a new experiment: carrying the previous run's clamp count into
    // it would make a clean run look as though it had been clamped.
    clamps_fired_ = 0;
    return {};
}

diag::Result<void> InclineFriction::advance(const kernels::BatchView& batch,
                                            kernels::AdvanceContext& ctx) {
    if (!prepared_) return diag::ErrorCode::invalid_argument;
    if (!batch.valid()) return diag::ErrorCode::invalid_argument;

    const double dt = ctx.dt;
    if (!std::isfinite(dt) || dt <= 0.0) return diag::ErrorCode::invalid_argument;

    if (writable_elements(*batch.in) == nullptr) return diag::ErrorCode::type_mismatch;
    if (writable_elements(*batch.out) == nullptr) return diag::ErrorCode::type_mismatch;

    double* in = writable_elements(*batch.in);
    double* out = writable_elements(*batch.out);

    // The stride comes from the ABI's definition of the buffer, never from this
    // operator's idea of how many numbers a block has. An earlier operator in this
    // module got that wrong and read a two-double state with a stride of one.
    const std::uint64_t stride =
        qp::abi::expected_spacing(batch.in->desc) / qp::abi::element_size(batch.in->desc.element);

    for (std::size_t i = 0; i < batch.count; ++i) {
        const std::uint64_t base = static_cast<std::uint64_t>(i) * stride;

        const double s = in[base + kPosition];
        double v = in[base + kVelocity];
        const double theta = in[base + kAngle];

        // A non-finite angle is refused per particle rather than in `prepare`, because it
        // is per-particle state and not a parameter. Writing the input back unchanged
        // would let a NaN spread through a run while every step reported success.
        if (!std::isfinite(theta) || !std::isfinite(s) || !std::isfinite(v)) {
            return diag::ErrorCode::invalid_argument;
        }

        const double sin_t = std::sin(theta);
        const double cos_t = std::cos(theta);

        // Gravity's component along the slope, positive downhill, and the normal force's
        // contribution to the friction magnitude. The mass cancels: both are
        // accelerations, which is why this operator has no mass parameter.
        const double a_gravity = gravity_ * sin_t;
        const double friction_limit = gravity_ * cos_t;  // per unit mu

        if (v == 0.0) {
            // At rest. Move only if gravity alone exceeds what static friction can hold.
            // The comparison is strict: exactly at the threshold the block holds, which
            // is what "the angle of repose" means and what a student measures.
            if (std::abs(a_gravity) > mu_static_ * friction_limit) {
                v = a_gravity * dt;
            }
            // Otherwise v stays exactly 0.0 -- not "a very small number". A resting block
            // that accumulated 1e-16 per step would drift measurably over a long run, and
            // the drift would look like a physical effect.
        } else {
            const double a_friction = -std::copysign(mu_kinetic_ * friction_limit, v);
            const double v_next = v + (a_gravity + a_friction) * dt;

            // If friction would carry the velocity through zero inside this step, the
            // block stops at zero rather than being accelerated back up the slope.
            // Without this the block oscillates about the bottom of the incline, which
            // is a numerical artefact that looks like a bouncing block.
            if (v_next * v < 0.0) {
                v = 0.0;
            } else {
                v = v_next;
            }
        }

        // Semi-implicit: the position uses the **new** velocity.
        //
        // ## Why this is not a rounding detail
        //
        // The exact position after a step is `s + v*dt + a*dt^2/2`. Using the new velocity
        // gives `s + (v + a*dt)*dt = s + v*dt + a*dt^2`, which omits the half and adds a
        // whole -- so the per-step truncation error is exactly `a*dt^2/2` and the error
        // after `n` steps is `a*dt^2*n/2 = a*dt*t/2`.
        //
        // That is **first order** in dt, and it is not an accident to be tightened away: a
        // friction decision at zero velocity is a discontinuity, and a higher-order scheme
        // that samples the force at intermediate velocities cannot make that decision
        // cleanly. The cost is a linear error term, and it is stated here rather than
        // discovered by whoever compares the graph against a closed form.
        // Clamped through the foundation's policy rather than by an inline bound, so this
        // operator cannot differ from the others on the case that matters: a NaN compares
        // false against every bound, and a hand-written `if (v > limit)` writes it on while
        // reporting that nothing happened. See kernel.clamp_policy.applies.
        const kernels::ClampPolicy policy = clamp_policy();
        const kernels::ClampResult rs = kernels::apply(policy, s + v * dt);
        const kernels::ClampResult rv = kernels::apply(policy, v);
        if (rs.changed) ++clamps_fired_;
        if (rv.changed) ++clamps_fired_;

        out[base + kPosition] = rs.value;
        out[base + kVelocity] = rv.value;
        // The angle is an input and nothing here changes it, so it is carried through. It
        // is part of the state rather than a parameter because it is per particle: one
        // graph can compare two inclines side by side, which a global value cannot express.
        out[base + kAngle] = theta;
    }

    (void)ctx.step;
    return {};
}

}  // namespace qp::plugins::mechanics
