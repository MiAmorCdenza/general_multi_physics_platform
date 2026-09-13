/**
 * @file view_items.hpp
 * @brief What draws a render declaration: the third list, and the value a drawer answers with.
 *
 * ## The last gap in the render story
 *
 * `graph/domain` says a render node is a **declaration**, not a computation, and `build_plan` puts one into
 * `plan.render.declared` instead of into an evaluation plan. The magnetosphere kit now has such a node
 * (`render.particles`). What was missing is the other end: something that reads a declaration and produces
 * something drawable. A search of `core/`, `views/` and `plugins/` finds "view slot" in exactly one place -- the
 * capability bit's own documentation -- with no registry, no interface and no host. So this file is the
 * interface, and `views/qt` gains the host.
 *
 * ## Why the drawing is a value and not a widget
 *
 * `views/model` is Qt-free **on purpose**: it is the half of the view layer both compilers build and the test
 * suite covers. A view item that painted would drag Qt into it, and -- worse -- would put the drawing decision
 * where no test can reach it. So an item answers with a `ViewScene`: a handful of 2-D points in its own units
 * and the bounds it wants fitted. **What to draw is content; how to draw it is the toolkit's.** That split is
 * the same one `IExporter` and `ports::Value` take, and for the same reason.
 *
 * ## Why there is no slot registry yet
 *
 * The obvious next abstraction is a registry of named slots with items claiming them. It is not here because
 * there is one item: a registry with a single client is a declaration nothing else consumes, which is the shape
 * this repository keeps finding and refusing. The reopening condition is concrete -- **when a second view item
 * exists** (field lines, a trail-only view), extract the registry and let items claim slots, because then the
 * "which slot does this draw into" question has more than one possible answer.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main (mounting) and the drawing thread (reading)
 * @pre         none
 * @post        none
 * @invariant   A mounted item outlives the list
 * @errors      See each declaration
 * @frozen      no
 * @tests       views.items.a_scene_is_a_value_not_a_widget
 */
#pragma once

#include <qp/graph/domain/declaration.hpp>
#include <qp/graph/ir.hpp>
#include <qp/views/model/run_controller.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace qp::views::model {

/**
 * @brief What a view item is asked to draw with.
 *
 * @ownership   borrows every pointer
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `declared` is the render plan's own list, never a copy
 * @errors      See each declaration
 * @frozen      no
 * @tests       views.items.a_scene_is_a_value_not_a_widget
 */
struct ViewRequest final {
    /// The graph the declarations came from. Borrowed.
    const qp::graph::Graph* graph = nullptr;
    /// The render-domain declarations, as `plan.render.declared()` produced them.
    const std::vector<qp::graph::DeclaredOutput>* declared = nullptr;
    /// The last run's result. Borrowed; its `particle_positions` may be empty, which means "nothing to draw yet"
    /// rather than an error -- a graph that has not been run is the ordinary state of a window.
    const RunResult* run = nullptr;

    /// @brief Whether there is anything to draw from.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        True exactly when a graph and a run are present
    /// @invariant   Does not inspect the run's contents
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       views.items.a_scene_is_a_value_not_a_widget
    [[nodiscard]] bool valid() const noexcept { return graph != nullptr && run != nullptr; }
};

/**
 * @brief What to draw, in the item's own units: points, an optional trail, and the bounds to fit.
 *
 * A value, copyable and Qt-free, so a test can assert what an item decided without a toolkit and without
 * pixels. The units are the item's; the host converts them to screen coordinates using `x_min`..`y_max`, which is
 * why the bounds are part of the scene rather than something the host infers -- an inferred fit jumps when a
 * particle leaves the box, and the item is the only thing that knows what "the interesting region" is.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `has_bounds` is false exactly when the scene has nothing to fit
 * @errors      See each declaration
 * @frozen      no
 * @tests       views.items.a_scene_is_a_value_not_a_widget
 */
struct ViewScene final {
    /// One 2-D point in the item's own units.
    struct Point final {
        double x = 0.0;
        double y = 0.0;
    };

    /// One point per particle, in the order the run holds them.
    std::vector<Point> points{};
    /// An optional polyline drawn behind the points, in order. Empty when the item has no trail.
    std::vector<Point> trail{};
    /// The region to fit: the item's own idea of what matters, not the extent of `points`.
    double x_min = 0.0;
    double x_max = 0.0;
    double y_min = 0.0;
    double y_max = 0.0;
    /// Whether the bounds mean anything. A scene with no bounds is drawn in whatever the host had.
    bool has_bounds = false;

    /// @brief Whether there is anything to draw.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        True exactly when both lists are empty
    /// @invariant   A scene with a trail and no points is not empty
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       views.items.a_scene_is_a_value_not_a_widget
    [[nodiscard]] bool empty() const noexcept { return points.empty() && trail.empty(); }
};

/**
 * @brief A thing that turns a render declaration into a scene.
 *
 * Implementations live in content (`plugins/views_items/`), next to the drawing they describe, and the
 * application mounts them -- the same inversion `IOperatorBinder` and `IGraphRunProvider` use on the other two
 * sides of a run. Nothing in `views/` names a kit or a content directory.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `name` and `draws` are constant for the object's lifetime
 * @errors      See each declaration
 * @frozen      no
 * @tests       views.items.a_scene_is_a_value_not_a_widget
 */
class IViewItem {
public:
    IViewItem() = default;
    virtual ~IViewItem() = default;
    IViewItem(const IViewItem&) = delete;
    IViewItem& operator=(const IViewItem&) = delete;

    /// @brief Stable short name, for a status line and for a panel's title.
    ///
    /// @ownership   observes
    /// @thread      any
    /// @pre         none
    /// @post        Non-empty
    /// @invariant   Constant for the object's lifetime
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       views.items.a_scene_is_a_value_not_a_widget
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// @brief Whether this item draws a render node of `type_name`.
    ///
    /// Asked per declaration rather than once per graph, because a graph may declare several items and one item
    /// may draw only some of them. The answer depends on the **type name**, never on an instance's parameters:
    /// an item that declined for a particular node would report "nothing to draw" about a graph that draws.
    ///
    /// @param type_name The render node's type.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        The answer depends only on the type name
    /// @invariant   A name this item answers true for is one `scene` can be asked about
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       views.items.a_scene_is_a_value_not_a_widget
    [[nodiscard]] virtual bool draws(std::string_view type_name) const noexcept = 0;

    /// @brief Produces the scene for one declaration.
    ///
    /// May allocate and may be slow: this runs when a run finishes, not per frame. That is why it is one call
    /// per draw rather than a per-particle callback, and why it is free to build a trail.
    ///
    /// @param request The graph, the declarations and the last run.
    ///
    /// @ownership   owns the returned scene
    /// @thread      main
    /// @pre         `request.valid()`
    /// @post        A scene in the item's own units, possibly empty
    /// @invariant   Does not modify the graph, the declarations or the run
    /// @errors      Cannot fail: an item that cannot draw answers with an empty scene
    /// @complexity  O(particles)
    /// @nondet      none
    /// @frozen      no
    /// @tests       views.items.a_scene_is_a_value_not_a_widget
    [[nodiscard]] virtual ViewScene scene(const ViewRequest& request) = 0;
};

/// @brief The view items the application mounted, in the order a declaration should be offered to them.
///
/// @ownership   borrows (the list holds pointers; whoever registered an item owns it)
/// @thread      main
/// @pre         none
/// @post        Empty when nothing has been mounted
/// @invariant   The same object every call
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       views.items.a_scene_is_a_value_not_a_widget
[[nodiscard]] const std::vector<IViewItem*>& view_items() noexcept;

/// @brief Adds a view item to the list the window offers declarations to.
///
/// @param item The item. Borrowed; it must outlive the window.
///
/// @ownership   observes `item`
/// @thread      main
/// @pre         `item` outlives the window
/// @post        `item` is the last entry, unless it was already there
/// @invariant   A null item is not added, and an item already mounted is not added twice
/// @errors      noexcept
/// @complexity  O(items)
/// @nondet      none
/// @frozen      no
/// @tests       views.items.a_scene_is_a_value_not_a_widget
void mount_view_item(IViewItem* item) noexcept;

/// @brief Forgets every view item, for a test that wants a clean list.
///
/// @ownership   owns
/// @thread      main
/// @pre         none
/// @post        `view_items()` is empty
/// @invariant   No item is destroyed: the list only borrows
/// @errors      noexcept
/// @complexity  O(1)
/// @nondet      none
/// @frozen      no
/// @tests       views.items.a_scene_is_a_value_not_a_widget
void clear_view_items() noexcept;

}  // namespace qp::views::model
