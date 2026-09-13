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

#include <qp/views/model/scene_projection.hpp>

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
    // **140 rather than the 220 this widget was born with**, and a measurement is why: the right-hand column
    // stacks five docks in a window that is 720 logical pixels tall, and a 220-pixel floor per scene panel was
    // enough to push the column past the window -- at which point Qt does not scroll, it gives the later docks
    // zero height and they vanish while every check still passes. A 140-pixel panel is a small but honest picture;
    // a 220-pixel minimum in a column that cannot hold it is no picture at all.
    setMinimumSize(140, 140);
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

    // **The plane this host draws is the scene's view, not the first two coordinates.** Until the scene grew a
    // third coordinate, "the plane" was `x` and `y` and each item chose it by throwing an axis away: the field-line
    // item wrote `{x, z}` and this widget drew it as if it were `{x, y}`. Now the item states the direction it wants
    // to be seen from, the basis comes from the one definition both hosts share, and the plane is a consequence --
    // which is what keeps the picture identical after the item stopped discarding `y`.
    const qp::views::model::ScreenBasis basis = qp::views::model::screen_basis(scene_.view);

    // The uniform fit: one scale for both screen axes, so equal distances stay equal. The half-width comes from the
    // scene's box **projected through the basis**, so a rotated view is fitted as tightly as an axis-aligned one.
    const double width = static_cast<double>(std::max(1, rect().width())) - 2.0 * kMarginPixels;
    const double height = static_cast<double>(std::max(1, rect().height())) - 2.0 * kMarginPixels;
    const double half = qp::views::model::fitted_half_width(scene_, basis, 1.0e-3);
    if (!(half > 0.0) || !std::isfinite(half)) {
        painter.drawText(rect(), Qt::AlignCenter, empty_text_);
        return;
    }
    const double scale = std::min(width, height) / (2.0 * half);
    const double centre_x = rect().center().x();
    const double centre_y = rect().center().y();

    // Scene units to widget pixels, once. The y flip is here rather than inverted at every call site, because
    // every consumer of a point -- curves, the body, the points -- needs the same mapping and a second copy of it
    // is how one of them ends up mirrored.
    const auto to_pixels = [&](const qp::graph::ViewScene::Point& point) {
        const std::pair<double, double> screen = qp::views::model::project_orthographic(basis, point);
        return QPointF{centre_x + screen.first * scale, centre_y - screen.second * scale};
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
        // The projected origin, which is the widget's centre: a view looks at the origin by definition, and the
        // body is at the origin by the scene's own contract.
        const QPointF origin{centre_x, centre_y};
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
                     QStringLiteral("%1 R_E").arg(half, 0, 'g', 3));
}

}  // namespace qp::views
