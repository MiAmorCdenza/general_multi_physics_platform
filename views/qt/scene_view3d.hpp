/**
 * @file scene_view3d.hpp
 * @brief The host with a camera: a `ViewScene` becomes a three-dimensional picture.
 *
 * The second host, beside `SceneView`. It draws the same value -- points, polylines, a body, bounds -- through
 * `OrbitCamera` instead of through a fixed plane, and it is where the third coordinate the scene grew is finally
 * *used*: a ring current is a ring, a field-line family is a set of shells, and the user can turn both.
 *
 * ## Three depth cues, because a wireframe with none is unreadable
 *
 * A perspective divide alone leaves a user unable to tell near from far. Three cues are cheap and honest:
 *
 *   - **size**: a particle's dot shrinks with distance, which is the perspective divide applied to a fixed screen
 *     radius rather than a constant radius;
 *   - **sorting**: points and curve segments are drawn far to near, so a near point covers a far one. Without it the
 *     far side of a ring is painted over the near side, which reads as a bug rather than as depth;
 *   - **a ground grid** in the equatorial plane at the fitted half-width, so the eye has something with known
 *     geometry to judge the depth against. It is drawn first and faintly, and it is the scene's own bounds rather
 *     than a decoration: the grid the user sees is the box the item asked to be fitted.
 *
 * ## What it deliberately does not do
 *
 * No shaders and no instancing: see the file comment. No picking: a click that selects a particle is a feature with a
 * caller's question behind it ("which particle?"), and there is none yet. No axes labels: the scale is already
 * reported, in earth radii, and the grid is at the fitted half-width so the two agree.
 *
 * @ownership   observes
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The camera is always a legal view: see `OrbitCamera`; and every paint starts from a cleared
 *              backdrop, because a `QOpenGLWidget` does not clear itself and an uncleared one accumulates frames
 * @errors      See each declaration
 * @frozen      no
 * @tests       qt.views.scene3d.a_scene_can_be_turned, qt.views.scene3d.the_panel_is_dockable_and_floatable
 */
#pragma once

#include <qp/graph/domain/view_items.hpp>
#include <qp/views/model/orbit_camera.hpp>

#include <QOpenGLWidget>
#include <QPoint>
#include <QString>

namespace qp::views {

class SceneView3D final : public QOpenGLWidget {
    Q_OBJECT

public:
    /// @brief A three-dimensional panel that says `empty_text` when its scene has nothing in it.
    explicit SceneView3D(QString empty_text, QWidget* parent = nullptr);

    /// @brief Takes a scene to draw, replacing whatever was there.
    ///
    /// The camera is **reset to the scene's own view**, because a scene states the direction it wants to be seen
    /// from and a new scene is a new picture. A user who has turned the camera keeps their angle only while the
    /// scene stays the same, which is what "the item chooses the default" means.
    void set_scene(qp::graph::ViewScene scene);

    /// @brief The scene being drawn.
    [[nodiscard]] const qp::graph::ViewScene& scene() const noexcept { return scene_; }
    /// @brief The camera, so a test can orbit it without a mouse.
    [[nodiscard]] qp::views::model::OrbitCamera& camera() noexcept { return camera_; }
    [[nodiscard]] const qp::views::model::OrbitCamera& camera() const noexcept { return camera_; }
    /// @brief The message painted when there is nothing to draw.
    [[nodiscard]] const QString& empty_text() const noexcept { return empty_text_; }
    /// @brief How many points were drawn in the last paint, and how many were behind the camera.
    ///
    /// Exposed for the same reason `FitPanel::coefficient_rows` is: the drawing itself cannot be asserted without
    /// pixels, and "the near half of the ring was drawn" is the part that can.
    [[nodiscard]] int drawn_points() const noexcept { return drawn_points_; }
    [[nodiscard]] int hidden_points() const noexcept { return hidden_points_; }
    /// @brief How many curve segments were drawn in the last paint.
    [[nodiscard]] int drawn_segments() const noexcept { return drawn_segments_; }

protected:
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    /// @brief Refits the camera to the scene's bounds, keeping the direction the scene asked for.
    void refit_camera();

    qp::graph::ViewScene scene_{};
    qp::views::model::OrbitCamera camera_{};
    QString empty_text_{};
    QPoint last_mouse_{};
    bool dragging_ = false;
    int drawn_points_ = 0;
    int hidden_points_ = 0;
    int drawn_segments_ = 0;
};

}  // namespace qp::views
