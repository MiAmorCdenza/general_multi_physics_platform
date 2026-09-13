/**
 * @file scene_view3d.cpp
 * @brief The camera's picture: a grid, curves sorted far to near, a body, and particles sized by distance.
 */
#include "scene_view3d.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace qp::views {
namespace {

/// @brief The margin kept between the fitted content and the widget's edge, in pixels.
constexpr double kMarginPixels = 12.0;
/// @brief The half-size of a particle's dot at the target distance, in pixels.
constexpr double kPointRadius = 4.0;
/// @brief How many lines the equatorial grid draws on each side of the origin.
constexpr int kGridLines = 6;
/// @brief The scene's backdrop. See `paintGL` for why it is painted rather than cleared with `glClear`.
const QColor kBackdrop{16, 20, 26};
/// @brief The label's colour, which has to read against that backdrop rather than against the window.
const QColor kLabel{170, 180, 195};

/// @brief A screen position with the depth it was drawn at.
struct Placed final {
    double x = 0.0;
    double y = 0.0;
    double depth = 0.0;
};

}  // namespace

SceneView3D::SceneView3D(QString empty_text, QWidget* parent)
    : QOpenGLWidget(parent), empty_text_(std::move(empty_text)) {
    setMinimumSize(240, 180);
    setMouseTracking(false);
}

void SceneView3D::set_scene(qp::graph::ViewScene scene) {
    scene_ = std::move(scene);
    refit_camera();
    update();
}

void SceneView3D::refit_camera() {
    const qp::views::model::ScreenBasis basis = qp::views::model::screen_basis(scene_.view);
    const double half = qp::views::model::fitted_half_width(scene_, basis, 1.0);
    camera_ = qp::views::model::OrbitCamera{scene_.view, half};
}

void SceneView3D::paintGL() {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    // **Clear, or the last frame stays and the next one lands on top of it.** `QWidget` hands a fresh backing store
    // to every `paintEvent`; a `QOpenGLWidget` hands over the same framebuffer, so a widget that draws without
    // clearing accumulates -- which is what a user saw, and why resizing the window appeared to fix it (a resize
    // re-creates the framebuffer, and only that was clearing anything).
    //
    // A dark backdrop rather than the window's light background: the flat panels next door are charts, and this one
    // is a scene with a grid, curves and depth-sorted points -- those read against a dark ground and disappear
    // against a light one.
    painter.fillRect(rect(), kBackdrop);

    drawn_points_ = 0;
    hidden_points_ = 0;
    drawn_segments_ = 0;

    if (scene_.empty() || !scene_.has_bounds) {
        painter.drawText(rect(), Qt::AlignCenter, empty_text_);
        return;
    }

    // The widget's pixels to scene units. The fit is the scene's own half-width, so the content occupies the same
    // fraction of the panel whatever the camera's distance is: pulling back shows more of the grid, not a smaller
    // picture of the same thing.
    const double usable = static_cast<double>(std::min(rect().width(), rect().height())) - 2.0 * kMarginPixels;
    const double half = camera_.half_width();
    const double pixels_per_unit = usable > 1.0 ? usable / (2.0 * half) : 1.0;
    const double centre_x = rect().center().x();
    const double centre_y = rect().center().y();

    const auto place = [&](const qp::graph::ViewScene::Point& point) {
        const std::pair<double, double> screen = camera_.project(point);
        return Placed{centre_x + screen.first * pixels_per_unit, centre_y - screen.second * pixels_per_unit,
                      camera_.depth(point)};
    };

    // First the equatorial grid, faintly: it is the depth cue, and it is the scene's own bounds rather than a
    // decoration -- what the user sees is the box the item asked to be fitted.
    painter.setPen(QPen(QColor(120, 130, 145, 110), 1.0));
    for (int i = -kGridLines; i <= kGridLines; ++i) {
        const double t = half * static_cast<double>(i) / kGridLines;
        const qp::graph::ViewScene::Point a1{t, -half, 0.0};
        const qp::graph::ViewScene::Point b1{t, half, 0.0};
        const qp::graph::ViewScene::Point a2{-half, t, 0.0};
        const qp::graph::ViewScene::Point b2{half, t, 0.0};
        for (const auto& pair : {std::pair<qp::graph::ViewScene::Point, qp::graph::ViewScene::Point>{a1, b1},
                                 std::pair<qp::graph::ViewScene::Point, qp::graph::ViewScene::Point>{a2, b2}}) {
            if (!camera_.visible(pair.first) || !camera_.visible(pair.second)) continue;
            painter.drawLine(QPointF{place(pair.first).x, place(pair.first).y},
                             QPointF{place(pair.second).x, place(pair.second).y});
        }
    }

    // The curves, **segment by segment in depth order**, so a near stretch of a field line covers a far one. A whole
    // polyline cannot be sorted -- it spans depths -- so the unit of sorting is the segment, and that is the price of
    // drawing lines with a painter instead of a depth buffer.
    struct Segment final {
        QPointF from;
        QPointF to;
        double depth = 0.0;
    };
    std::vector<Segment> segments;
    for (const std::vector<qp::graph::ViewScene::Point>& line : scene_.polylines) {
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            if (!camera_.visible(line[i]) || !camera_.visible(line[i + 1])) continue;
            const Placed a = place(line[i]);
            const Placed b = place(line[i + 1]);
            segments.push_back(Segment{QPointF{a.x, a.y}, QPointF{b.x, b.y}, 0.5 * (a.depth + b.depth)});
        }
    }
    std::stable_sort(segments.begin(), segments.end(),
                     [](const Segment& a, const Segment& b) { return a.depth > b.depth; });
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(120, 170, 230), 1.6));
    for (const Segment& segment : segments) {
        painter.drawLine(segment.from, segment.to);
        ++drawn_segments_;
    }

    // The body at the origin, as a disc shaded by a radial gradient so it reads as a sphere rather than a coin. A
    // mesh would be more honest geometry and more code; the item's `body_radius` is a sphere in the scene's units,
    // and at this scale the difference is invisible -- which is exactly the argument a mesh would have to beat.
    if (scene_.body_radius > 0.0) {
        const double radius = scene_.body_radius * pixels_per_unit;
        const QPointF centre{centre_x, centre_y};
        QRadialGradient shade{centre - QPointF{radius * 0.35, radius * 0.35}, radius * 1.8};
        shade.setColorAt(0.0, QColor(120, 170, 230));
        shade.setColorAt(1.0, QColor(30, 70, 130));
        painter.setBrush(shade);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(centre, radius, radius);
    }

    // The particles, sorted far to near and sized by distance: the two cues that make a ring read as a ring.
    std::vector<Placed> points;
    points.reserve(scene_.points.size());
    for (const qp::graph::ViewScene::Point& point : scene_.points) {
        if (!camera_.visible(point)) {
            ++hidden_points_;
            continue;
        }
        points.push_back(place(point));
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const Placed& a, const Placed& b) { return a.depth > b.depth; });
    painter.setPen(QPen(QColor(120, 90, 20), 1.0));
    for (const Placed& point : points) {
        const double scale = camera_.distance() / std::max(point.depth, 1.0e-9);
        const double radius = std::clamp(kPointRadius * scale, 1.0, 14.0);
        painter.setBrush(QColor(240, 200, 90));
        painter.drawEllipse(QPointF{point.x, point.y}, radius, radius);
        ++drawn_points_;
    }

    painter.setPen(QPen(kLabel, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft,
                     QStringLiteral("%1 R_E   az %2  el %3")
                         .arg(half, 0, 'g', 3)
                         .arg(camera_.view().azimuth_deg, 0, 'f', 0)
                         .arg(camera_.view().elevation_deg, 0, 'f', 0));
}

void SceneView3D::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        last_mouse_ = event->pos();
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void SceneView3D::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        QOpenGLWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint delta = event->pos() - last_mouse_;
    last_mouse_ = event->pos();
    // Half a degree a pixel: a drag across a 400-pixel panel is a half turn, which is the rate a user expects from
    // an orbit control. Dragging **down** raises the camera, because that is the direction the content appears to
    // move.
    camera_.orbit(-0.5 * delta.x(), 0.5 * delta.y());
    update();
    event->accept();
}

void SceneView3D::wheelEvent(QWheelEvent* event) {
    const int notches = event->angleDelta().y();
    if (notches != 0) {
        camera_.zoom(notches > 0 ? 1.0 / qp::views::model::kZoomStep : qp::views::model::kZoomStep);
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::wheelEvent(event);
}

}  // namespace qp::views
