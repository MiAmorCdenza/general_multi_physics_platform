/**
 * @file particle_view.hpp
 * @brief The host: a `ViewScene` becomes pixels.
 *
 * The last link of the render chain. Everything before it is a value: the graph declares what it wants drawn, the
 * run controller produces the declarations and the particle snapshot, and a `IViewItem` turns them into a
 * `ViewScene` -- points in its own units plus the bounds it wants fitted. This widget does the one thing that
 * needs a toolkit: it maps those units onto its own rectangle and draws.
 *
 * ## Why it holds a scene and not a run
 *
 * A widget that kept a pointer to a run would be a widget whose picture depends on data somebody else may
 * replace; holding a **copy** of a scene means the painted frame and the reported numbers can never be from
 * different runs, which is the kind of mismatch nobody notices until a screenshot is used as evidence. A scene
 * is a few hundred doubles, so the copy costs nothing worth naming.
 *
 * ## Why the axes are labelled rather than drawn from the data
 *
 * The scene's bounds are the item's decision -- see `IViewItem` -- and this widget only fits them. That is the
 * split the whole design rests on: what is interesting is content, how big the box is on screen is the
 * toolkit's. The scale text is in earth radii because the kit reports positions there, and the widget says so
 * rather than pretending to know the unit.
 *
 * @ownership   owns the scene copy
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The painted scene is the last one `set_scene` was given
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.particle.draws_a_scene_and_says_when_there_is_none
 */
#pragma once

#include <qp/graph/domain/view_items.hpp>

#include <QString>
#include <QWidget>

namespace qp::views {

/**
 * @brief Paints one view scene: a point per particle, fitted to the scene's own bounds.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   An empty scene paints the message from `empty_text` and no points
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.particle.draws_a_scene_and_says_when_there_is_none
 */
class ParticleView final : public QWidget {
    Q_OBJECT

public:
    explicit ParticleView(QWidget* parent = nullptr);

    /// @brief Takes a scene to paint, replacing whatever was there.
    ///
    /// @param scene The scene. Copied: see the file comment for why this is not a pointer.
    ///
    /// @ownership   owns a copy
    /// @thread      ui
    /// @pre         none
    /// @post        `scene()` equals `scene` and the widget repaints
    /// @invariant   No pointer into the caller's data is kept
    /// @errors      noexcept
    /// @complexity  O(points)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.particle.draws_a_scene_and_says_when_there_is_none
    void set_scene(qp::graph::ViewScene scene);

    /// @brief The scene being painted.
    ///
    /// @ownership   borrows from this object
    /// @thread      ui
    /// @pre         none
    /// @post        The last scene handed to `set_scene`
    /// @invariant   Never null; an empty one before the first call
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.particle.draws_a_scene_and_says_when_there_is_none
    [[nodiscard]] const qp::graph::ViewScene& scene() const noexcept { return scene_; }

    /// @brief The message painted when there is nothing to draw.
    ///
    /// A sentence rather than a blank widget, and exposed so a test can assert it: a window that has not been
    /// run and a graph that draws nothing look identical on screen, and only one of them is worth a second
    /// look from the user.
    ///
    /// @ownership   pure
    /// @thread      ui
    /// @pre         none
    /// @post        A non-empty message
    /// @invariant   Constant
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.particle.draws_a_scene_and_says_when_there_is_none
    [[nodiscard]] static QString empty_text();

protected:
    /// @brief Paints the scene, or the message when there is none.
    ///
    /// @param event The paint event. Unused beyond Qt's own bookkeeping.
    ///
    /// @ownership   observes
    /// @thread      ui
    /// @pre         none
    /// @post        The widget shows the current scene
    /// @invariant   Reads only `scene_`
    /// @errors      Cannot fail
    /// @complexity  O(points)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.particle.draws_a_scene_and_says_when_there_is_none
    void paintEvent(QPaintEvent* event) override;

private:
    qp::graph::ViewScene scene_{};
};

}  // namespace qp::views
