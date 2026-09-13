/**
 * @file orbit_camera.cpp
 * @brief The camera's arithmetic: wrapping, clamping, and the perspective divide.
 */
#include <qp/views/model/orbit_camera.hpp>

#include <algorithm>
#include <cmath>

namespace qp::views::model {
namespace {

/// @brief One degree inside the poles, where the screen's up would flip.
constexpr double kElevationLimit = 89.0;

[[nodiscard]] double wrap_azimuth(double degrees) noexcept {
    if (!std::isfinite(degrees)) return 0.0;
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0) wrapped += 360.0;
    return wrapped;
}

[[nodiscard]] double clamp_elevation(double degrees) noexcept {
    if (!std::isfinite(degrees)) return 0.0;
    return std::clamp(degrees, -kElevationLimit, kElevationLimit);
}

}  // namespace

OrbitCamera::OrbitCamera(const qp::graph::ViewScene::View& start, double half) noexcept {
    half_ = (std::isfinite(half) && half > 0.0) ? half : 1.0;
    view_.azimuth_deg = wrap_azimuth(start.azimuth_deg);
    view_.elevation_deg = clamp_elevation(start.elevation_deg);
    view_.distance = start.distance;
    if (!(view_.distance > 0.0) || !std::isfinite(view_.distance)) {
        view_.distance = kDefaultCameraDistanceFactor * half_;
    }
    set_distance(view_.distance);
}

void OrbitCamera::orbit(double delta_azimuth_deg, double delta_elevation_deg) noexcept {
    view_.azimuth_deg = wrap_azimuth(view_.azimuth_deg + delta_azimuth_deg);
    view_.elevation_deg = clamp_elevation(view_.elevation_deg + delta_elevation_deg);
}

void OrbitCamera::zoom(double factor) noexcept {
    if (!(factor > 0.0) || !std::isfinite(factor)) return;
    set_distance(view_.distance * factor);
}

void OrbitCamera::set_distance(double distance) noexcept {
    const double low = kMinCameraDistanceFactor * half_;
    const double high = kMaxCameraDistanceFactor * half_;
    if (!std::isfinite(distance)) {
        view_.distance = kDefaultCameraDistanceFactor * half_;
        return;
    }
    view_.distance = std::clamp(distance, low, high);
}

std::pair<double, double> OrbitCamera::project(const qp::graph::ViewScene::Point& point) const noexcept {
    const ScreenBasis basis = this->basis();
    const double across = point.x * basis.right_x + point.y * basis.right_y + point.z * basis.right_z;
    const double up = point.x * basis.up_x + point.y * basis.up_y + point.z * basis.up_z;
    const double w = depth(point);
    if (!(w > 0.0) || !std::isfinite(w)) return {0.0, 0.0};
    // The target-plane scale: `distance / w` is 1 exactly at the target plane, which is what makes a point there
    // project where the orthographic projection puts it.
    const double scale = view_.distance / w;
    return {across * scale, up * scale};
}

double OrbitCamera::depth(const qp::graph::ViewScene::Point& point) const noexcept {
    const ScreenBasis basis = this->basis();
    const double along = point.x * basis.cam_x + point.y * basis.cam_y + point.z * basis.cam_z;
    return view_.distance - along;
}

bool OrbitCamera::visible(const qp::graph::ViewScene::Point& point) const noexcept {
    const double w = depth(point);
    return w > 0.0 && std::isfinite(w);
}

}  // namespace qp::views::model
