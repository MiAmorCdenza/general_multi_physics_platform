/**
 * @file scene_view.cpp
 * @brief The mapping from scene units to pixels, and nothing else.
 *
 * The fit is deliberately **uniform**: one scale for both axes, chosen as the smaller of the two the bounds would
 * allow, so a ring of particles stays a ring. A widget that stretched each axis to fill its rectangle would draw a
 * circle as an ellipse and a physical picture would be a lie about the shape of an orbit -- the one thing a student
 * is meant to read off it.
 *
 * The origin is placed at the centre of the widget and the y axis is **flipped**, because the scene's `y` is the
 * graph's `y` (north up in the meridional plane) while a widget's `y` grows downward.
 */
#include "scene_view.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace qp::views {
namespace {

/// @brief The point size of one particle, in pixels.
constexpr double kPointRadius = 3.0;
/// @brief The margin kept between the fitted content and the widget's edge, in pixels.
constexpr double kMarginPixels = 8.0;
/// @brief The width of a traced curve, in pixels.
constexpr double kCurveWidth = 1.2;

}  // namespace

SceneView::SceneView(QString empty_text, QWidget* parent)
    : QWidget(parent), empty_text_(std::move(empty_text)) {
    setMinimumSize(220, 220);
    setAutoFillBackground(true);
}

void SceneView::set_scene(qp::graph::ViewScene scene) {
    scene_ = std::move(scene);
    update();
}

void SceneView::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    if ((scene_.points.empty() && scene_.polylines.empty()) || !scene_.has_bounds) {
        painter.drawText(rect(), Qt::AlignCenter, empty_text_);
        return;
    }

    // The uniform fit: the smaller of the two scales, so equal distances stay equal on screen.
    const double width = static_cast<double>(std::max(1, rect().width())) - 2.0 * kMarginPixels;
    const double height = static_cast<double>(std::max(1, rect().height())) - 2.0 * kMarginPixels;
    const double span_x = scene_.x_max - scene_.x_min;
    const double span_y = scene_.y_max - scene_.y_min;
    if (!(span_x > 0.0) || !(span_y > 0.0)) {
        painter.drawText(rect(), Qt::AlignCenter, empty_text_);
        return;
    }
    const double scale = std::min(width / span_x, height / span_y);
    const double centre_x = rect().center().x();
    const double centre_y = rect().center().y();
    const double scene_centre_x = 0.5 * (scene_.x_min + scene_.x_max);
    const double scene_centre_y = 0.5 * (scene_.y_min + scene_.y_max);

    // Scene units to widget pixels, once. The y flip is here rather than inverted at every call site, because
    // every consumer of a point -- curves, the body, the points -- needs the same mapping and a second copy of it
    // is how one of them ends up mirrored.
    const auto to_pixels = [&](const qp::graph::ViewScene::Point& point) {
        return QPointF{centre_x + (point.x - scene_centre_x) * scale,
                       centre_y - (point.y - scene_centre_y) * scale};
    };

    // The curves first, so the body and the particles sit on top of them.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(120, 170, 230), kCurveWidth));
    for (const std::vector<qp::graph::ViewScene::Point>& line : scene_.polylines) {
        if (line.size() < 2) continue;
        QPolygonF path;
        path.reserve(static_cast<int>(line.size()));
        for (const qp::graph::ViewScene::Point& point : line) path.push_back(to_pixels(point));
        painter.drawPolyline(path);
    }

    // The body at the origin, if the scene says there is one. Without it a ring of dots is a ring of dots; with it
    // the picture is a magnetosphere. Its radius is the scene's own number in the scene's own units, so it scales
    // with everything else rather than staying a fixed number of pixels.
    if (scene_.body_radius > 0.0) {
        const QPointF origin{centre_x + (0.0 - scene_centre_x) * scale,
                             centre_y - (0.0 - scene_centre_y) * scale};
        painter.setBrush(QColor(60, 110, 180));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(origin, scene_.body_radius * scale, scene_.body_radius * scale);
    }

    painter.setBrush(QColor(240, 200, 90));
    painter.setPen(QPen(QColor(120, 90, 20), 1.0));
    for (const qp::graph::ViewScene::Point& point : scene_.points) {
        painter.drawEllipse(to_pixels(point), kPointRadius, kPointRadius);
    }
    painter.setPen(QPen(QColor(90, 90, 90), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft,
                     QStringLiteral("%1 R_E").arg(scene_.x_max, 0, 'g', 3));
}

}  // namespace qp::views
