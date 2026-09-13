/**
 * @file rk4.cpp
 * @brief Four stages, one sample each, and no invariant promised anywhere.
 *
 * The equation, the units and the sub-step control are the family's -- `pusher.hpp` and `pusher.cpp` -- and the
 * state is the same `(x, u)` with `u = gamma v` that Boris advances. What is here is the classical fourth-order
 * Runge-Kutta step, written out rather than looped over a table of stage coefficients, because four stages with
 * coefficients `1/2`, `1/2`, `1` are a shape a reader can check against the textbook and a table is not.
 *
 * **Drag is part of the right-hand side here**, where Boris multiplies by `1 - nu dt` after the rotation: the
 * decay is then second-order accurate rather than first, and the difference is `O((nu dt)^2)` per step -- which is
 * the size of number this kit measures rather than argues about.
 */
#include <qp/plugins/magnetosphere/rk4.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace qp::plugins::magnetosphere {

namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;
namespace gfield = qp::graph::field;

Rk4Advancer::Derivative Rk4Advancer::stage(const pk::BatchView& batch, const Vec3& position, const Vec3& momentum,
                                           double charge_mass, bool has_electric, bool has_drag) const noexcept {
    const GridMetadata field_grid = grid();
    const Vec3 point = position * kEarthRadiusM;

    // `u = gamma v`, so the velocity the Lorentz force needs is `u / gamma` -- the same relation Boris inverts
    // before its rotation.
    const double gamma = std::sqrt(1.0 + norm2(momentum));
    const Vec3 velocity = momentum * (1.0 / gamma);

    const Vec3 b = sampled_volume(batch.in[pp::slot_index(pp::SlotName::magnetic)], field_grid, point) *
                   (1.0 / kEquatorialSurfaceFieldT);

    Derivative out;
    out.velocity = velocity;
    out.acceleration = cross(velocity, b) * charge_mass;
    if (has_electric) {
        const Vec3 electric = sampled_volume(batch.in[pp::slot_index(pp::SlotName::electric)], field_grid, point) *
                              (1.0 / (kEquatorialSurfaceFieldT * kSpeedOfLightSI));
        out.acceleration = out.acceleration + electric * charge_mass;
    }

    // Gravity, radial and attractive, skipped inside a tenth of a radius where the potential is singular -- the
    // same guard, the same expression and the same constant as Boris, because it is the same force.
    const double gm = kNormalizedGravity * params().real(PusherParams::kIndexGravity);
    if (gm > 0.0) {
        const double r = norm(position);
        if (r > 0.1) out.acceleration = out.acceleration + position * (-gm / (r * r * r));
    }

    if (has_drag) {
        // The table is SI per second; a rate converts by the **reciprocal** of the time unit. As a force it is
        // `du/dt = -nu u`, which integrates to `u(t) = u0 exp(-nu t)` -- the decay the atmosphere node's case
        // measures against, and the reason this term is here rather than applied to the velocity afterwards.
        const double nu = sampled_scalar(batch.in[pp::slot_index(pp::SlotName::drag)], field_grid, point) /
                          kNormalizedPerSecond;
        if (nu > 0.0) out.acceleration = out.acceleration - momentum * nu;
    }
    return out;
}

qp::diag::Result<void> Rk4Advancer::advance(const pk::BatchView& batch, pk::AdvanceContext& ctx) {
    if (!batch.valid()) return qp::diag::ErrorCode::invalid_argument;
    if (!std::isfinite(ctx.dt) || ctx.dt == 0.0) return qp::diag::ErrorCode::invalid_argument;
    if (batch.count < pp::kBatchSlotCount) return qp::diag::ErrorCode::invalid_argument;

    constexpr std::size_t kPosition = pp::slot_index(pp::BatchSlot::position);
    constexpr std::size_t kVelocity = pp::slot_index(pp::BatchSlot::velocity);
    constexpr std::size_t kChargeMass = pp::slot_index(pp::BatchSlot::charge_mass);
    constexpr std::size_t kStatus = pp::slot_index(pp::BatchSlot::status);
    constexpr std::size_t kElectric = pp::slot_index(pp::SlotName::electric);

    if (!is_sampleable_volume(batch.in[pp::slot_index(pp::SlotName::magnetic)])) {
        return qp::diag::ErrorCode::invalid_argument;
    }
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
    const bool has_electric = is_sampleable_volume(batch.in[kElectric]);
    const bool has_drag = drag_enabled(batch);
    begin_advance();

    for (std::size_t particle = 0; particle < count; ++particle) {
        if (status[particle] != kStatusLive) continue;
        Loaded loaded = load(batch, particle, status, ctx.dt);
        if (!loaded.usable) continue;

        // The scheme's state is `(x, u)` with `u = gamma v`; the family hands a velocity over, so the momentum is
        // built here rather than carried in the buffer. The same conversion Boris does before its rotation.
        Vec3 pos = loaded.position;
        Vec3 momentum = loaded.velocity * (1.0 / std::sqrt(1.0 - norm2(loaded.velocity)));
        const double h = ctx.dt / static_cast<double>(loaded.substeps);

        for (std::size_t sub = 0; sub < loaded.substeps; ++sub) {
            const Derivative k1 = stage(batch, pos, momentum, loaded.charge_mass, has_electric, has_drag);
            const Derivative k2 = stage(batch, pos + k1.velocity * (h / 2.0), momentum + k1.acceleration * (h / 2.0),
                                        loaded.charge_mass, has_electric, has_drag);
            const Derivative k3 = stage(batch, pos + k2.velocity * (h / 2.0), momentum + k2.acceleration * (h / 2.0),
                                        loaded.charge_mass, has_electric, has_drag);
            const Derivative k4 = stage(batch, pos + k3.velocity * h, momentum + k3.acceleration * h,
                                        loaded.charge_mass, has_electric, has_drag);

            pos = pos + (k1.velocity + k2.velocity * 2.0 + k3.velocity * 2.0 + k4.velocity) * (h / 6.0);
            momentum = momentum +
                       (k1.acceleration + k2.acceleration * 2.0 + k3.acceleration * 2.0 + k4.acceleration) * (h / 6.0);
        }

        // Back to a velocity, by the same relation the loop inverted. `|u|` is free to pass one -- it is
        // `gamma v` -- which is why the light-speed bound is applied to the **velocity** in `load` and not here.
        const double gamma = std::sqrt(1.0 + norm2(momentum));
        loaded.velocity = momentum * (1.0 / gamma);
        loaded.position = pos;
        finish(loaded, loaded.position, loaded.velocity, position + particle * 3, velocity + particle * 3,
               status + particle);
    }

    return {};
}

}  // namespace qp::plugins::magnetosphere
