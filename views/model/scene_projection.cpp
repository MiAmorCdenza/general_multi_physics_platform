/**
 * @file scene_projection.cpp
 * @brief The arithmetic behind `scene_projection.hpp`, written so that the poles are a special case and not a bug.
 */
#include <qp/views/model/scene_projection.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace qp::views::model {
namespace {

constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

}  // namespace

ScreenBasis screen_basis(const qp::graph::ViewScene::View& view) noexcept {
    ScreenBasis basis;

    const double a = view.azimuth_deg * kDegreesToRadians;
    const double e = view.elevation_deg * kDegreesToRadians;
    const double ca = std::cos(a);
    const double sa = std::sin(a);
    const double ce = std::cos(e);
    const double se = std::sin(e);

    // The camera direction, from the two angles. Clamped rather than trusted: a scene is content, and an item with
    // a NaN angle would otherwise turn every point into a NaN and paint nothing at all.
    double cx = ce * ca;
    double cy = ce * sa;
    double cz = se;
    const double length = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (!(length > 0.0) || !std::isfinite(length)) {
        cx = 0.0;
        cy = 0.0;
        cz = 1.0;
    } else {
        cx /= length;
        cy /= length;
        cz /= length;
    }

    // right = normalize(z_hat x cam). At the poles that cross product vanishes, and the fallback is `+x`, which is
    // what a user looking straight down the axis expects to have to the right.
    double rx = -cy;
    double ry = cx;
    double rz = 0.0;
    double rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (!(rlen > 1.0e-12) || !std::isfinite(rlen)) {
        rx = 1.0;
        ry = 0.0;
        rz = 0.0;
        rlen = 1.0;
    }
    rx /= rlen;
    ry /= rlen;
    rz /= rlen;

    // up = cam x right, which is unit by construction because the two are perpendicular unit vectors.
    const double ux = cy * rz - cz * ry;
    const double uy = cz * rx - cx * rz;
    const double uz = cx * ry - cy * rx;

    basis.cam_x = cx;
    basis.cam_y = cy;
    basis.cam_z = cz;
    basis.right_x = rx;
    basis.right_y = ry;
    basis.right_z = rz;
    basis.up_x = ux;
    basis.up_y = uy;
    basis.up_z = uz;
    return basis;
}

std::pair<double, double> project_orthographic(const ScreenBasis& basis,
                                               const qp::graph::ViewScene::Point& point) noexcept {
    const double sx = point.x * basis.right_x + point.y * basis.right_y + point.z * basis.right_z;
    const double sy = point.x * basis.up_x + point.y * basis.up_y + point.z * basis.up_z;
    return {sx, sy};
}

double fitted_half_width(const qp::graph::ViewScene& scene, const ScreenBasis& basis, double half_min) noexcept {
    if (!scene.has_bounds || !(half_min > 0.0) || !std::isfinite(half_min)) return half_min;

    // The eight corners of the box, projected: the largest `|right|` and `|up|` among them is the half-width the
    // scene needs. Corners rather than points, because the box is what the item asked to be fitted -- a scene whose
    // points all sit near the origin but whose bounds say "twenty earth radii" wants twenty.
    const double xs[2] = {scene.x_min, scene.x_max};
    const double ys[2] = {scene.y_min, scene.y_max};
    const double zs[2] = {scene.z_min, scene.z_max};
    double half = half_min;
    for (const double x : xs) {
        for (const double y : ys) {
            for (const double z : zs) {
                const std::pair<double, double> projected =
                    project_orthographic(basis, qp::graph::ViewScene::Point{x, y, z});
                half = std::max(half, std::abs(projected.first));
                half = std::max(half, std::abs(projected.second));
            }
        }
    }
    if (!std::isfinite(half)) return half_min;
    return half;
}

}  // namespace qp::views::model
