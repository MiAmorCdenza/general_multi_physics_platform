/**
 * @file view_items.hpp
 * @brief What draws a render declaration: the render domain's other end, and the value a drawer answers with.
 *
 * ## The last gap in the render story, and why it lives here
 *
 * This module says a render node is a **declaration**, not a computation, and `build_plan` puts one into
 * `plan.render.declared` instead of into an evaluation plan. The magnetosphere kit now has such a node
 * (`render.particles`). What was missing is the **other end**: something that reads a declaration and produces
 * something drawable. A search of `core/`, `views/` and `plugins/` finds "view slot" in exactly one place -- the
 * capability bit's own documentation -- with no registry, no interface and no host.
 *
 * **It is here rather than in `views/model`, and that is a correction.** The first version lived in the view
 * layer carrying a `const RunResult*`; a plugin implementing it would then have depended on `views/`, and
 * `execution_binders.hpp` already records what that costs -- `views -> plugins -> views` was a cycle, and the
 * lesson written there is that **the interface a plugin implements belongs in `core/`**. The declarations
 * themselves are this module's own vocabulary, so this is the module whose subject it is.
 *
 * ## Why the drawing is a value and not a widget
 *
 * `views/model` is Qt-free **on purpose**: it is the half of the view layer both compilers build and the test
 * suite covers. A view item that painted would drag Qt into it, and -- worse -- would put the drawing decision
 * where no test can reach it. So an item answers with a `ViewScene`: a handful of 2-D points in its own units
 * and the bounds it wants fitted. **What to draw is content; how to draw it is the toolkit's.** That split is
 * the same one `IExporter` and `ports::Value` take, and for the same reason.
 *
 * ## The slot registry, and the condition that fired without it
 *
 * The obvious next abstraction is a registry of named slots with items claiming them, and this file said it would
 * arrive "when a second view item exists (field lines, a trail-only view)". **That condition has now been met** --
 * the magnetosphere kit ships a field-line item beside the particle one -- and the registry is still not here, so
 * the condition is recorded as fired rather than quietly rewritten.
 *
 * The reason it did not need to arrive is that the question it answers never acquired a second answer: the host
 * gives **every mounted item its own panel**, so "which slot does this draw into" is one panel per item, decided by
 * the item's identity rather than by a claim. What the second item did change is the host itself, which no longer
 * looks a panel up by widget type: it walks `view_items()`, asks each item about each declaration, and keeps one
 * panel per item -- the smallest thing that stops two items from drawing over each other.
 *
 * The registry reopens when an item has a **choice**: two items claiming one panel, one item drawing into two, or
 * a graph declaring two items of one type. None of those exists today, and a registry built for a question with
 * one answer is the shape this repository keeps finding and refusing.
 *
 * @ownership   mixed -- see each declaration
 * @thread      main (mounting) and the drawing thread (reading)
 * @pre         none
 * @post        none
 * @invariant   A mounted item outlives the list
 * @errors      See each declaration
 * @frozen      no
 * @tests       graph.domain.a_scene_is_a_value_not_a_widget
 */
#pragma once

#include <qp/graph/domain/declaration.hpp>
#include <qp/graph/field/field_set.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/structure.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace qp::graph {

/**
 * @brief What a view item is asked to draw with.
 *
 * **It names no view-layer type**, and that is a correction rather than a preference. The first version carried
 * a `const RunResult*`, which tied this contract to `views/model`; a plugin implementing `IViewItem` would then
 * depend on the view layer, and this repository has already written down why that is wrong -- the comment on
 * `IOperatorBinder` in `execution_binders.hpp` records that `views -> plugins -> views` was a cycle, and that
 * the interface a plugin implements therefore belongs in `core/`. A request that names only a graph, a list of
 * declarations and a span of positions can move to `core/` unchanged, which is where it is going.
 *
 * @ownership   borrows every pointer
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `declared` is the render plan's own list, never a copy
 * @errors      See each declaration
 * @frozen      no
 * @tests       graph.domain.a_scene_is_a_value_not_a_widget
 */
struct ViewRequest final {
    /// The graph the declarations came from. Borrowed.
    const qp::graph::Graph* graph = nullptr;
    /// The render-domain declarations, as `plan.render.declared()` produced them.
    const std::vector<qp::graph::DeclaredOutput>* declared = nullptr;
    /// The position snapshot the last run produced: **three doubles per particle**, in the run's own units.
    ///
    /// A plain span rather than the controller's `RunResult`, for the reason in the file comment. Null means "no
    /// run yet", which is the ordinary state of a window rather than an error; an **empty** vector means a run
    /// that produced no particles, and an item draws nothing for it without treating it as a failure.
    const std::vector<double>* positions = nullptr;
    /// Host steps the run took, for whatever label an item or a host wants to show.
    std::size_t steps = 0;
    /// The **field tables the run baked**, for an item that draws a field rather than a population.
    ///
    /// Absent (null) for every run that baked nothing, which is the operator loop and therefore most runs. An
    /// item that needs a field treats null as "nothing to draw" -- the same answer as a store that does not
    /// contain the key it was told about, because in both cases the honest report is that the field is not
    /// there rather than that the picture is empty.
    ///
    /// **Why the request carries a store and not a `FieldValue`.** A declaration names a *node and a port*, and
    /// turning that into samples is one lookup the item performs after following the wire; a request that
    /// pre-resolved the value would have to decide which field is interesting, which is the item's job and
    /// depends on the item. It is also the honest ownership answer: the store belongs to the run and outlives
    /// every declaration read from it.
    ///
    /// **Why it is the last member.** `ViewRequest` is built by aggregate initialization at four call sites, and
    /// a member inserted in the middle silently rebinds the fourth initializer -- `ViewRequest{g, d, p, 0}` would
    /// set `fields` to null and leave `steps` zero, which is the *intended* meaning at one of them and an
    /// accident at the others. Appending keeps every existing four-element form exactly as it was, so a reader
    /// of this file never has to check the call sites to know what a `0` in that position means.
    const qp::graph::field::FieldSet* fields = nullptr;

    /**
     * @brief Whether there is anything to draw from.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        True exactly when a graph and a position snapshot are present
     * @invariant   Does not inspect the positions
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.domain.a_scene_is_a_value_not_a_widget
     */
    [[nodiscard]] bool valid() const noexcept { return graph != nullptr && positions != nullptr; }
};

/**
 * @brief What to draw, in the item's own units: points in space, curves, the bounds to fit, and the way to look at
 * it.
 *
 * A value, copyable and Qt-free, so a test can assert what an item decided without a toolkit and without
 * pixels. The units are the item's; the host converts them to screen coordinates using the bounds, which is
 * why the bounds are part of the scene rather than something the host infers -- an inferred fit jumps when a
 * particle leaves the box, and the item is the only thing that knows what "the interesting region" is.
 *
 * **The coordinates are three, and the view is the item's choice.** Until this header grew a `z`, a scene was
 * "points in a plane" and each item picked its own projection by throwing an axis away: the particle item drew
 * the equatorial plane and the field-line item the meridional one. That worked while the host could only draw a
 * plane, and it stopped working the moment a host grew a camera -- because a camera pointed at a family of
 * meridional curves from the equatorial direction sees a line, and only the item knows that. So the scene carries
 * a **default view**, an orthographic host projects with it, and a host with a camera starts there.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `has_bounds` is false exactly when the scene has nothing to fit
 * @errors      See each declaration
 * @frozen      no
 * @tests       graph.domain.a_scene_is_a_value_not_a_widget
 */
struct ViewScene final {
    /// One point in space, in the item's own units.
    ///
    /// `z` is **appended** rather than inserted, so every `Point{x, y}` a producer already wrote still means what
    /// it meant: a point in the equatorial plane. Three times this session a member inserted in the middle of a
    /// positional aggregate has silently re-labelled every construction of it, and the rule that came out of those
    /// is that a new coordinate goes at the end.
    struct Point final {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    /// One point per particle, in the order the run holds them.
    std::vector<Point> points{};
    /// Zero or more polylines drawn behind the points, each in order.
    ///
    /// **Plural, and that is the second view item's doing.** The field was a single `trail` while the only item
    /// was the particle view, where one trail is the whole story. A field-line picture is the opposite shape:
    /// a dipole's cross-section is a *family* of closed curves, and an item that could draw one curve could not
    /// draw the thing the item exists for. So the field is a list, the particle item puts its one history in a
    /// list of one, and the host draws them all the same way -- which is what makes "how many curves" a content
    /// decision instead of a change to this header.
    std::vector<std::vector<Point>> polylines{};
    /// The direction an item wants its picture seen from. See the type's comment.
    struct View final {
        /// Degrees around the polar axis, measured from `+x` toward `+y`.
        double azimuth_deg = 0.0;
        /// Degrees above the equatorial plane, from `0` (in the plane) to `90` (on the `+z` axis).
        double elevation_deg = 90.0;
        /// How far the camera sits from the origin, in the scene's own units. Zero means "the host fits the
        /// bounds", which is what an orthographic host does and what a camera host uses as a starting distance
        /// before its own zoom takes over.
        double distance = 0.0;
    };

    /// The default view. `azimuth 0, elevation 90` looks down the `+z` axis at the equatorial plane with `+x`
    /// to the right and `+y` up -- the orientation the particle item has always drawn in, and therefore the
    /// default a scene that says nothing gets.
    View view{};

    /// The region to fit: the item's own idea of what matters, not the extent of `points`.
    ///
    /// A box rather than a rectangle, with the same three-of-everything rule as `Point`: `z_min`/`z_max` are
    /// appended, an orthographic host ignores them, and a camera host uses the whole box to place its near and
    /// far planes and to bound how far the user can pull back.
    double x_min = 0.0;
    double x_max = 0.0;
    double y_min = 0.0;
    double y_max = 0.0;
    double z_min = 0.0;
    double z_max = 0.0;
    /// Whether the bounds mean anything. A scene with no bounds is drawn in whatever the host had.
    bool has_bounds = false;
    /// The radius of a **body at the origin** to draw under everything else, in the item's own units.
    ///
    /// Zero means none, and zero is the default, which is the right default for a general scene: an item drawing a
    /// laboratory oscillator has an origin but nothing at it. It is in the scene rather than in the widget because
    /// the widget is the toolkit and the body is content -- the same split that already puts the *bounds* here
    /// instead of letting the host infer them. The first two items this platform ships both draw a magnetosphere
    /// and both set one earth radius, and a widget that knew that would be a widget that cannot draw anything else.
    ///
    /// It is a radius and not a flag because a scene drawn in earth radii wants a planet one earth radius across:
    /// a flag would leave the host choosing a size, and the host does not know the units.
    double body_radius = 0.0;

    /**
     * @brief Whether there is anything to draw.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        True exactly when both lists are empty
     * @invariant   A scene with a polyline and no points is not empty
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.domain.a_scene_is_a_value_not_a_widget
     */
    [[nodiscard]] bool empty() const noexcept { return points.empty() && polylines.empty(); }
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
 * @tests       graph.domain.a_scene_is_a_value_not_a_widget
 */
class IViewItem {
public:
    IViewItem() = default;
    virtual ~IViewItem() = default;
    IViewItem(const IViewItem&) = delete;
    IViewItem& operator=(const IViewItem&) = delete;

    /**
     * @brief Stable short name, for a status line and for a panel's title.
     *
     * @ownership   observes
     * @thread      any
     * @pre         none
     * @post        Non-empty
     * @invariant   Constant for the object's lifetime
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.domain.a_scene_is_a_value_not_a_widget
     */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * @brief Whether this item draws a render node of `type_name`.
     *
     * Asked per declaration rather than once per graph, because a graph may declare several items and one item
     * may draw only some of them. The answer depends on the **type name**, never on an instance's parameters:
     * an item that declined for a particular node would report "nothing to draw" about a graph that draws.
     *
     * @param type_name The render node's type.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        The answer depends only on the type name
     * @invariant   A name this item answers true for is one `scene` can be asked about
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       graph.domain.a_scene_is_a_value_not_a_widget
     */
    [[nodiscard]] virtual bool draws(std::string_view type_name) const noexcept = 0;

    /**
     * @brief Produces the scene for one declaration.
     *
     * May allocate and may be slow: this runs when a run finishes, not per frame. That is why it is one call
     * per draw rather than a per-particle callback, and why it is free to build a trail.
     *
     * @param request The graph, the declarations and the last run.
     *
     * @ownership   owns the returned scene
     * @thread      main
     * @pre         `request.valid()`
     * @post        A scene in the item's own units, possibly empty
     * @invariant   Does not modify the graph, the declarations or the run
     * @errors      Cannot fail: an item that cannot draw answers with an empty scene
     * @complexity  O(whatever this item has to read): the particle item is O(particles), a field-line item is
     *              O(lines x steps x interpolation), and the interface makes no claim beyond "not per frame"
     * @nondet      none
     * @frozen      no
     * @tests       graph.domain.a_scene_is_a_value_not_a_widget
     */
    [[nodiscard]] virtual ViewScene scene(const ViewRequest& request) = 0;
};

/**
 * @brief The view items the application mounted, in the order a declaration should be offered to them.
 *
 * @ownership   borrows (the list holds pointers; whoever registered an item owns it)
 * @thread      main
 * @pre         none
 * @post        Empty when nothing has been mounted
 * @invariant   The same object every call
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       graph.domain.a_scene_is_a_value_not_a_widget
 */
[[nodiscard]] const std::vector<IViewItem*>& view_items() noexcept;

/**
 * @brief Adds a view item to the list the window offers declarations to.
 *
 * @param item The item. Borrowed; it must outlive the window.
 *
 * @ownership   observes `item`
 * @thread      main
 * @pre         `item` outlives the window
 * @post        `item` is the last entry, unless it was already there
 * @invariant   A null item is not added, and an item already mounted is not added twice
 * @errors      noexcept
 * @complexity  O(items)
 * @nondet      none
 * @frozen      no
 * @tests       graph.domain.a_scene_is_a_value_not_a_widget
 */
void mount_view_item(IViewItem* item) noexcept;

/**
 * @brief Forgets every view item, for a test that wants a clean list.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        `view_items()` is empty
 * @invariant   No item is destroyed: the list only borrows
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       graph.domain.a_scene_is_a_value_not_a_widget
 */
void clear_view_items() noexcept;

}  // namespace qp::graph
