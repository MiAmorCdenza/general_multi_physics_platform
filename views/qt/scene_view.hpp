/**
 * @file scene_view.hpp
 * @brief The host: a `ViewScene` becomes pixels, whatever the scene happens to be.
 *
 * The last link of the render chain. Everything before it is a value: the graph declares what it wants drawn, the
 * run controller produces the declarations, the snapshots and the baked tables, and an `IViewItem` turns them into
 * a `ViewScene`. This widget does the one thing that needs a toolkit: it maps the scene's own units onto its own
 * rectangle and draws.
 *
 * ## Why it was called `ParticleView` and is not any more
 *
 * It drew particle snapshots when it was written, and it drew them well. The second view item is a family of field
 * lines, which is the same job on a different scene -- curves and a body instead of points -- and a widget named
 * after its first caller would have been a widget every later reader had to check before trusting. What is
 * genuinely item-specific, and therefore what arrives through the constructor, is only the **message shown when
 * there is nothing to draw**: an item knows what it would have drawn, and the window can say so in the item's own
 * words because `IViewItem::name` is on the interface.
 *
 * ## Why the body at the origin is a scene field and not a flag here
 *
 * The widget draws a disc at the scene's origin when the scene says there is a body there, and draws no disc when
 * it does not. It does not know that the body is a planet, which is the point: the toolkit maps units to pixels,
 * and what is interesting -- including whether anything sits at the origin -- is content. A widget with a
 * "magnetosphere" flag would be a widget that cannot draw a laboratory experiment.
 *
 * ## Why it holds a scene and not a run
 *
 * A widget that kept a pointer to a run would be a widget whose picture depends on data somebody else may replace;
 * holding a **copy** of a scene means the painted frame and the reported numbers can never be from different runs,
 * which is the kind of mismatch nobody notices until a screenshot is used as evidence. A scene is a few hundred
 * doubles, so the copy costs nothing worth naming.
 *
 * @ownership   owns the scene copy
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   The painted scene is the last one `set_scene` was given
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.scene.draws_a_scene_and_says_when_there_is_none
 */
#pragma once

#include <qp/graph/domain/view_items.hpp>

#include <QString>
#include <QWidget>

namespace qp::views {

/**
 * @brief Paints one view scene: curves, a body and points, fitted to the scene's own bounds.
 *
 * @ownership   owns
 * @thread      ui
 * @pre         none
 * @post        none
 * @invariant   An empty scene paints `empty_text` and nothing else
 * @errors      noexcept
 * @frozen      no
 * @tests       qt.views.scene.draws_a_scene_and_says_when_there_is_none
 */
class SceneView final : public QWidget {
    Q_OBJECT

public:
    /// @brief A panel that says `empty_text` when its scene has nothing in it.
    ///
    /// @param empty_text What to show before there is anything to draw. Passed in because the item knows what it
    ///                   would have drawn -- see the file comment -- and it is a parameter rather than a setter
    ///                   because a panel that changed its message later would be a panel whose blank state means
    ///                   two different things at two moments.
    /// @param parent     The Qt parent, which owns this widget.
    ///
    /// @ownership   owns
    /// @thread      ui
    /// @pre         none
    /// @post        The widget shows `empty_text` until `set_scene` is called with something drawable
    /// @invariant   `empty_text()` is the string passed here
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.scene.draws_a_scene_and_says_when_there_is_none
    explicit SceneView(QString empty_text, QWidget* parent = nullptr);

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
    /// @complexity  O(points + curves)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.scene.draws_a_scene_and_says_when_there_is_none
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
    /// @tests       qt.views.scene.draws_a_scene_and_says_when_there_is_none
    [[nodiscard]] const qp::graph::ViewScene& scene() const noexcept { return scene_; }

    /// @brief The message painted when there is nothing to draw.
    ///
    /// A sentence rather than a blank widget, and exposed so a test can assert it: a window that has not been run
    /// and a graph that draws nothing look identical on screen, and only one of them is worth a second look from
    /// the user.
    ///
    /// @ownership   borrows from this object
    /// @thread      ui
    /// @pre         none
    /// @post        The string the constructor was given
    /// @invariant   Constant for the widget's lifetime
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.scene.draws_a_scene_and_says_when_there_is_none
    [[nodiscard]] const QString& empty_text() const noexcept { return empty_text_; }

protected:
    /// @brief Paints the scene, or the message when there is none.
    ///
    /// @param event The paint event. Unused beyond Qt's own bookkeeping.
    ///
    /// @ownership   observes
    /// @thread      ui
    /// @pre         none
    /// @post        The widget shows the current scene
    /// @invariant   Reads only `scene_` and `empty_text_`
    /// @errors      Cannot fail
    /// @complexity  O(points + curves)
    /// @nondet      none
    /// @frozen      no
    /// @tests       qt.views.scene.draws_a_scene_and_says_when_there_is_none
    void paintEvent(QPaintEvent* event) override;

private:
    qp::graph::ViewScene scene_{};
    QString empty_text_{};
};

}  // namespace qp::views
