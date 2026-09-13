/**
 * @file dipole.hpp
 * @brief The geomagnetic field as a tilted dipole: the arithmetic, and the tilt.
 *
 * ## The field this describes
 *
 * In the dipole's own frame -- `z` along the magnetic moment, `x` through the Greenwich meridian -- the field of
 * a moment `m` along `+z` is
 *
 *     B(r) = (mu0 / 4pi) * [ 3 r (m . r) / |r|^5  -  m / |r|^3 ]
 *
 * which for `m = m zhat` reduces to the three components this file computes:
 *
 *     B_x = 3 C x z / r^5
 *     B_y = 3 C y z / r^5
 *     B_z = C (3 z^2 - r^2) / r^5
 *
 * with `C = (mu0 / 4pi) * m`.
 *
 * ## The sign, which is the part to read twice
 *
 * The moment is `-kDipoleMomentAm2 * zhat` with no tilt: it points **south**. That is not a convention this file
 * invented -- the geomagnetic dipole's moment really does point south, which is why a compass needle's north pole
 * points north -- and it is why the field at the magnetic equator has a **negative** `z` component in a frame
 * with `z` up. `geomagnetic.hpp` derives the moment with that sign from the IGRF `g10`, and this file rotates it.
 *
 * Getting it backwards is the most consequential mistake available here. A proton gyrates about a field line with
 * `omega = qB/m`, so reversing `B` reverses the gyration; the gradient and curvature drifts reverse with it; and
 * the symptom -- a proton drifting east instead of west, an electron the other way -- appears thousands of time
 * steps later as a ring current going the wrong way round the planet. No test of the field **magnitude** would
 * see it, which is why `magnetosphere.dipole.points_south_at_the_equator` asserts the sign directly and why the
 * particle test asserts the drift direction.
 *
 * ## The tilt
 *
 * The magnetic poles are not the rotation poles. `tilt_degrees` rotates about the `y` axis, so a point is rotated
 * **into** the dipole frame, the field is evaluated there, and the result is rotated back:
 *
 *     r_dipole = Ry(tilt) * r        Ry(a) = [ cos a, 0, sin a;  0, 1, 0;  -sin a, 0, cos a ]
 *     B_geo    = Ry(-tilt) * B_dipole
 *
 * A rotation rather than a re-derived field, because a rotation is exact and a second formula is a second chance
 * to disagree with the first about a sign.
 *
 * ## What this deliberately is not
 *
 * Not an IGRF expansion, and the reopening condition is a **number** rather than a feeling:
 * `units::kNondipoleFraction` is about 11%, so a feature at the 10% level is the largest thing this model cannot
 * show. A course that needs the South Atlantic Anomaly, a storm-time field, or a ground station's field to three
 * digits needs a different field model -- and it arrives as another node type, not as a parameter on this one. A
 * dipole with eleven coefficients is a model nobody can reason about.
 *
 * @ownership   pure (a value type and a pure function)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Divergence-free and curl-free everywhere except the origin
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.dipole.matches_the_textbook_at_the_equator,
 *              magnetosphere.dipole.points_south_at_the_equator,
 *              magnetosphere.dipole.decays_as_the_cube,
 *              magnetosphere.dipole.is_axisymmetric_without_a_tilt,
 *              magnetosphere.dipole.the_tilt_rotates_the_field,
 *              magnetosphere.dipole.the_normalized_form_agrees_with_the_si_form
 */
#pragma once

#include <qp/plugins/magnetosphere/geometry.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

namespace qp::plugins::magnetosphere {

/**
 * @brief A centred dipole, with its moment pointing south unless a tilt says otherwise.
 *
 * @ownership   owns
 * @thread      any
 * @pre         `tilt_degrees` is finite
 * @post        none
 * @invariant   The same tilt and point give the same field
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.dipole.matches_the_textbook_at_the_equator
 */
struct DipoleField final {
    /// The tilt of the moment's axis from the rotation axis, in degrees. Rotates the axis from `+z` towards `+x`.
    double tilt_degrees = kMagneticTiltDegrees;
    /// The moment's magnitude, in ampere square metres. Overridable so a test can use a round number.
    double moment_am2 = kDipoleMomentAm2;

    /**
     * @brief The field at `point`, in tesla.
     *
     * @param point Where to evaluate, in **metres**, in the geographic frame.
     *
     * @ownership   pure
     * @thread      any
     * @pre         `point` is finite
     * @post        The dipole field there; **zero at the origin**, where the model is singular, because a
     *              diverging value would poison every average taken downstream of it
     * @invariant   The result is finite, and zero, for any finite input
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.dipole.matches_the_textbook_at_the_equator,
     *              magnetosphere.dipole.points_south_at_the_equator,
     *              magnetosphere.dipole.decays_as_the_cube
     */
    [[nodiscard]] Vec3 at(const Vec3& point) const noexcept;

    /**
     * @brief The same field in the normalized units the pusher runs in.
     *
     * A convenience rather than a second implementation: it converts the point, calls `at`, and converts the
     * result. Kept because the alternative -- every caller remembering two scale factors -- is how a pusher ends
     * up integrating a field that is `1e5` times too large, which produces a gyration far too fast to see and a
     * trajectory that reads as noise.
     *
     * @param point Where to evaluate, in **normalized length units**, in the geographic frame.
     *
     * @ownership   pure
     * @thread      any
     * @pre         `point` is finite
     * @post        `at(point / kNormalizedPerMetre) * kNormalizedPerTesla`
     * @invariant   Agrees with `at` to the rounding of the two conversions
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.dipole.the_normalized_form_agrees_with_the_si_form,
 *              magnetosphere.trace.every_point_of_a_dipole_line_is_the_closed_form,
 *              magnetosphere.field_nodes.the_node_bakes_the_closed_form_dipole
     */
    [[nodiscard]] Vec3 at_normalized(const Vec3& point) const noexcept;
};

}  // namespace qp::plugins::magnetosphere
