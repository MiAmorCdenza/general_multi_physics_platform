/**
 * @file scene_projection.hpp
 * @brief Where a scene's points land, given the direction the item asked to be seen from.
 *
 * ## The convention, stated once
 *
 * A view is a direction and a screen basis derived from it:
 *
 *   - `azimuth_deg` turns around the polar axis, measured from `+x` toward `+y`;
 *   - `elevation_deg` lifts the camera above the equatorial plane, `0` in the plane and `+90` on the `+z` axis;
 *   - the camera sits at `c = (cos e cos a, cos e sin a, sin e) * distance` and looks at the origin;
 *   - **right** = `normalize(z_hat x c)`, **up** = `normalize(c x right)`.
 *
 * The two views this platform ships come out as the two pictures the items drew before the scene had a third
 * coordinate: `azimuth 0, elevation 90` (the particle item's equatorial plane, `+x` right and `+y` up) and
 * `azimuth -90, elevation 0` (the field-line item's meridional plane, `+x` right and `+z` up).
 *
 * The degenerate case is handled rather than refused: at `elevation = +-90` the camera is on the `+z` axis, where
 * `z_hat x c` vanishes, so `right` falls back to `+x` -- which is the orientation a user looking straight down
 * expects, and the alternative (a NaN basis that turns every coordinate into a NaN) is how a picture disappears
 * without saying why.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `right` and `up` are orthonormal and both perpendicular to the camera direction
 * @errors      noexcept
 * @frozen      no
 * @tests       views.scene.a_ring_current_looks_like_a_ring, views.scene.the_two_default_views_are_the_two_planes
 */
#pragma once

#include <qp/graph/domain/view_items.hpp>

namespace qp::views::model {

/**
 * @brief The screen basis a view implies: where the camera is, and which way is right and up.
 *
 * @ownership   value
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   The three vectors are finite and `right`/`up` are unit length for every finite input
 * @errors      noexcept
 * @frozen      no
 * @tests       views.scene.the_basis_is_orthonormal_and_survives_the_poles
 */
struct ScreenBasis final {
    /// Unit vector from the origin toward the camera. Its `z` is `sin(elevation)`.
    double cam_x = 0.0;
    double cam_y = 0.0;
    double cam_z = 1.0;
    /// Unit screen axes.
    double right_x = 1.0;
    double right_y = 0.0;
    double right_z = 0.0;
    double up_x = 0.0;
    double up_y = 1.0;
    double up_z = 0.0;
};

/**
 * @brief The basis for `view`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        The camera direction is the unit vector `(cos e cos a, cos e sin a, sin e)`
 * @invariant   Never returns a non-finite component, for any finite angles
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       views.scene.the_basis_is_orthonormal_and_survives_the_poles
 */
[[nodiscard]] ScreenBasis screen_basis(const qp::graph::ViewScene::View& view) noexcept;

/**
 * @brief Where one point lands on screen, in the scene's own units.
 *
 * An **orthographic** projection: the result is `(dot(p, right), dot(p, up))`, so equal lengths stay equal and a
 * test can assert a coordinate rather than a pixel. Perspective is the camera host's business and is applied there,
 * over this basis -- which is why the basis is what this function returns rather than a finished projection.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Depends only on the point and the basis
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       views.scene.the_two_default_views_are_the_two_planes
 */
[[nodiscard]] std::pair<double, double> project_orthographic(const ScreenBasis& basis,
                                                             const qp::graph::ViewScene::Point& point) noexcept;

/**
 * @brief The square that fits a scene's bounds, for a host that draws in a plane.
 *
 * The box's extent along `right` and `up` reversed into a square whose centre is the origin: the items place their
 * bounds around the origin by construction, and a host that fitted the *content* instead would rescale the picture
 * the moment one particle left the frame.
 *
 * @param scene    The scene whose bounds are wanted.
 * @param basis    The basis its view implies.
 * @param half_min A floor for the half-width, so a scene of one point still has a scale.
 *
 * @ownership   pure
 * @thread      any
 * @pre         `half_min` is positive and finite
 * @post        `half >= half_min` and finite
 * @invariant   The same scene and basis give the same square
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       views.scene.the_two_default_views_are_the_two_planes
 */
[[nodiscard]] double fitted_half_width(const qp::graph::ViewScene& scene, const ScreenBasis& basis,
                                       double half_min) noexcept;

}  // namespace qp::views::model
