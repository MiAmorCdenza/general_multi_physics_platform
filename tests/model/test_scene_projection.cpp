/**
 * @file test_scene_projection.cpp
 * @brief The one definition of "where does the camera sit", asserted where it can be asserted without a window.
 *
 * Two hosts draw a `ViewScene`: the orthographic widget in `views/qt` and the three-dimensional one that is being
 * built beside it. They share this arithmetic, so a scene cannot open in two different orientations depending on
 * which panel it landed in -- which is the failure a second, hand-written basis would have produced, and the one
 * this file exists to prevent.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/orbit_camera.hpp>
#include <qp/views/model/scene_projection.hpp>

#include <cmath>
#include <vector>

namespace {

using qp::graph::ViewScene;
using qp::views::model::fitted_half_width;
using qp::views::model::project_orthographic;
using qp::views::model::screen_basis;
using qp::views::model::OrbitCamera;

constexpr double kTolerance = 1.0e-12;

[[nodiscard]] double dot(double ax, double ay, double az, double bx, double by, double bz) noexcept {
    return ax * bx + ay * by + az * bz;
}

/// @brief A scene whose box is the cube `[-half, half]^3`.
[[nodiscard]] ViewScene cube(double half) {
    ViewScene scene;
    scene.x_min = -half;
    scene.x_max = half;
    scene.y_min = -half;
    scene.y_max = half;
    scene.z_min = -half;
    scene.z_max = half;
    scene.has_bounds = true;
    return scene;
}

}  // namespace

TEST_CASE("views.camera.a_point_in_the_target_plane_projects_like_the_flat_view", "[views][camera]") {
    // **The convention that makes a perspective camera testable**: the projection is scaled by the distance to the
    // target plane, so a point lying in the plane through the origin perpendicular to the camera projects exactly
    // where the orthographic projection puts it. Two consequences the rest of the window depends on -- a scene does
    // not jump when a camera arrives, and the fit computed from the scene's box stays valid at the target distance.
    for (const ViewScene::View& start : {ViewScene::View{0.0, 90.0, 0.0}, ViewScene::View{-90.0, 0.0, 0.0},
                                         ViewScene::View{35.0, 25.0, 0.0}}) {
        const qp::views::model::OrbitCamera camera{start, 5.0};
        const qp::views::model::ScreenBasis basis = camera.basis();
        // A point in the target plane: its component along the camera direction is zero by construction.
        const double along = 0.0;
        const ViewScene::Point point{2.0 * basis.right_x + 1.0 * basis.up_x + along * basis.cam_x,
                                     2.0 * basis.right_y + 1.0 * basis.up_y + along * basis.cam_y,
                                     2.0 * basis.right_z + 1.0 * basis.up_z + along * basis.cam_z};
        const std::pair<double, double> flat = project_orthographic(basis, point);
        const std::pair<double, double> deep = camera.project(point);
        REQUIRE(std::abs(deep.first - flat.first) < 1.0e-12);
        REQUIRE(std::abs(deep.second - flat.second) < 1.0e-12);

        // **In front projects larger, behind is not projected at all.** A point halfway to the camera is magnified by
        // exactly two, because the scale is `distance / depth` and the depth is half the distance.
        const ViewScene::Point nearer{point.x + 0.5 * camera.distance() * basis.cam_x,
                                      point.y + 0.5 * camera.distance() * basis.cam_y,
                                      point.z + 0.5 * camera.distance() * basis.cam_z};
        const std::pair<double, double> close = camera.project(nearer);
        REQUIRE(std::abs(close.first - 2.0 * flat.first) < 1.0e-9);
        REQUIRE(std::abs(close.second - 2.0 * flat.second) < 1.0e-9);
        REQUIRE(camera.depth(nearer) < camera.depth(point));

        // **Beyond the camera, not merely on the far side of the origin.** The first version of this put the point
        // at `-2 * distance` along the camera axis, which is a point the camera is still looking at -- further away
        // than the origin, and visible. "Behind" means the depth is negative, and the depth is
        // `distance - dot(cam, p)`, so the point has to be more than one distance *towards* the camera.
        const ViewScene::Point behind{2.0 * camera.distance() * basis.cam_x,
                                      2.0 * camera.distance() * basis.cam_y,
                                      2.0 * camera.distance() * basis.cam_z};
        REQUIRE_FALSE(camera.visible(behind));
        REQUIRE(camera.depth(behind) < 0.0);
        // The position of an unprojectable point is the origin rather than a mirrored coordinate: a caller that
        // needs to know asks `visible`, because a *position* is the wrong place to encode "there is none".
        const std::pair<double, double> none = camera.project(behind);
        REQUIRE(none.first == 0.0);
        REQUIRE(none.second == 0.0);
    }
}

TEST_CASE("views.camera.orbiting_wraps_and_zooming_clamps", "[views][camera]") {
    // A camera is three numbers and three rules, and each rule is here because the alternative is a user stuck:
    // azimuth **wraps** (an orbit is a circle, and a drag past 360 must not hit a wall), elevation **clamps** one
    // degree inside the poles (the basis's fallback there points the screen the other way, so a picture that flipped
    // would be worse than one that stopped), and distance is **clamped** at both ends of a plausible range.
    const qp::views::model::OrbitCamera start{ViewScene::View{10.0, 20.0, 0.0}, 4.0};
    REQUIRE(start.view().azimuth_deg == 10.0);
    REQUIRE(start.view().elevation_deg == 20.0);
    // A view with no distance gets the default multiple of the scene's half-width: zero means "the host decides",
    // which is what the scene's own contract says a zero distance means.
    REQUIRE(std::abs(start.distance() - qp::views::model::kDefaultCameraDistanceFactor * 4.0) < 1.0e-12);

    qp::views::model::OrbitCamera camera = start;
    camera.orbit(350.0, 0.0);
    REQUIRE(std::abs(camera.view().azimuth_deg - 0.0) < 1.0e-12);   // wrapped, not 360
    camera.orbit(-20.0, 0.0);
    REQUIRE(std::abs(camera.view().azimuth_deg - 340.0) < 1.0e-12); // and from the other side
    camera.orbit(0.0, 1000.0);
    REQUIRE(camera.view().elevation_deg < 90.0);
    REQUIRE(camera.view().elevation_deg == 89.0);
    camera.orbit(0.0, -1000.0);
    REQUIRE(camera.view().elevation_deg == -89.0);
    // The basis stays orthonormal at the clamp, which is the reason the clamp exists.
    const qp::views::model::ScreenBasis basis = camera.basis();
    REQUIRE(std::isfinite(basis.up_z));
    REQUIRE(std::abs(basis.up_z - 1.0) > 0.0);

    // Zooming: in and out by a factor, and the two clamps.
    camera.set_distance(4.0);
    camera.zoom(2.0);
    REQUIRE(std::abs(camera.distance() - 8.0) < 1.0e-12);
    camera.zoom(0.5);
    REQUIRE(std::abs(camera.distance() - 4.0) < 1.0e-12);
    camera.zoom(1000.0);
    REQUIRE(std::abs(camera.distance() - qp::views::model::kMaxCameraDistanceFactor * 4.0) < 1.0e-12);
    camera.zoom(1.0 / 100000.0);
    REQUIRE(std::abs(camera.distance() - qp::views::model::kMinCameraDistanceFactor * 4.0) < 1.0e-12);
    // A factor that is not a positive number is ignored rather than applied: a wheel event of zero notches, or a
    // NaN from a caller, must not leave the camera at an unusable distance.
    const double before = camera.distance();
    camera.zoom(0.0);
    camera.zoom(-1.0);
    camera.zoom(std::nan(""));
    REQUIRE(camera.distance() == before);
    camera.set_distance(std::nan(""));
    REQUIRE(camera.distance() > 0.0);

    // A half-width of zero or a non-finite one falls back to one, so a scene with no bounds still has a camera.
    const qp::views::model::OrbitCamera degenerate{ViewScene::View{0.0, 90.0, 0.0}, 0.0};
    REQUIRE(degenerate.half_width() == 1.0);
    REQUIRE(degenerate.distance() > 0.0);
}

TEST_CASE("views.scene.the_basis_is_orthonormal_and_survives_the_poles", "[views][scene]") {
    // The basis is the whole convention: everything a user sees is `dot(point, right)` and `dot(point, up)`. It has
    // to be orthonormal for a picture not to be sheared, and it has to stay finite **at the poles**, where the
    // natural definition `right = normalize(z_hat x camera)` divides by zero.
    const std::vector<ViewScene::View> views{
        {0.0, 90.0, 0.0},     // the equatorial plane, looked down at
        {-90.0, 0.0, 0.0},    // the meridional plane, looked at from -y
        {90.0, 0.0, 0.0},     // ... and from +y
        {0.0, 0.0, 0.0},      // in the equatorial plane, edge on
        {45.0, 35.0, 0.0},    // a general tilt
        {33.0, -70.0, 0.0},   // below the plane
        {0.0, -90.0, 0.0},    // the south pole, the other division by zero
    };
    for (const ViewScene::View& view : views) {
        const qp::views::model::ScreenBasis basis = screen_basis(view);
        const double cam = std::sqrt(dot(basis.cam_x, basis.cam_y, basis.cam_z, basis.cam_x, basis.cam_y,
                                         basis.cam_z));
        const double right = std::sqrt(dot(basis.right_x, basis.right_y, basis.right_z, basis.right_x,
                                           basis.right_y, basis.right_z));
        const double up = std::sqrt(dot(basis.up_x, basis.up_y, basis.up_z, basis.up_x, basis.up_y, basis.up_z));
        INFO("azimuth " << view.azimuth_deg << ", elevation " << view.elevation_deg);
        REQUIRE(std::abs(cam - 1.0) < kTolerance);
        REQUIRE(std::abs(right - 1.0) < kTolerance);
        REQUIRE(std::abs(up - 1.0) < kTolerance);
        // Perpendicular in all three pairs: this is what "orthonormal" buys, and a shear is exactly a non-zero dot.
        REQUIRE(std::abs(dot(basis.cam_x, basis.cam_y, basis.cam_z, basis.right_x, basis.right_y, basis.right_z)) <
                kTolerance);
        REQUIRE(std::abs(dot(basis.cam_x, basis.cam_y, basis.cam_z, basis.up_x, basis.up_y, basis.up_z)) <
                kTolerance);
        REQUIRE(std::abs(dot(basis.right_x, basis.right_y, basis.right_z, basis.up_x, basis.up_y, basis.up_z)) <
                kTolerance);
    }

    // At the poles the fallback is `+x` to the right, which is what a user looking straight down the axis expects;
    // the alternative is a NaN basis that turns every coordinate into a NaN and paints nothing, without saying why.
    const qp::views::model::ScreenBasis north = screen_basis(ViewScene::View{17.0, 90.0, 0.0});
    REQUIRE(std::abs(north.right_x - 1.0) < kTolerance);
    REQUIRE(std::abs(north.right_y) < kTolerance);
    // ... and at the south pole the screen's up is -y, because the camera is upside down relative to the north one.
    const qp::views::model::ScreenBasis south = screen_basis(ViewScene::View{17.0, -90.0, 0.0});
    // The **y** component, not the z one: from the south pole the screen's up is `-y`, and the first version of this
    // assertion named the wrong component while its own sentence said the right thing.
    REQUIRE(std::abs(south.up_y + 1.0) < kTolerance);
    REQUIRE(std::abs(south.up_z) < kTolerance);

    // A scene is content, so an item can hand this function anything: a non-finite angle falls back to the identity
    // basis rather than propagating a NaN into every point of every curve.
    const qp::views::model::ScreenBasis broken =
        screen_basis(ViewScene::View{std::nan(""), std::nan(""), 0.0});
    REQUIRE(std::isfinite(broken.cam_z));
    REQUIRE(std::isfinite(broken.right_x));
    REQUIRE(std::isfinite(broken.up_y));
    REQUIRE(std::abs(broken.cam_z - 1.0) < kTolerance);
}

TEST_CASE("views.scene.the_two_default_views_are_the_two_planes", "[views][scene]") {
    // **The change that added a third coordinate had to be invisible, and this is the assertion that says so.**
    // The particle item has always drawn the equatorial plane (`x` right, `y` up) and the field-line item the
    // meridional one (`x` right, `z` up). Before the scene had a `z`, each achieved that by *writing the two
    // coordinates it wanted into a two-component point*; now both write the real point and state a view, and the
    // two projections below are exactly the two pictures the items used to produce.
    const qp::views::model::ScreenBasis equatorial = screen_basis(ViewScene::View{0.0, 90.0, 0.0});
    const std::pair<double, double> down = project_orthographic(equatorial, ViewScene::Point{1.0, 2.0, 3.0});
    REQUIRE(std::abs(down.first - 1.0) < kTolerance);      // x to the right
    REQUIRE(std::abs(down.second - 2.0) < kTolerance);     // y up
    // `z` is what the equatorial view drops -- and it drops it by being perpendicular to the screen, not by being
    // absent from the point.

    const qp::views::model::ScreenBasis meridional = screen_basis(ViewScene::View{-90.0, 0.0, 0.0});
    const std::pair<double, double> side = project_orthographic(meridional, ViewScene::Point{1.0, 2.0, 3.0});
    REQUIRE(std::abs(side.first - 1.0) < kTolerance);      // x to the right
    REQUIRE(std::abs(side.second - 3.0) < kTolerance);     // z up: the plane the field-line item used to write
    // ... and this time it is `y` that the view drops, which is why a trace seeded in the equatorial plane draws a
    // proper curve from this direction and a degenerate line from the other.
    // At azimuth 0 the camera sits on **+x**, so the screen shows y to the right and z up -- the first version
    // of this assertion expected x to the right, which is the azimuth **-90** convention the meridional view uses.
    // Getting that wrong is how a convention drifts: it is written here so the next reader cannot.
    const std::pair<double, double> edge_on = project_orthographic(
        screen_basis(ViewScene::View{0.0, 0.0, 0.0}), ViewScene::Point{1.0, 2.0, 3.0});
    REQUIRE(std::abs(edge_on.first - 2.0) < kTolerance);
    REQUIRE(std::abs(edge_on.second - 3.0) < kTolerance);

    // **The fit is the box through the basis, and a box is not the same size from every direction.** Looking along
    // a face normal the silhouette is a square and the half-width is exactly the box's; looking along a body
    // diagonal it is a hexagon and the half-width grows. The first version of this assertion claimed the half-width
    // was direction-independent, and the measurement says otherwise:
    //
    //     azimuth   elevation   half-width of a +-6 cube
    //        0          90          6.000000      (face on: the equatorial plane)
    //      -90           0          6.000000      (face on: the meridional plane)
    //       45          35          9.781870      (corner on: sqrt(3) times as much, which is the maximum)
    //
    // So the two views the items actually ask for are the two that fit exactly, and a camera host that orbits away
    // from them **zooms out** slightly rather than clipping. That is the behaviour worth stating, and it is stated
    // as the inequality it is.
    const ViewScene::View face_on[2] = {ViewScene::View{0.0, 90.0, 0.0}, ViewScene::View{-90.0, 0.0, 0.0}};
    for (const ViewScene::View& view : face_on) {
        const double half = fitted_half_width(cube(6.0), screen_basis(view), 0.001);
        REQUIRE(std::abs(half - 6.0) < 1.0e-9);
    }
    const double corner_on = fitted_half_width(cube(6.0), screen_basis(ViewScene::View{45.0, 35.0, 0.0}), 0.001);
    REQUIRE(std::abs(corner_on - 9.781870) < 1.0e-6);
    // Neither smaller than the box nor larger than its diagonal: the two bounds a fit can never cross.
    REQUIRE(corner_on > 6.0);
    REQUIRE(corner_on < 6.0 * std::sqrt(3.0) + 1.0e-9);
    // A floor, so a one-point scene still has a scale rather than a degenerate box.
    ViewScene tiny;
    tiny.has_bounds = true;
    tiny.x_max = 0.0;
    REQUIRE(std::abs(fitted_half_width(tiny, screen_basis(ViewScene::View{0.0, 90.0, 0.0}), 1.0) - 1.0) < kTolerance);
    // No bounds at all: the floor is the answer, and no corner is read.
    ViewScene none;
    REQUIRE(std::abs(fitted_half_width(none, screen_basis(ViewScene::View{0.0, 90.0, 0.0}), 2.5) - 2.5) < kTolerance);
}

TEST_CASE("views.scene.a_ring_current_looks_like_a_ring", "[views][scene]") {
    // **Why the view belongs in the scene rather than in the host.** A ring of particles in the equatorial plane is
    // a ring from above and a line from the side; a family of meridional field lines is the opposite. The host
    // cannot know which it has -- it would have to know what a ring current is -- so the item states it, and this
    // case is the measurement of that argument: the same points, two views, a ring and a line.
    constexpr int kPoints = 32;
    constexpr double kRadius = 5.0;
    std::vector<ViewScene::Point> ring;
    for (int i = 0; i < kPoints; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / kPoints;
        ring.push_back(ViewScene::Point{kRadius * std::cos(angle), kRadius * std::sin(angle), 0.0});
    }

    // From the equatorial view the projected points are all at the same distance from the origin: a circle.
    const qp::views::model::ScreenBasis above = screen_basis(ViewScene::View{0.0, 90.0, 0.0});
    for (const ViewScene::Point& point : ring) {
        const std::pair<double, double> screen = project_orthographic(above, point);
        REQUIRE(std::abs(std::hypot(screen.first, screen.second) - kRadius) < 1.0e-9);
    }

    // From the edge-on view every projected point lies on the horizontal axis: the ring has become a segment, which
    // is exactly the picture the field-line item would produce if it were drawn from the particle item's view -- the
    // reason the two items state different views rather than sharing one.
    const qp::views::model::ScreenBasis edge = screen_basis(ViewScene::View{0.0, 0.0, 0.0});
    double widest = 0.0;
    for (const ViewScene::Point& point : ring) {
        const std::pair<double, double> screen = project_orthographic(edge, point);
        REQUIRE(std::abs(screen.second) < 1.0e-9);
        widest = std::max(widest, std::abs(screen.first));
    }
    REQUIRE(std::abs(widest - kRadius) < 1.0e-9);

    // A tilted ring -- which is what a real ring current is, and what the particle item could not draw before the
    // scene had a `z` -- keeps looking like a ring from above, with its vertical extent visible:
    const double tilt = 0.2;   // radians
    std::vector<ViewScene::Point> tilted;
    for (const ViewScene::Point& point : ring) {
        tilted.push_back(ViewScene::Point{point.x, point.y * std::cos(tilt), point.y * std::sin(tilt)});
    }
    double z_extent = 0.0;
    for (const ViewScene::Point& point : tilted) z_extent = std::max(z_extent, std::abs(point.z));
    REQUIRE(z_extent > 0.5);
    // From above, a tilted ring is still a closed curve around the origin.
    for (const ViewScene::Point& point : tilted) {
        const std::pair<double, double> screen = project_orthographic(above, point);
        REQUIRE(std::hypot(screen.first, screen.second) > 0.5);
    }
}

