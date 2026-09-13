/**
 * @file verlet.cpp
 * @brief Half a drift, the force at the midpoint, the rotation, half a drift.
 *
 * The equation, the units and the sub-step control are the family's -- `pusher.hpp` and `pusher.cpp` -- and the
 * state is the same `(x, u)` with `u = gamma v` that Boris advances, so a comparison between the two schemes is a
 * comparison of schemes rather than of physics. What is here is the ordering, which is the scheme.
 */
#include <qp/plugins/magnetosphere/verlet.hpp>

#include <cmath>
#include <cstddef>

namespace qp::plugins::magnetosphere {

namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;

qp::diag::Result<void> VerletAdvancer::advance(const pk::BatchView& batch, pk::AdvanceContext& ctx) {
    if (!batch.valid()) return qp::diag::ErrorCode::invalid_argument;
    if (!std::isfinite(ctx.dt) || ctx.dt == 0.0) return qp::diag::ErrorCode::invalid_argument;
    // The batch's length is a constant of the executor, not a report of how many fields are bound -- the same check
    // the family's other schemes make, for the same reason: a shorter array is a caller who built a batch by hand,
    // and guessing what its four entries meant is how a magnetic field gets read as a drag coefficient.
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

void VerletAdvancer::push(PusherAdvancer::Loaded& loaded, const pk::BatchView& batch,
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
        // **The first half of the drift, and it is first on purpose.** Boris kicks at the position it starts from;
        // this scheme moves half a step, evaluates the force *there*, and kicks with it -- which is what makes the
        // force sample land where the particle is during the step. The reference's own one-line description of this
        // scheme is "position first", and this is the line it means.
        const Vec3 half_step = pos + vel * (sub_dt / 2.0);

        // The magnetic field **at the midpoint**, which is the read this scheme's cost is made of: one per
        // sub-step, counted by the sampler rather than documented in a header.
        const Vec3 b = sampled_volume(batch.in[pp::slot_index(pp::SlotName::magnetic)], field_grid,
                                      half_step * kEarthRadiusM) *
                       (1.0 / kEquatorialSurfaceFieldT);

        // Gravity, radial and attractive, skipped inside a tenth of a radius where the potential is singular --
        // the same guard, the same constant and the same expression as the family's other schemes, because it is
        // the same force evaluated somewhere else.
        Vec3 acceleration{};
        if (gm > 0.0) {
            const double r = norm(half_step);
            if (r > 0.1) acceleration = half_step * (-gm / (r * r * r));
        }

        // The electric field, in the units `E / (B_eq c)`, also at the midpoint.
        if (has_electric) {
            acceleration = acceleration + sampled_volume(batch.in[kElectric], field_grid, half_step * kEarthRadiusM) *
                                              (loaded.charge_mass / (kEquatorialSurfaceFieldT * kSpeedOfLightSI));
        }

        // The kick: the whole impulse, then the family's exact rotation. `u` is `gamma v`, so the velocity the
        // caller holds is converted into the momentum the rotation acts on and back afterwards.
        const Vec3 u_minus = vel * (1.0 / std::sqrt(1.0 - norm2(vel))) + acceleration * sub_dt;
        const double gamma_minus = std::sqrt(1.0 + norm2(u_minus));

        const Vec3 t = b * (loaded.charge_mass * (sub_dt / 2.0) / gamma_minus);
        const Vec3 s = t * (2.0 / (1.0 + norm2(t)));
        const Vec3 u_prime = u_minus + cross(u_minus, t);
        const Vec3 u = u_minus + cross(u_prime, s);

        // `|u|` is preserved by the rotation to the rounding -- the two shears are constructed so that the scale
        // factors cancel -- so a purely magnetic run's kinetic energy does not drift here either, which is the
        // property the reference's own description of this scheme claims for it.
        const double gamma = std::sqrt(1.0 + norm2(u));
        vel = u * (1.0 / gamma);

        if (has_drag) {
            // The family's treatment, and the contract says what it costs: a factor is first-order accurate where
            // RK4's right-hand side term is second, and the difference is `O((nu dt)^2)` per step. Drag is listed
            // in the reference's description of its *other* schemes and not of this one, which is right about the
            // scheme and wrong about a kit that would otherwise drop a wired input.
            const double nu = sampled_scalar(batch.in[kDrag], field_grid, half_step * kEarthRadiusM) /
                              kNormalizedPerSecond;
            if (nu > 0.0) {
                double factor = 1.0 - nu * sub_dt;
                if (factor < 0.0) factor = 0.0;
                vel = vel * factor;
            }
        }

        // ... and the second half of the drift, with the velocity the kick produced. The step is symmetric about
        // its own midpoint, which is what `is_reversible` reports.
        pos = half_step + vel * (sub_dt / 2.0);
    }

    loaded.position = pos;
    loaded.velocity = vel;
    loaded.substeps = loaded.substeps;   // unchanged: the count belongs to `load`, and `finish` reads it
}

}  // namespace qp::plugins::magnetosphere
