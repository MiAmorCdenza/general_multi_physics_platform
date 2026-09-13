/**
 * @file pusher.cpp
 * @brief The half of a pusher that is the same whoever advances the particles.
 *
 * `pusher.hpp` argues why this exists; this file is the code. Everything here was moved out of `boris.cpp`
 * **verbatim** when the kit grew a second scheme, which is the only reason to trust the move: a refactor that
 * also edited the arithmetic would be a refactor whose test results mean nothing. The Boris cases -- the
 * gyrofrequency, the round trip, the speed-limit count -- are what says the move was a move.
 */
#include <qp/plugins/magnetosphere/pusher.hpp>

namespace qp::plugins::magnetosphere {

namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;

qp::diag::Result<void> PusherAdvancer::prepare(const pk::ParamBlock& params) {
    const double range = params.real(PusherParams::kIndexMaxRange);
    const double gravity = params.real(PusherParams::kIndexGravity);
    const double substep_cap = params.real(PusherParams::kIndexSubstepCap);
    const double speed_limit = params.real(PusherParams::kIndexSpeedLimit);

    if (!std::isfinite(range) || range <= 0.0) return qp::diag::ErrorCode::invalid_argument;
    if (!std::isfinite(gravity) || gravity < 0.0) return qp::diag::ErrorCode::invalid_argument;
    if (!std::isfinite(substep_cap) || substep_cap < 1.0) return qp::diag::ErrorCode::invalid_argument;
    // **Strictly below one**, not at most one. A limit of exactly `c` would permit a state with `|v| = 1`, and the
    // Lorentz factor computed from it divides by zero: the run would produce a NaN on its first step instead of
    // saying that the limit it was given is not a limit. The reference implementation's `0.999999` is this
    // constraint met by hand.
    if (!std::isfinite(speed_limit) || speed_limit <= 0.0 || speed_limit >= 1.0) {
        return qp::diag::ErrorCode::invalid_argument;
    }
    if (!grid_is_usable(grid_of(params))) return qp::diag::ErrorCode::invalid_argument;
    params_ = params;
    return {};
}

bool PusherAdvancer::drag_enabled(const pk::BatchView& batch) const noexcept {
    constexpr std::size_t kDrag = pp::slot_index(pp::SlotName::drag);
    if (params_.integer(PusherParams::kIndexUseDrag) == 0) return false;
    return is_sampleable_scalar_volume(batch.in[kDrag]);
}

PusherAdvancer::Loaded PusherAdvancer::load(const pk::BatchView& batch, std::size_t particle, double* status,
                                            double dt) noexcept {
    constexpr std::size_t kPosition = pp::slot_index(pp::BatchSlot::position);
    constexpr std::size_t kVelocity = pp::slot_index(pp::BatchSlot::velocity);
    constexpr std::size_t kChargeMass = pp::slot_index(pp::BatchSlot::charge_mass);
    constexpr std::size_t kMagnetic = pp::slot_index(pp::SlotName::magnetic);

    const auto* position = static_cast<const double*>(batch.out[kPosition].data);
    const auto* velocity = static_cast<const double*>(batch.out[kVelocity].data);
    const auto* charge_mass = static_cast<const double*>(batch.in[kChargeMass].data);
    const gfield::FieldValue& magnetic = batch.in[kMagnetic];
    const GridMetadata grid = grid_of(params_);

    Loaded loaded;
    // **The boundary.** The batch is SI -- metres, metres per second, coulombs per kilogram -- because that is what
    // `particle_state.hpp` declares its slots to hold and what every report and every trace quotes; a buffer whose
    // description is a lie is worse than no description. The loop is normalized, because a dipole field at the
    // surface is 3e-5 T and a particle there moves at 4e5 m/s, and a double has the most room near one.
    loaded.position = Vec3{position[particle * 3 + 0], position[particle * 3 + 1], position[particle * 3 + 2]} *
                      kNormalizedPerMetre;
    loaded.velocity = Vec3{velocity[particle * 3 + 0], velocity[particle * 3 + 1], velocity[particle * 3 + 2]} *
                      kNormalizedPerMetrePerSecond;
    // `normalized_charge_mass` is the kit's own conversion, derived in `units.hpp` from the equation of motion.
    // The kernel calls it rather than repeating the product, which is the only reason the two cannot drift apart --
    // and it is the constant whose first two versions were wrong by 2209.
    loaded.charge_mass = normalized_charge_mass(charge_mass[particle]);

    if (!is_finite(loaded.position) || !is_finite(loaded.velocity) || !std::isfinite(loaded.charge_mass)) {
        *status = kStatusEscaped;
        ++retirements_;
        return loaded;
    }

    // The light-speed bound, applied to the **velocity**, which is what the Lorentz factor is computed from. A
    // particle that arrives past it is counted, because a report has to be able to say how many steps were
    // throttled -- and reaching this line at all means the step before it was not good enough, which is a
    // configuration finding rather than a numerical one.
    const double speed2 = norm2(loaded.velocity);
    const double speed_limit = params_.real(PusherParams::kIndexSpeedLimit);
    if (speed2 > speed_limit * speed_limit) {
        loaded.velocity = loaded.velocity * (speed_limit / std::sqrt(speed2));
        ++speed_clamps_;
    }

    // The sub-step count, from the rotation angle. `|t|` is the tangent of half the angle one rotation turns
    // through, so `2 atan(|t|)` is that angle, and the count keeps it under the threshold. Measured from the field
    // at the start of the step and the Lorentz factor at the start of the step, which is what makes a backward step
    // take the same number of sub-steps in the other direction: `|dt|` is what enters.
    const Vec3 b_si_start = sampled_volume(magnetic, grid, loaded.position * kEarthRadiusM);
    loaded.magnetic_start = b_si_start * (1.0 / kEquatorialSurfaceFieldT);
    const double gamma_now = 1.0 / std::sqrt(1.0 - norm2(loaded.velocity));
    const double t_magnitude = std::abs(loaded.charge_mass) * norm(loaded.magnetic_start) * std::abs(dt) /
                               (2.0 * gamma_now);
    const double rotation = 2.0 * std::atan(t_magnitude);
    double substeps = 1.0;
    if (rotation > PusherParams::kDefaultMaxRotation) {
        substeps = std::ceil(rotation / PusherParams::kDefaultMaxRotation);
    }
    const double cap = params_.real(PusherParams::kIndexSubstepCap);
    if (substeps > cap) substeps = cap;
    loaded.substeps = static_cast<std::size_t>(substeps);
    last_substeps_ += loaded.substeps;
    loaded.usable = true;
    return loaded;
}

void PusherAdvancer::finish(const Loaded& loaded, const Vec3& position, const Vec3& velocity, double* position_out,
                            double* velocity_out, double* status) noexcept {
    (void)loaded;
    const double range = params_.real(PusherParams::kIndexMaxRange);
    const double r = norm(position);
    if (!std::isfinite(r) || !is_finite(velocity)) {
        *status = kStatusEscaped;
        ++retirements_;
    } else if (r < 1.0) {
        *status = kStatusAbsorbed;
        ++retirements_;
    } else if (r > range + 2.0) {
        // Retired just past the range rather than at it, so a particle whose orbit reaches the boundary is not
        // clipped by the boundary condition. The reference implementation's own margin, kept.
        *status = kStatusEscaped;
        ++retirements_;
    }

    // Back across the boundary, in the other direction and with the other spelling of the same constant, so that a
    // reader of this file sees both halves of the conversion and neither has to be inferred.
    position_out[0] = position.x * kEarthRadiusM;
    position_out[1] = position.y * kEarthRadiusM;
    position_out[2] = position.z * kEarthRadiusM;
    velocity_out[0] = velocity.x * kSpeedOfLightSI;
    velocity_out[1] = velocity.y * kSpeedOfLightSI;
    velocity_out[2] = velocity.z * kSpeedOfLightSI;
}

}  // namespace qp::plugins::magnetosphere
