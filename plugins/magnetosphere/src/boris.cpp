/**
 * @file boris.cpp
 * @brief The push itself: five lines of vector algebra inside a sub-step loop, and the bookkeeping around it.
 *
 * ## The units, in one place
 *
 * Everything in the loop is dimensionless, because that is what `units.hpp` is for: a position is in earth radii,
 * a velocity in units of `c` so `|v| < 1` **is** the light-speed bound, a field is in units of the equatorial
 * surface value, and a charge-to-mass ratio is already an angular frequency. Three conversions touch the outside
 * world and all three are here:
 *
 *   - the field table holds **SI tesla on an SI grid**, and is sampled at `position * R_E` then divided by
 *     `B_eq`. That is the split `units.hpp` argues for: SI at the boundary, normalized inside;
 *   - the electric table holds **SI volts per metre**, and is divided by `B_eq c`, which is the field that makes
 *     a particle move at the speed of light -- so `E` and `v x B` end up in the same units before they are added;
 *   - the drag table holds **SI per second**, and is divided by `kNormalizedPerSecond`, because a rate is the one
 *     quantity whose conversion is the reciprocal of the time unit's.
 *
 * ## The grid, and why its metadata is a parameter
 *
 * A `field::FieldValue` describes a lattice's **counts** and not its origin or spacing, so a kernel that wants to
 * interpolate between samples has to be told where they are. There are two ways to tell it -- carry the metadata
 * beside the value, or put it in the parameter block -- and the parameter block is the one that needs no new type
 * in `core/`: it already exists, it is already handed to `prepare`, and it is already the place a kernel's
 * configuration lives. The six slots are named `kIndexGridOrigin0` and so on in the header.
 *
 * The alternative considered and not taken is a heap of samples per particle read through `get_component`, which
 * is correct and is what the shared blend exists to avoid: eight reads per component per sub-step, times twenty
 * thousand particles, times twenty sub-steps.
 *
 * ## Why the blend is not in this file
 *
 * It used to be. It moved to `baked_field.cpp` when the **emitter** needed the same read: an emitter has to know
 * the local field direction to launch a particle at a given pitch angle, and two copies of a trilinear sampler in
 * one kit are two answers to "what is the field between two nodes". The disagreement would show up as a particle
 * curving slightly differently from where it was launched -- and no test of either half would catch it, because
 * each half would be right about its own arithmetic. What is left here is the geometry the sampler needs, read
 * from the parameter block, and two one-line calls.
 */
#include <qp/plugins/magnetosphere/boris.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/geometry.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qp::plugins::magnetosphere {
namespace {

namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;
namespace gfield = qp::graph::field;

/// @brief The status codes, taken from the module that owns them rather than re-spelled here.
///
/// `particles::Status` is the definition; a plugin that wrote its own `0.0`, `1.0`, `2.0` would be a second
/// definition, and the day one of them changed the failure would be a particle that is retired in one file and
/// live in the other. The conversion is explicit because the slot is a double: `field::FieldValue` reads f64 and
/// nothing else, which is why `Status` is a small integer stored in a double rather than an enum in a buffer.
constexpr double kStatusLive = static_cast<double>(pp::Status::live);
constexpr double kStatusAbsorbed = static_cast<double>(pp::Status::absorbed);
constexpr double kStatusEscaped = static_cast<double>(pp::Status::escaped);

/// @brief Where the field grid's first node is, and how far apart they are -- in SI, because the table is SI.
struct GridMetadata final {
    Vec3 origin{};
    Vec3 spacing{};
};

/// @brief The grid a plan's field slots were baked onto, from the parameter block.
[[nodiscard]] GridMetadata grid_of(const pk::ParamBlock& params) noexcept {
    GridMetadata grid;
    grid.origin = Vec3{params.real(BorisAdvancer::kIndexGridOrigin0),
                       params.real(BorisAdvancer::kIndexGridOrigin1),
                       params.real(BorisAdvancer::kIndexGridOrigin2)};
    grid.spacing = Vec3{params.real(BorisAdvancer::kIndexGridSpacing0),
                        params.real(BorisAdvancer::kIndexGridSpacing1),
                        params.real(BorisAdvancer::kIndexGridSpacing2)};
    return grid;
}

/// @brief Whether a grid's spacing can be divided by.
[[nodiscard]] bool grid_is_usable(const GridMetadata& grid) noexcept {
    return std::isfinite(grid.origin.x) && std::isfinite(grid.origin.y) && std::isfinite(grid.origin.z) &&
           std::isfinite(grid.spacing.x) && std::isfinite(grid.spacing.y) && std::isfinite(grid.spacing.z) &&
           grid.spacing.x > 0.0 && grid.spacing.y > 0.0 && grid.spacing.z > 0.0;
}

/// @brief Whether a slot is a volume of vectors on three axes, which is the only shape this kernel samples.
///
/// A `point` lattice describes one value and carries zero counts, so reading one as a volume would silently
/// produce a zero field: the particle travels in a straight line and nothing says why. Refusing names it.
[[nodiscard]] bool is_sampleable_volume(const gfield::FieldValue& field) noexcept {
    return gfield::is_readable(field) && field.kind() == gfield::Kind::Volume && field.is_vector() &&
           field.desc.count[0] >= 2 && field.desc.count[1] >= 2 && field.desc.count[2] >= 2;
}

/// @brief Whether a slot is a volume of scalars on three axes, which is the drag table's shape.
[[nodiscard]] bool is_sampleable_scalar_volume(const gfield::FieldValue& field) noexcept {
    return gfield::is_readable(field) && field.kind() == gfield::Kind::Volume && field.is_scalar() &&
           field.desc.count[0] >= 2 && field.desc.count[1] >= 2 && field.desc.count[2] >= 2;
}

/// @brief The vector field at `point` (SI), read through the sampler this kit shares with its emitter.
[[nodiscard]] Vec3 sample_volume(const gfield::FieldValue& field, const GridMetadata& grid,
                                 const Vec3& point) noexcept {
    return sample_baked(field, grid.origin, grid.spacing, point);
}

/// @brief The scalar field at `point` (SI), read through the same sampler.
[[nodiscard]] double sample_scalar(const gfield::FieldValue& field, const GridMetadata& grid,
                                   const Vec3& point) noexcept {
    return sample_baked_scalar(field, grid.origin, grid.spacing, point);
}

}  // namespace

qp::diag::Result<void> BorisAdvancer::prepare(const pk::ParamBlock& params) {
    const double range = params.real(kIndexMaxRange);
    const double gravity = params.real(kIndexGravity);
    const double substep_cap = params.real(kIndexSubstepCap);
    const double speed_limit = params.real(kIndexSpeedLimit);

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

qp::diag::Result<void> BorisAdvancer::advance(const pk::BatchView& batch, pk::AdvanceContext& ctx) {
    if (!batch.valid()) return qp::diag::ErrorCode::invalid_argument;
    if (!std::isfinite(ctx.dt) || ctx.dt == 0.0) return qp::diag::ErrorCode::invalid_argument;
    // The batch's length is a constant of the executor, not a report of how many fields are bound: a shorter array
    // is a caller who built a batch by hand, and guessing what its four entries meant would be how a magnetic
    // field gets read as a drag coefficient.
    if (batch.count < pp::kBatchSlotCount) return qp::diag::ErrorCode::invalid_argument;

    constexpr std::size_t kPosition = pp::slot_index(pp::BatchSlot::position);
    constexpr std::size_t kVelocity = pp::slot_index(pp::BatchSlot::velocity);
    constexpr std::size_t kChargeMass = pp::slot_index(pp::BatchSlot::charge_mass);
    constexpr std::size_t kStatus = pp::slot_index(pp::BatchSlot::status);
    constexpr std::size_t kMagnetic = pp::slot_index(pp::SlotName::magnetic);
    constexpr std::size_t kElectric = pp::slot_index(pp::SlotName::electric);
    constexpr std::size_t kDrag = pp::slot_index(pp::SlotName::drag);

    const gfield::FieldValue& magnetic = batch.in[kMagnetic];
    if (!is_sampleable_volume(magnetic)) return qp::diag::ErrorCode::invalid_argument;
    if (batch.in[kPosition].desc.element != qp::abi::ElementType::f64 ||
        batch.in[kVelocity].desc.element != qp::abi::ElementType::f64 ||
        batch.in[kChargeMass].desc.element != qp::abi::ElementType::f64 ||
        batch.in[kStatus].desc.element != qp::abi::ElementType::f64) {
        return qp::diag::ErrorCode::invalid_argument;
    }

    const bool has_electric = is_sampleable_volume(batch.in[kElectric]);
    const bool use_drag = params_.integer(kIndexUseDrag) != 0;
    const bool has_drag = use_drag && is_sampleable_scalar_volume(batch.in[kDrag]);
    const GridMetadata grid = grid_of(params_);

    auto* position = const_cast<double*>(static_cast<const double*>(batch.out[kPosition].data));
    auto* velocity = const_cast<double*>(static_cast<const double*>(batch.out[kVelocity].data));
    auto* status = const_cast<double*>(static_cast<const double*>(batch.out[kStatus].data));
    const auto* charge_mass = static_cast<const double*>(batch.in[kChargeMass].data);
    if (position == nullptr || velocity == nullptr || status == nullptr || charge_mass == nullptr) {
        return qp::diag::ErrorCode::invalid_argument;
    }

    const std::size_t count = static_cast<std::size_t>(batch.in[kPosition].point_count());
    const double range = params_.real(kIndexMaxRange);
    const double gravity_multiplier = params_.real(kIndexGravity);
    const double substep_cap = params_.real(kIndexSubstepCap);
    const double speed_limit = params_.real(kIndexSpeedLimit);
    const double dt = ctx.dt;

    last_substeps_ = 0;

    for (std::size_t particle = 0; particle < count; ++particle) {
        if (status[particle] != kStatusLive) continue;

        // **The boundary.** The batch is SI -- metres, metres per second, coulombs per kilogram -- because that is
        // what `particle_state.hpp` declares its slots to hold and what every report and every trace quotes;
        // a buffer whose description is a lie is worse than no description. The loop is normalized, because a
        // dipole field at the surface is 3e-5 T and a particle there moves at 4e5 m/s, and a double has the most
        // room near one. So the three quantities a step reads are converted **once per particle per host step**,
        // not per sub-step: it is nine multiplies against a loop that already does hundreds.
        Vec3 pos = Vec3{position[particle * 3 + 0], position[particle * 3 + 1], position[particle * 3 + 2]} *
                   kNormalizedPerMetre;
        Vec3 vel = Vec3{velocity[particle * 3 + 0], velocity[particle * 3 + 1], velocity[particle * 3 + 2]} *
                   kNormalizedPerMetrePerSecond;
        // `normalized_charge_mass` is the kit's own conversion, derived in `units.hpp` from the equation of
        // motion. The kernel calls it rather than repeating the product, which is the only reason the two cannot
        // drift apart -- and it is the constant whose first two versions were wrong by 2209.
        const double q_prime = normalized_charge_mass(charge_mass[particle]);

        if (!is_finite(pos) || !is_finite(vel) || !std::isfinite(q_prime)) {
            status[particle] = kStatusEscaped;
            ++retirements_;
            continue;
        }

        // The light-speed bound, applied to the **velocity**, which is what the Lorentz factor is computed from.
        // A particle that arrives past it is counted, because a report has to be able to say how many steps were
        // throttled -- and reaching this line at all means the step before it was not good enough, which is a
        // configuration finding rather than a numerical one.
        const double speed2 = norm2(vel);
        if (speed2 > speed_limit * speed_limit) {
            vel = vel * (speed_limit / std::sqrt(speed2));
            ++speed_clamps_;
        }

        // The sub-step count, from the rotation angle. `|t|` is the tangent of half the angle one rotation turns
        // through, so `2 atan(|t|)` is that angle, and the count keeps it under the threshold. Measured from the
        // field at the start of the step and the Lorentz factor at the start of the step, which is what makes a
        // backward step take the same number of sub-steps in the other direction: `|dt|` is what enters.
        const Vec3 b_si_start = sample_volume(magnetic, grid, pos * kEarthRadiusM);
        const Vec3 b_start = b_si_start * (1.0 / kEquatorialSurfaceFieldT);
        const double gamma_now = 1.0 / std::sqrt(1.0 - norm2(vel));
        const double t_magnitude = std::abs(q_prime) * norm(b_start) * std::abs(dt) / (2.0 * gamma_now);
        const double rotation = 2.0 * std::atan(t_magnitude);
        double substeps = 1.0;
        if (rotation > kDefaultMaxRotation) substeps = std::ceil(rotation / kDefaultMaxRotation);
        if (substeps > substep_cap) substeps = substep_cap;
        const std::size_t n_sub = static_cast<std::size_t>(substeps);
        last_substeps_ += n_sub;

        const double sub_dt = dt / static_cast<double>(n_sub);
        const double gm = kNormalizedGravity * gravity_multiplier;

        for (std::size_t sub = 0; sub < n_sub; ++sub) {
            const Vec3 b = (sub == 0)
                               ? b_start
                               : sample_volume(magnetic, grid, pos * kEarthRadiusM) *
                                     (1.0 / kEquatorialSurfaceFieldT);

            // Gravity, radial and attractive, skipped inside a tenth of a radius where the potential is singular
            // and the force would dominate everything else in the run. `kNormalizedGravity / r^2` is the Earth's
            // own acceleration in units of `R_E / T^2`, which is the only place the SI value is used.
            Vec3 acceleration{};
            if (gm > 0.0) {
                const double r = norm(pos);
                if (r > 0.1) acceleration = pos * (-gm / (r * r * r));
            }

            // The electric field, in the units `E / (B_eq c)` -- the field that moves a particle at `c`. The
            // impulse it contributes is `q_prime * electric`, because `q_prime` is already a rate per normalized
            // time and the two normalized fields are then in the same units as `v x B`.
            Vec3 electric{};
            if (has_electric) {
                electric = sample_volume(batch.in[kElectric], grid, pos * kEarthRadiusM) *
                           (1.0 / (kEquatorialSurfaceFieldT * kSpeedOfLightSI));
            }

            // The half impulse, then the rotation, then the other half. `u` is `gamma v`, so the first line
            // converts the velocity the caller holds into the momentum the scheme rotates.
            const Vec3 impulse = (electric * q_prime + acceleration) * (sub_dt / 2.0);
            const Vec3 u_minus = vel * (1.0 / std::sqrt(1.0 - norm2(vel))) + impulse;
            const double gamma_minus = std::sqrt(1.0 + norm2(u_minus));

            const Vec3 t = b * (q_prime * (sub_dt / 2.0) / gamma_minus);
            const Vec3 s = t * (2.0 / (1.0 + norm2(t)));
            const Vec3 u_prime = u_minus + cross(u_minus, t);
            const Vec3 u = u_minus + cross(u_prime, s) + impulse;

            // `|u|` is preserved by the rotation to the rounding -- the two shears are constructed so that the
            // scale factors cancel -- which is why the kinetic energy of a purely magnetic run does not drift, and
            // what `magnetosphere.boris.a_magnetic_field_does_no_work` measures rather than assumes.
            const double gamma = std::sqrt(1.0 + norm2(u));
            vel = u * (1.0 / gamma);

            if (has_drag) {
                // The table is SI per second; a rate converts by the **reciprocal** of the time unit, which is why
                // this divides where a length or a field multiplies.
                const double nu = sample_scalar(batch.in[kDrag], grid, pos * kEarthRadiusM) /
                                  kNormalizedPerSecond;
                if (nu > 0.0) {
                    double factor = 1.0 - nu * sub_dt;
                    if (factor < 0.0) factor = 0.0;
                    vel = vel * factor;
                }
            }

            pos = pos + vel * sub_dt;
        }

        const double r = norm(pos);
        if (!std::isfinite(r) || !is_finite(vel)) {
            status[particle] = kStatusEscaped;
            ++retirements_;
        } else if (r < 1.0) {
            status[particle] = kStatusAbsorbed;
            ++retirements_;
        } else if (r > range + 2.0) {
            // Retired just past the range rather than at it, so a particle whose orbit reaches the boundary is not
            // clipped by the boundary condition. The reference implementation's own margin, kept.
            status[particle] = kStatusEscaped;
            ++retirements_;
        }

        // Back across the boundary, in the other direction and with the other spelling of the same constant, so
        // that a reader of this file sees both halves of the conversion and neither has to be inferred.
        position[particle * 3 + 0] = pos.x * kEarthRadiusM;
        position[particle * 3 + 1] = pos.y * kEarthRadiusM;
        position[particle * 3 + 2] = pos.z * kEarthRadiusM;
        velocity[particle * 3 + 0] = vel.x * kSpeedOfLightSI;
        velocity[particle * 3 + 1] = vel.y * kSpeedOfLightSI;
        velocity[particle * 3 + 2] = vel.z * kSpeedOfLightSI;
    }

    return {};
}

}  // namespace qp::plugins::magnetosphere
