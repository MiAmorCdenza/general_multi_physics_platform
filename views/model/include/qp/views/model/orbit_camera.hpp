/**
 * @file orbit_camera.hpp
 * @brief Where the user is looking from, and what that does to a point.
 *
 * A camera is three numbers -- azimuth, elevation, distance -- wrapped around the basis `scene_projection.hpp`
 * already defines, plus a perspective divide. It is deliberately **not** a matrix stack: this platform draws points
 * and polylines, and a camera that can express a full transform hierarchy would be a camera with no caller.
 *
 * ## The target plane, and why the scale is what it is
 *
 * The camera sits at `c * distance` (see `screen_basis` for `c`) and looks at the origin. A point `p` has
 *
 *     w = distance - dot(c, p)          // how far in front of the camera it is
 *     screen = (dot(p, right), dot(p, up)) * distance / w
 *
 * so **a point with `dot(c, p) == 0` -- one lying in the target plane -- projects exactly where the orthographic
 * projection puts it**. Two consequences worth stating: the fit computed from the scene's box stays valid at the
 * target distance (a scene does not jump when the third coordinate arrives), and a case can assert a coordinate
 * rather than a pixel.
 *
 * A point with `w <= 0` is **behind the camera** and has no screen position; `visible` is how a caller asks, rather
 * than a division that produces a mirrored point the user cannot explain.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `elevation` is in (-90, 90) and `distance` is within `[kMinDistance, kMaxDistance]`
 * @errors      noexcept
 * @frozen      no
 * @tests       views.camera.a_point_in_the_target_plane_projects_like_the_flat_view,
 *              views.camera.orbiting_wraps_and_zooming_clamps
 */
#pragma once

#include <qp/graph/domain/view_items.hpp>
#include <qp/views/model/scene_projection.hpp>

#include <utility>

namespace qp::views::model {

/// The closest a user may pull the camera, in multiples of the scene's fitted half-width.
inline constexpr double kMinCameraDistanceFactor = 0.2;
/// The furthest, for the same reason: past this the content is a dot and the orbit feels broken.
inline constexpr double kMaxCameraDistanceFactor = 20.0;
/// The distance used when a scene does not name one, in multiples of its fitted half-width.
inline constexpr double kDefaultCameraDistanceFactor = 3.0;
/// How much one wheel notch changes the distance.
inline constexpr double kZoomStep = 1.15;

/**
 * @brief A camera that orbits the origin.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The view it reports is always a legal one: finite angles, elevation inside (-90, 90), distance
 *              inside the clamps
 * @errors      noexcept
 * @frozen      no
 * @tests       views.camera.orbiting_wraps_and_zooming_clamps
 */
class OrbitCamera final {
public:
    /**
     * @brief A camera looking from `start`, at the distance the view asks for or the default.
     *
     * @param start The view the scene asked for. Its `distance` of zero means "the host decides", which is the
     *              documented meaning of zero and the reason the default factor exists.
     * @param half  The scene's fitted half-width, which the distance is measured in. Zero or non-finite is treated
     *              as one, so a scene whose bounds mean nothing still has a camera rather than a division by zero.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        `view()` is `start` with its azimuth wrapped into [0, 360), its elevation clamped inside the
     *              poles, and its distance either kept or replaced by the default multiple of `half`
     * @invariant   The camera is immediately usable: every number is finite and the distance is inside the clamps
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.orbiting_wraps_and_zooming_clamps
     */
    explicit OrbitCamera(const qp::graph::ViewScene::View& start = {}, double half = 1.0) noexcept;

    /**
     * @brief The direction and distance, as a scene would state them.
     *
     * @ownership   value
     * @thread      any
     * @pre         none
     * @post        A view whose numbers are the camera's own
     * @invariant   The same call gives the same view
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.orbiting_wraps_and_zooming_clamps
     */
    [[nodiscard]] const qp::graph::ViewScene::View& view() const noexcept { return view_; }

    /**
     * @brief The screen basis this camera's direction implies.
     *
     * @ownership   value
     * @thread      any
     * @pre         none
     * @post        The basis of `view().azimuth_deg` and `view().elevation_deg`
     * @invariant   Orthonormal and finite, at every angle including the poles
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.a_point_in_the_target_plane_projects_like_the_flat_view
     */
    [[nodiscard]] ScreenBasis basis() const noexcept { return screen_basis(view_); }

    /**
     * @brief How far the camera is from the origin, in scene units.
     *
     * @ownership   value
     * @thread      any
     * @pre         none
     * @post        A positive finite number inside the clamps
     * @invariant   Never zero: a camera at the origin has no view direction left
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.orbiting_wraps_and_zooming_clamps
     */
    [[nodiscard]] double distance() const noexcept { return view_.distance; }

    /**
     * @brief The fitted half-width the distance is measured in.
     *
     * @ownership   value
     * @thread      any
     * @pre         none
     * @post        A positive finite number
     * @invariant   The value the constructor was given, or one when that was unusable
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.a_point_in_the_target_plane_projects_like_the_flat_view
     */
    [[nodiscard]] double half_width() const noexcept { return half_; }

    /**
     * @brief Turns the camera by an increment, in degrees.
     *
     * Azimuth **wraps** -- an orbit is a circle and a user who keeps dragging must not hit a wall at 360 -- and
     * elevation **clamps** just short of the poles, where the direction is still defined but the screen's up would
     * flip. The clamp is one degree inside, because the basis's fallback at the pole points the screen a different
     * way and a picture that flips is worse than a picture that stops.
     *
     * @param delta_azimuth_deg   Degrees to turn around the polar axis. Any finite value; the result is wrapped.
     * @param delta_elevation_deg Degrees to lift the camera. Any finite value; the result is clamped.
     *
     * @ownership   value
     * @thread      main
     * @pre         none
     * @post        `azimuth` is in [0, 360) and `elevation` is in (-90, 90)
     * @invariant   The distance is unchanged
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.orbiting_wraps_and_zooming_clamps
     */
    void orbit(double delta_azimuth_deg, double delta_elevation_deg) noexcept;

    /**
     * @brief Moves the camera in or out by a factor.
     *
     * A factor that is not a positive finite number is **ignored** rather than applied: a wheel event with no
     * notches, or a caller's NaN, must not leave the camera at an unusable distance.
     *
     * @param factor The multiple of the current distance to move to. Greater than one pulls back.
     *
     * @ownership   value
     * @thread      main
     * @pre         none
     * @post        `distance` is clamped into `[0.2, 20] * half_width`
     * @invariant   The direction is unchanged
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.orbiting_wraps_and_zooming_clamps
     */
    void zoom(double factor) noexcept;

    /**
     * @brief Puts the camera at a distance, clamped. For a caller that computed one from a scene's bounds.
     *
     * @param distance The wanted distance in scene units. A non-finite value becomes the default distance rather
     *                 than a NaN the projection would propagate into every point.
     *
     * @ownership   value
     * @thread      main
     * @pre         none
     * @post        `distance` is inside the clamps and finite
     * @invariant   The direction is unchanged
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.orbiting_wraps_and_zooming_clamps
     */
    void set_distance(double distance) noexcept;

    /**
     * @brief Where a point lands, with perspective.
     *
     * The scale is the target-plane scale: a point in that plane projects exactly as `project_orthographic` would.
     * For a point behind the camera the answer is `(0, 0)` -- a caller that needs to know asks `visible`, because a
     * position is not the right place to encode "there is none".
     *
     * @param point The point, in the scene's own units.
     *
     * @ownership   value
     * @thread      any
     * @pre         none
     * @post        A finite pair, or `(0, 0)` when the point is not in front of the camera
     * @invariant   Depends only on the point and the camera's current view
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.a_point_in_the_target_plane_projects_like_the_flat_view
     */
    [[nodiscard]] std::pair<double, double> project(const qp::graph::ViewScene::Point& point) const noexcept;

    /**
     * @brief How far in front of the camera a point is. Negative means behind it.
     *
     * The value a painter sorts by, and the reason a three-dimensional scene needs one: without it the far side of a
     * ring is drawn over the near side, which reads as a rendering bug rather than as depth.
     *
     * @param point The point, in the scene's own units.
     *
     * @ownership   value
     * @thread      any
     * @pre         none
     * @post        `distance - dot(camera_direction, point)`
     * @invariant   Positive exactly in front of the camera
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.a_point_in_the_target_plane_projects_like_the_flat_view
     */
    [[nodiscard]] double depth(const qp::graph::ViewScene::Point& point) const noexcept;

    /**
     * @brief Whether a point is in front of the camera at all.
     *
     * @param point The point, in the scene's own units.
     *
     * @ownership   pure
     * @thread      any
     * @pre         none
     * @post        True exactly when `depth(point) > 0` and finite
     * @invariant   `project` returns `(0, 0)` for every point this answers false for
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       views.camera.a_point_in_the_target_plane_projects_like_the_flat_view
     */
    [[nodiscard]] bool visible(const qp::graph::ViewScene::Point& point) const noexcept;

private:
    qp::graph::ViewScene::View view_{};
    double half_ = 1.0;
};

}  // namespace qp::views::model
