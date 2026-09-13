/**
 * @file field_lines_item.hpp
 * @brief The kit's second drawing side: a baked field becomes a family of curves.
 *
 * `render.field_lines` is what a graph **declares**; this reads the declaration and answers with something
 * drawable. It is the second item, and being the second is what changed the platform around it:
 *
 * ## Three contracts this item is the reason for
 *
 *   - `ViewRequest` carries the **run's field tables**. A render node is a declaration, and this one names a
 *     *baked product* -- "draw the field on this wire" -- so the thing that reads the declaration has to reach
 *     samples. `run_provider.hpp` had written the cost of not having them and the condition that would pay it;
 *     this item is that condition met.
 *   - `ViewScene` holds **polylines** rather than one `trail`. A dipole's cross-section is a family of closed
 *     curves, and an item that could draw one curve could not draw the thing the item exists for.
 *   - the drawing host offers each declaration to **every** mounted item and keeps one panel per item, instead of
 *     looking up the particle panel by type. `view_items.hpp` wrote that condition too -- "when a second view item
 *     exists" -- and it is met here. The registry it predicted (items claiming named slots) is deliberately still
 *     not built: with two items and one panel each, "which panel" has exactly one answer, and a registry that
 *     could not be asked a question with two answers is the shape this repository keeps refusing.
 *
 * ## What it draws, and the projection it states
 *
 * The **meridional plane**: `x` across, `z` up, the field lines of a dipole closing over the poles with the Earth
 * at the origin. That is the picture every textbook draws of a magnetosphere, and it is not the equatorial plane
 * the particle item uses -- worth stating because the two panels sit side by side and a reader will compare them.
 * In the equatorial plane a dipole's field points *out of* the plane, so a field line traced there leaves it
 * immediately; the choice is forced by the physics rather than made for looks. The reopening condition is a
 * third projection (a drift shell seen down the magnetic axis), at which point the projection becomes a parameter
 * of the item and the panel grows a control.
 *
 * ## Why the seeds are on the `+x` axis
 *
 * A family of lines is a one-parameter family for a dipole: each line is identified by the radius at which it
 * crosses the equator. Seeding along `+x` and tracing **both ways** gives each line's two halves -- north to the
 * surface and south to the surface -- so the picture is the symmetric set of nested shells, and a line whose
 * trace stops early is visibly short rather than silently missing.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The same request produces the same scene
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.render.a_field_becomes_a_family_of_curves
 */
#pragma once

#include <qp/graph/domain/view_items.hpp>

#include <string_view>

namespace qp::plugins::magnetosphere {

/**
 * @brief Draws `render.field_lines`: the field's curves, in the meridional plane.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Stateless: two calls with equal requests give equal scenes
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.render.a_field_becomes_a_family_of_curves
 */
class FieldLinesViewItem final : public qp::graph::IViewItem {
public:
    /// @brief The name a panel title and a status line show.
    static constexpr const char* kName = "field lines";

    /// @brief The fraction of the outermost seed's radius the fitted box adds around it.
    ///
    /// The field-line picture's fit is **derived from the seeds and not from the curves**, and that is the
    /// difference between a stable frame and a jumping one: a line traced to twelve earth radii in one graph and
    /// to six in the next would rescale the axes, so the same field would look like a different one. The seeds are
    /// what the graph asked for, so they are what the frame is about.
    static constexpr double kFitMargin = 1.1;

    FieldLinesViewItem() = default;

    /**
     * @brief The name.
     *
     * @ownership   observes
     * @thread      any
     * @pre         none
     * @post        `kName`
     * @invariant   Constant
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.render.a_field_becomes_a_family_of_curves
     */
    [[nodiscard]] std::string_view name() const noexcept override { return kName; }

    /**
     * @brief Whether the declaration is this kit's field-line item.
     *
     * **This rule is what makes the item dimension-agnostic, and that is why the reference's separate electric-field
     * item is not ported.** The claim is on the **type name** and nothing else: whichever table the declaration's
     * wire leads to is the field that gets traced -- tesla or volts per metre, a dipole or a convection pattern --
     * and the tracer's own specification mentions no dimension either. So "draw the electric field's lines" is a
     * wire in this kit rather than a second item with the same arithmetic under a second name, and the case below
     * measures it: two declarations on one item, two tables with different dimensions, two different families of
     * curves.
     *
     * This contract is a **block comment** where its neighbours are doc-comment lines, and that is the gate's rule
     * rather than a style choice: it reads block comments, so a claim written in doc-comment lines is invisible to
     * it and the case is reported as an orphan. The trap fired three times in this tree before the rule was written
     * down here, which is the reason it is spelled out rather than remembered.
     *
     * @param type_name The render node's type.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        True exactly for `render.field_lines`
     * @invariant   Depends only on the type name
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.render.a_field_becomes_a_family_of_curves,
     *              magnetosphere.render.an_electric_field_is_drawn_by_the_same_item
     */
    [[nodiscard]] bool draws(std::string_view type_name) const noexcept override;

    /**
     * @brief Traces the declared field's lines and answers with them.
     *
     * This is the expensive item: one trace per seed, five field reads a stage, and the interface sanctions it
     * explicitly -- `IViewItem::scene` "may allocate and may be slow: this runs when a run finishes, not per
     * frame".
     *
     * @param request The graph, the declarations and the run's tables.
     *
     * @ownership   owns the returned scene
     * @thread      main
     * @pre         `request.valid()`
     * @post        One polyline per seed whose trace produced a curve, in `(x, z)`, with bounds unless nothing
     *              was drawn
     * @invariant   Does not modify the request
     * @errors      Cannot fail: a declaration whose field is missing, unreadable or not baked answers with an
     *              empty scene, which is the honest picture of a field that is not there
     * @complexity  O(seeds x points x stages)
     * @nondet      none
     * @frozen      no
     * @tests       magnetosphere.render.a_field_becomes_a_family_of_curves
     */
    [[nodiscard]] qp::graph::ViewScene scene(const qp::graph::ViewRequest& request) override;
};

}  // namespace qp::plugins::magnetosphere
