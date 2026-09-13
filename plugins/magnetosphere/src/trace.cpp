/**
 * @file trace.cpp
 * @brief Runge-Kutta-Merson along the unit field, with the five stop reasons the header names.
 *
 * ## The one sign convention, stated once
 *
 * The curve is `x'(s) = sense * unit_B(x)`, where `sense` is `+1` or `-1`, so **`s` is arc length** and every
 * step below is a positive distance along the curve. The reference implementation this is ported from writes the
 * same five stages with a *negative* step (`s3 = -ds/3`, R = unit_B * s3) and a comment saying so; the two forms
 * compute identical numbers, and this one is used because the alternative is a function whose every line has a
 * minus sign in it that a reader has to hold in their head at once. The correspondence is exact:
 *
 *     reference:  R(p) = unit_B(p) * (-ds/3),   ds < 0 travels along +B
 *     here:       k(p) = unit_B(p),             h > 0  travels along sense * B,  h = -ds
 *     stages:     R1..R5 and k1..k5 are the same five arguments (substituting h = -ds into each)
 *     estimate:   sum|R1 - 4.5 R3 + 4 R4 - 0.5 R5| = (h/3) * sum|k1 - 4.5 k3 + 4 k4 - 0.5 k5|
 *     update:     x + (R1 + 4 R4 + R5)/2 = x + (h/6)(k1 + 4 k4 + k5)
 *
 * The factor of a third in the estimate is not decoration: it is what makes the tolerance mean the same thing on
 * both sides, and it is why `TraceSpec::tolerance` carries the reference's own 1e-4.
 *
 * ## Two improvements over the reference, both found by reading it
 *
 *   - **a vanishing field stops the walk.** In the reference, a zero field makes every stage's `R` zero, so the
 *     error estimate is zero, so the step grows to its cap and `x` never moves -- and the loop runs to
 *     `max_points`, appending duplicates. It reports `Stagnant` only when no step *converges*, which a zero
 *     field never fails to do. Here a stage that cannot produce a direction ends the walk as `no_field`.
 *   - **the radial rate is exact.** The reference seeds its "am I moving inward" test with `|x| + 0.01` (or
 *     `- 0.01`), a fudge whose comment says it is only for initialisation. The derivative of `|x|` along the
 *     curve is `dot(x, unit_B) / |x|`, which costs one dot product of two vectors already in hand, has no magic
 *     constant, and is exact for the first step as well as every later one.
 */
#include <qp/plugins/magnetosphere/trace.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <cmath>
#include <limits>

namespace qp::plugins::magnetosphere {
namespace {

namespace gfield = qp::graph::field;

/// The sum of the absolute components. Used for the Merson estimate because it is compared against a tolerance:
/// a Euclidean norm would take a square root per stage for a number that decides whether to halve a step, and
/// five extra square roots per stage per line is measurable across a family of a hundred lines.
[[nodiscard]] double abs_sum(const Vec3& v) noexcept {
    return std::abs(v.x) + std::abs(v.y) + std::abs(v.z);
}

/// How small a squared field may be before it has no direction worth trusting. `unit_B` amplifies whatever the
/// table holds, so a sample that is numerically zero would otherwise be turned into a full-length direction
/// pointing along the interpolation's own rounding error.
constexpr double kDirectionFloor2 = 1.0e-30;

/// The narrowing near the surface: below this radius the step is chosen to resolve the approach, not the field.
constexpr double kNearRadius = 3.0;
/// The narrowing factor, and the tighter one used within `kNearBand` of the surface.
constexpr double kNearFactor = 0.2;
constexpr double kTightFactor = 0.05;
constexpr double kNearBand = 0.05;
/// The offset in `factor * (r - surface + offset)`: it keeps the step positive at the surface itself.
constexpr double kNearOffset = 0.2;
/// A step is grown only when the error is below this fraction of the tolerance, and never past the cap.
constexpr double kGrowBelow = 0.04;
/// How many radial reversals means "closed, or circling". A closed dipole line reverses twice a lap.
constexpr int kMaxReversals = 4;
/// A radial change below this fraction of the step is rounding noise rather than motion: see the reversal count.
constexpr double kRadialDeadband = 1.0e-3;

}  // namespace

const char* to_string(TraceStop stop) noexcept {
    switch (stop) {
        case TraceStop::hit_surface: return "hit_surface";
        case TraceStop::left_table: return "left_table";
        case TraceStop::looped: return "looped";
        case TraceStop::max_points: return "max_points";
        case TraceStop::no_field: return "no_field";
    }
    return "unknown";
}

bool TraceSpec::usable() const noexcept {
    if (!(step_max_re > 0.0) || !std::isfinite(step_max_re)) return false;
    if (!(tolerance > 0.0) || !std::isfinite(tolerance)) return false;
    if (!(surface_radius_re > 0.0) || !std::isfinite(surface_radius_re)) return false;
    return max_points > 0 && max_attempts > 0;
}

FieldTracer::FieldTracer(gfield::FieldValue table, Vec3 origin_m, Vec3 spacing_m, TraceSpec spec) noexcept
    : table_(table), origin_m_(origin_m), spacing_m_(spacing_m), spec_(spec) {
    // A volume of vectors with at least two nodes an axis, and a positive spacing on each: anything else has no
    // interior to interpolate in, and `sample_baked` would answer the boundary value everywhere.
    const bool shaped = table_.desc.kind == qp::abi::LatticeKind::volume &&
                        table_.desc.component == qp::abi::ComponentKind::vector && gfield::is_readable(table_) &&
                        table_.desc.count[0] >= 2 && table_.desc.count[1] >= 2 && table_.desc.count[2] >= 2;
    const bool spaced = spacing_m_.x > 0.0 && spacing_m_.y > 0.0 && spacing_m_.z > 0.0 &&
                        std::isfinite(spacing_m_.x) && std::isfinite(spacing_m_.y) && std::isfinite(spacing_m_.z);
    usable_ = shaped && spaced && spec_.usable();
    if (!usable_) return;

    min_re_ = origin_m_ * kNormalizedPerMetre;
    max_re_ = Vec3{origin_m_.x + static_cast<double>(table_.desc.count[0] - 1) * spacing_m_.x,
                   origin_m_.y + static_cast<double>(table_.desc.count[1] - 1) * spacing_m_.y,
                   origin_m_.z + static_cast<double>(table_.desc.count[2] - 1) * spacing_m_.z} *
              kNormalizedPerMetre;
}

Vec3 FieldTracer::direction(const Vec3& point_re) const noexcept {
    // One multiply per component per read: the tracer's own arithmetic is all in earth radii, and the conversion
    // lives here so that no caller has to know what unit the samples are in.
    const Vec3 sample = sample_baked(table_, origin_m_, spacing_m_, point_re * kEarthRadiusM);
    ++samples_;
    const double n2 = norm2(sample);
    if (!(n2 > kDirectionFloor2) || !std::isfinite(n2)) return Vec3{};
    return sample * (1.0 / std::sqrt(n2));
}

bool FieldTracer::inside(const Vec3& point_re) const noexcept {
    return point_re.x >= min_re_.x && point_re.x <= max_re_.x && point_re.y >= min_re_.y &&
           point_re.y <= max_re_.y && point_re.z >= min_re_.z && point_re.z <= max_re_.z;
}

TraceStop FieldTracer::walk(const Vec3& seed_re, double sense, FieldLine& out) const {
    out.points_re.clear();
    out.length_re = 0.0;
    out.min_radius_re = std::numeric_limits<double>::infinity();

    if (!inside(seed_re)) return TraceStop::left_table;
    Vec3 x = seed_re;
    Vec3 t = direction(x);
    if (norm2(t) == 0.0) return TraceStop::no_field;

    out.points_re.push_back(x);
    double radius = norm(x);
    out.min_radius_re = radius;
    double step = spec_.step_max_re;
    double previous_radius = radius;
    int reversals = 0;
    double previous_dr = 0.0;

    while (out.points_re.size() < spec_.max_points) {
        // The travel direction for this walk: the field's own unit direction, oriented by which way this half
        // goes. **Every stage must be oriented, not just the first**, and that is worth a sentence because getting
        // it half right is a plausible-looking mistake: `x + (k1 + 4 k4 + k5)/6` with only `k1` flipped advances
        // by `((sense + 5)/6) * unit_B` -- the right *direction* for the forward half, two thirds of the requested
        // step in both, and a backward half that walks **along** the field instead of against it. Measured on this
        // kit's own L = 5 dipole case: the backward half took one step of 0.133 instead of 0.2 and then stopped at
        // a foot point 0.13 from the seed, in the same hemisphere as the forward one.
        const auto oriented = [this, sense](const Vec3& point) { return direction(point) * sense; };
        const Vec3 heading = t * sense;
        // The exact radial rate along the direction of travel: |x|'(s) = dot(x, heading)/|x|.
        const double radial_rate = dot(x, heading) / radius;
        // Narrowing, only while approaching the surface: a step chosen for the approach must not be reused on
        // the way out, where the field changes slowly and a large step is both correct and much cheaper.
        if (radial_rate < 0.0 && radius < kNearRadius) {
            const double gap = radius - spec_.surface_radius_re;
            const double factor = gap < kNearBand ? kTightFactor : kNearFactor;
            step = factor * (gap + kNearOffset);
        }
        if (step > spec_.step_max_re) step = spec_.step_max_re;
        if (!(step > 0.0)) return TraceStop::no_field;

        // ---- the five Merson stages, retried until the step is both accepted and within the cap ----
        Vec3 next{};
        bool stepped = false;
        double tried = step;
        for (std::size_t attempt = 0; attempt < spec_.max_attempts; ++attempt) {
            const Vec3 k1 = heading;
            const Vec3 k2 = oriented(x + k1 * (tried / 3.0));
            const Vec3 k3 = oriented(x + (k1 + k2) * (tried / 6.0));
            const Vec3 k4 = oriented(x + (k1 + k3 * 3.0) * (tried / 8.0));
            const Vec3 k5 = oriented(x + (k1 - k3 * 3.0 + k4 * 4.0) * (tried / 2.0));
            if (norm2(k2) == 0.0 || norm2(k3) == 0.0 || norm2(k4) == 0.0 || norm2(k5) == 0.0) {
                // A stage with no direction is a field that is not there, not a step that is too big: halving it
                // would sample the same zero again. This is the case the reference loops to `max_points` on.
                return TraceStop::no_field;
            }
            const double error = (tried / 3.0) * abs_sum(k1 - k3 * 4.5 + k4 * 4.0 - k5 * 0.5);
            if (!(error <= spec_.tolerance)) {
                tried *= 0.5;
                continue;
            }
            if (tried > spec_.step_max_re) {
                // Accepted, but it grew past the cap: clamp and redo, so that the cap is what the caller asked
                // for rather than a value the growth rule happened to reach.
                tried = spec_.step_max_re;
                continue;
            }
            next = x + (k1 + k4 * 4.0 + k5) * (tried / 6.0);
            // Grow only from a comfortably accurate step, so that a step is not oscillating between the cap and
            // the tolerance on every iteration.
            step = (error < spec_.tolerance * kGrowBelow && tried < spec_.step_max_re / 1.5) ? tried * 1.5 : tried;
            stepped = true;
            break;
        }
        if (!stepped) return TraceStop::no_field;

        const double next_radius = norm(next);
        if (!std::isfinite(next_radius)) return TraceStop::no_field;

        // The inner boundary -- placed on the sphere **exactly**, by solving for the crossing rather than by
        // interpolating between the two points. The reference interpolates the *position* linearly and lands at
        // `0.9999874883` rather than `1.0` (measured, on the L = 5 line this kit's own case traces), because the
        // radius along a chord is not linear in the chord's parameter: `|p(t)|^2` is a quadratic. It costs one
        // square root once per line, at the one place in the trace where an exact answer is available, and it is
        // what makes "the foot point is on the surface" an equality a case can assert instead of a tolerance.
        if (next_radius < spec_.surface_radius_re && radius > next_radius) {
            const Vec3 previous = out.points_re.back();
            const Vec3 span = next - previous;
            const double span2 = norm2(span);
            const double half_b = dot(previous, span);
            const double c = norm2(previous) - spec_.surface_radius_re * spec_.surface_radius_re;
            // The discriminant is non-negative because the segment does cross -- one end is outside and the other
            // inside -- and the guard is only for the grazing case, where rounding can push it just below zero.
            const double disc = std::max(0.0, half_b * half_b - span2 * c);
            // The **entry** root: the smaller of the two, which is the one the curve reaches first coming inward.
            double t = (-half_b - std::sqrt(disc)) / span2;
            if (!(t > 0.0)) t = 0.0;
            if (t > 1.0) t = 1.0;
            const Vec3 foot = previous + span * t;
            out.length_re += norm(foot - x);
            out.points_re.back() = foot;
            out.min_radius_re = spec_.surface_radius_re;
            return TraceStop::hit_surface;
        }

        // Leaving the sampled box ends the walk **before** the point leaves it. The sampler clamps outside the
        // grid, so a walk that continued would integrate a constant field and draw a long straight line that
        // looks like data -- the failure the reference's own comment calls the root of a "messy" outer picture.
        if (!inside(next)) return TraceStop::left_table;

        out.length_re += norm(next - x);
        x = next;
        t = direction(x);
        if (norm2(t) == 0.0) return TraceStop::no_field;
        radius = next_radius;
        if (radius < out.min_radius_re) out.min_radius_re = radius;
        out.points_re.push_back(x);

        // Closure on the seed itself, which the reversal counter structurally cannot see: a **circular** line has
        // a constant radius, so there is no radial reversal to count and the walk would run to `max_points`
        // around a curve it had already closed. The minimum length is what keeps this from firing on the first
        // step's own neighbourhood.
        //
        // Half a step rather than a whole one, and the difference is measurable on a circle: the curve is stopped
        // at the first point that comes back within the threshold, so the traced length is short of the true
        // perimeter by up to that threshold. Measured on the radius-two circle of this kit's own case, half a step
        // is a shortfall of under a percent while a whole step is over one and a half.
        if (out.length_re > 4.0 * spec_.step_max_re && norm(x - seed_re) < 0.5 * step) {
            return TraceStop::looped;
        }

        // Closure by counting reversals of the radial direction: a closed dipole line crosses the equator twice a
        // lap, so more than four reversals means the curve is circling without coming back to where it started.
        //
        // The **deadband** is not tuning. On a line whose radius is nearly constant -- the circular case above,
        // or any line near its own radial turning point -- `dr` is the difference of two nearly equal numbers and
        // its sign is rounding noise, so a counter that read every sign change would report `looped` after five
        // steps of a perfectly good curve. Only a change in the *significant* radial motion counts, where
        // significant is a thousandth of the step: three orders below any real turning of a field line, and well
        // above the noise of a double-precision difference of order-one numbers.
        const double dr = radius - previous_radius;
        if (std::abs(dr) > step * kRadialDeadband) {
            if (previous_dr * dr < 0.0) ++reversals;
            previous_dr = dr;
            if (reversals > kMaxReversals) return TraceStop::looped;
        }
    }
    return TraceStop::max_points;
}

FieldLine FieldTracer::trace(const Vec3& seed_re, bool both_ways) const {
    FieldLine out;
    if (!usable_) return out;
    if (!both_ways) {
        out.stop = walk(seed_re, -1.0, out);
        return out;
    }

    // Both directions, joined at the seed. A field line has no orientation, so a one-directional trace would
    // draw half a shell and leave a reader to guess where the other half went; the seed appears once, from the
    // forward half.
    FieldLine forward;
    FieldLine backward;
    const TraceStop forward_stop = walk(seed_re, 1.0, forward);
    const TraceStop backward_stop = walk(seed_re, -1.0, backward);

    out.points_re.reserve(forward.points_re.size() + backward.points_re.size());
    for (std::size_t i = backward.points_re.size(); i > 1; --i) {
        out.points_re.push_back(backward.points_re[i - 1]);
    }
    for (const Vec3& point : forward.points_re) out.points_re.push_back(point);
    out.length_re = forward.length_re + backward.length_re;
    out.min_radius_re = std::min(forward.min_radius_re, backward.min_radius_re);

    // The more informative of the two reasons, in a fixed order rather than "whichever ran second": reaching the
    // surface says the line is a closed shell line, a loop says it closes on itself, leaving the table says the
    // samples ran out, a vanished field says the model does, and a budget says only that the picture is big.
    // `left_table` outranks `no_field` because a line that ran off the edge of the bake is a statement about the
    // grid, which a caller can act on, while a zero field inside the box is a statement about the model.
    const auto rank = [](TraceStop stop) {
        switch (stop) {
            case TraceStop::hit_surface: return 0;
            case TraceStop::looped: return 1;
            case TraceStop::left_table: return 2;
            case TraceStop::no_field: return 3;
            case TraceStop::max_points: return 4;
        }
        return 5;
    };
    out.stop = rank(forward_stop) <= rank(backward_stop) ? forward_stop : backward_stop;
    return out;
}

}  // namespace qp::plugins::magnetosphere
