/**
 * @file dipole.cpp
 * @brief Three components, a rotation in, and a rotation out.
 *
 * ## Why the tilt is two rotations rather than a rotated moment
 *
 * The tempting alternative is to rotate the moment vector once and generalise the three components to a dot
 * product and a cross product. It is the same arithmetic and it hides the sign: `m = -m0 zhat` rotated by an
 * angle is a vector whose direction has to be checked against a picture, where rotating the **point** and the
 * **result** keeps the field formula in its textbook form with `m` still pointing along `-z`. Every line below
 * can therefore be read against the equation in the header, which is the property that made this port
 * checkable in the first place.
 */
#include <qp/plugins/magnetosphere/dipole.hpp>

#include <cmath>

namespace qp::plugins::magnetosphere {
namespace {

/// @brief Rotate about `y` by `radians`, the frame change between the geographic and dipole axes.
///
/// The matrix is written out rather than looped, because it is three lines and a loop over a sparse matrix is
/// how a sign gets lost.
[[nodiscard]] Vec3 rotate_y(const Vec3& v, double radians) noexcept {
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return Vec3{c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
}

/**
 * @brief The field of a dipole whose moment points along `-z`, at `r`, in the moment's own frame.
 *
 * ## The closed form, and the ground truth it was checked against
 *
 *     B_x = 3 C x z / r^5
 *     B_y = 3 C y z / r^5
 *     B_z = C (3 z^2 - r^2) / r^5          with  C = (mu0/4pi) * m0
 *
 * This file's leading sign was wrong twice, in both directions, while it was being written -- and what settled
 * it is worth keeping, because no amount of reasoning did. `build/diag_curl.cpp` evaluates
 *
 *     A = (mu0/4pi) (m x rhat) / r^2          B = curl A
 *
 * by **central differences** at the equator, at both geographic poles and at a mid-latitude point, for a moment
 * pointing south and for one pointing north. The closed form above agrees with that to every printed digit in
 * all six cases, with `C = +(mu0/4pi) * m0`. A remembered textbook formula and physical intuition between them
 * gave three different answers; `B = curl A` gave one.
 *
 * ## The two directions it produces, which are neither intuitive nor arbitrary
 *
 * With the geomagnetic moment pointing **south**, and `z` up:
 *
 *   | where | `B_z` | which is |
 *   |---|---|---|
 *   | magnetic equator | negative | the field points **south**, which is what a compass feels and why a compass needle's north pole points north |
 *   | either geographic pole | **positive** | the field points **outward**, i.e. radially away from the Earth |
 *
 * The second row is the one that reads wrong and is not. A dipole's field is divergence-free, and "outward at
 * both poles" looks like a net flux until you notice that it is **radially outward on the axis** in both
 * hemispheres -- so the flux through a sphere is positive near the axis and negative near the equator, and the
 * two cancel. The divergence was measured at `7e-21` against a field scale of `6e-6`, i.e. zero to the
 * arithmetic, and the curl likewise.
 *
 * The practical consequence for this kit: on the axis the field points **away** from the origin in both
 * hemispheres, so a particle there is pushed **towards** the equatorial plane. That is the magnetic mirror, and
 * it is what traps the radiation belts. An implementation with either sign flipped would still conserve energy
 * in a uniform field test and would fail to trap anything -- which is why the sign is asserted at the poles.
 */
[[nodiscard]] Vec3 field_of_a_southern_moment(const Vec3& r, double m0) noexcept {
    const double r2 = norm2(r);
    if (!(r2 > 0.0)) {
        // The origin, or a non-finite input. Zero rather than a division by zero, and the guard is written as
        // `!(r2 > 0)` rather than `r2 <= 0` so that a NaN takes this branch too -- a NaN compares false against
        // every bound, which is the same trap `ClampPolicy::apply` documents one layer down.
        return Vec3{};
    }
    const double rn = std::sqrt(r2);
    const double r3 = r2 * rn;
    const double r5 = r2 * r3;
    // `+`, and the sign was decided by `build/diag_curl.cpp` -- `B = curl A` by central differences -- rather
    // than by reasoning, after reasoning had produced three different answers. See the comment above.
    const double c = kMu0OverFourPi * m0;
    return Vec3{3.0 * c * r.x * r.z / r5,
                3.0 * c * r.y * r.z / r5,
                c * (3.0 * r.z * r.z - r2) / r5};
}

}  // namespace

Vec3 DipoleField::at(const Vec3& point) const noexcept {
    if (!is_finite(point)) return Vec3{};

    const double tilt = tilt_degrees * 3.14159265358979323846 / 180.0;
    // Into the dipole frame, evaluate there, and back out. The two rotations are inverses, so a zero tilt
    // leaves the point and the field exactly as they were -- which is what makes the untilted cases assertable
    // without a tolerance.
    const Vec3 dipole_frame = rotate_y(point, tilt);
    const Vec3 in_dipole_frame = field_of_a_southern_moment(dipole_frame, moment_am2);
    return rotate_y(in_dipole_frame, -tilt);
}

Vec3 DipoleField::at_normalized(const Vec3& point) const noexcept {
    const Vec3 si = point * (1.0 / kNormalizedPerMetre);
    const Vec3 field = at(si);
    return field * kNormalizedPerTesla;
}

}  // namespace qp::plugins::magnetosphere
