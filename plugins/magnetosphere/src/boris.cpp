/**
 * @file boris.cpp
 * @brief The push itself: five lines of vector algebra inside a sub-step loop, and the bookkeeping around it.
 *
 * ## The units, in one place
 *
 * Everything in the loop is dimensionless, because that is what `units.hpp` is for: a position is in earth radii,
 * a velocity in units of `c` so `|v| < 1` **is** the light-speed bound, a field is in units of the equatorial
 * surface value, and a charge-to-mass ratio is already an angular frequency. Three conversions touch the outside
 * world and all three are in `pusher.cpp` now, beside the family they belong to:
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
 * configuration lives. The six slots are named in `pusher.hpp`, once, for every scheme in the family.
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
 * each half would be right about its own arithmetic. What is left here is the scheme: the sub-step loop.
 */
#include <qp/plugins/magnetosphere/boris.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qp::plugins::magnetosphere {

namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;
namespace gfield = qp::graph::field;

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

    const gfield::FieldValue& magnetic = batch.in[pp::slot_index(pp::SlotName::magnetic)];
    if (!is_sampleable_volume(magnetic)) return qp::diag::ErrorCode::invalid_argument;
    if (batch.out[kPosition].desc.element != qp::abi::ElementType::f64 ||
        batch.out[kVelocity].desc.element != qp::abi::ElementType::f64 ||
        batch.in[kChargeMass].desc.element != qp::abi::ElementType::f64 ||
        batch.out[kStatus].desc.element != qp::abi::ElementType::f64) {
        return qp::diag::ErrorCode::invalid_argument;
    }

    auto* position = const_cast<double*>(static_cast<const double*>(batch.out[kPosition].data));
    auto* velocity = const_cast<double*>(static_cast<const double*>(batch.out[kVelocity].data));
    auto* status = const_cast<double*>(static_cast<const double*>(batch.out[kStatus].data));
    if (position == nullptr || velocity == nullptr || status == nullptr) {
        return qp::diag::ErrorCode::invalid_argument;
    }

    const std::size_t count = static_cast<std::size_t>(batch.out[kPosition].point_count());
    begin_advance();
    for (std::size_t particle = 0; particle < count; ++particle) {
        if (status[particle] != kStatusLive) continue;
        Loaded loaded = load(batch, particle, status, ctx.dt);
        if (!loaded.usable) continue;
        push(loaded, batch, ctx);
        finish(loaded, loaded.position, loaded.velocity, position + particle * 3, velocity + particle * 3,
               status + particle);
    }

    return {};
}

void BorisAdvancer::push(PusherAdvancer::Loaded& loaded, const pk::BatchView& batch,
                         const pk::AdvanceContext& ctx) noexcept {
    constexpr std::size_t kElectric = pp::slot_index(pp::SlotName::electric);
    constexpr std::size_t kDrag = pp::slot_index(pp::SlotName::drag);

    const bool has_electric = is_sampleable_volume(batch.in[kElectric]);
    const bool has_drag = drag_enabled(batch);
    const GridMetadata field_grid = grid();
    const double sub_dt = ctx.dt / static_cast<double>(loaded.substeps);
    const double gm = kNormalizedGravity * params().real(PusherParams::kIndexGravity);

    Vec3 pos = loaded.position;
    Vec3 vel = loaded.velocity;

    for (std::size_t sub = 0; sub < loaded.substeps; ++sub) {
        const Vec3 b = (sub == 0) ? loaded.magnetic_start
                                  : sampled_volume(batch.in[pp::slot_index(pp::SlotName::magnetic)], field_grid,
                                                  pos * kEarthRadiusM) *
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
            electric = sampled_volume(batch.in[kElectric], field_grid, pos * kEarthRadiusM) *
                       (1.0 / (kEquatorialSurfaceFieldT * kSpeedOfLightSI));
        }

        // The half impulse, then the rotation, then the other half. `u` is `gamma v`, so the first line
        // converts the velocity the caller holds into the momentum the scheme rotates.
        const Vec3 impulse = (electric * loaded.charge_mass + acceleration) * (sub_dt / 2.0);
        const Vec3 u_minus = vel * (1.0 / std::sqrt(1.0 - norm2(vel))) + impulse;
        const double gamma_minus = std::sqrt(1.0 + norm2(u_minus));

        const Vec3 t = b * (loaded.charge_mass * (sub_dt / 2.0) / gamma_minus);
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
            const double nu = sampled_scalar(batch.in[kDrag], field_grid, pos * kEarthRadiusM) /
                              kNormalizedPerSecond;
            if (nu > 0.0) {
                double factor = 1.0 - nu * sub_dt;
                if (factor < 0.0) factor = 0.0;
                vel = vel * factor;
            }
        }

        pos = pos + vel * sub_dt;
    }

    loaded.position = pos;
    loaded.velocity = vel;
}

}  // namespace qp::plugins::magnetosphere
