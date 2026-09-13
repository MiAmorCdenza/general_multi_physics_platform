/**
 * @file view_item.hpp
 * @brief The kit's drawing side: a particle snapshot becomes a scene.
 *
 * `render.particles` is what a graph **declares**; this is what reads the declaration and answers with something
 * drawable. It is the other half of the pair, and it lives here rather than in `views/` because it is content:
 * the kit knows what a particle is and what "particles at six earth radii" should look like, and the window
 * knows nothing about either.
 *
 * ## Why the scene is 2-D and the projection is stated
 *
 * `ViewScene` is points in a plane. A ring of particles gyrating in the equatorial plane projects onto it
 * without loss of the interesting structure; a particle's `z` is dropped, and that is a **stated** limitation
 * rather than an oversight: a 3-D scene needs a camera, and a camera is the host's business, not an item's. The
 * reopening condition is a second item that draws the same particles from another angle -- at which point the
 * scene grows a third coordinate and the host grows a projection, and neither change is this file's.
 *
 * ## What the item does not do
 *
 * It keeps **no state**, which the interface requires and which decides two things a reader might expect here:
 *
 *   - **no trail.** A trail is a series of snapshots, and a request carries one. The host accumulates them --
 *     it owns the clock -- and the render node's `trail` parameter is what tells it how many samples to keep.
 *   - **no fitted-to-the-experiment bounds.** The bounds come from the population in this snapshot, because
 *     that is what the item can see: an item that reached back through the graph for the emitter's L shell would
 *     be reading an experiment's parameter to decide how to draw a picture, and the fit would then lie whenever
 *     the population did not fill the region.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   The same request produces the same scene
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.render.a_snapshot_becomes_a_scene
 */
#pragma once

#include <qp/graph/domain/view_items.hpp>

#include <string_view>

namespace qp::plugins::magnetosphere {

/**
 * @brief Draws `render.particles`: one point per particle, in the equatorial plane.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Stateless: two calls with equal requests give equal scenes
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.render.a_snapshot_becomes_a_scene
 */
class ParticleViewItem final : public qp::graph::IViewItem {
public:
    /// @brief The name a panel title and a status line show.
    static constexpr const char* kName = "particles";

    /// @brief The fraction of the population's extent the fitted box adds around it.
    ///
    /// A margin rather than a tight fit: a particle on the boundary of the view is a particle whose next step
    /// leaves it, and redrawing the axes every frame is what makes a plot unreadable. The value is a drawing
    /// decision, not a physical one, and it is named so it can be argued with.
    static constexpr double kFitMargin = 1.2;

    ParticleViewItem() = default;

    /// @brief The name.
    ///
    /// @ownership   observes
    /// @thread      any
    /// @pre         none
    /// @post        `kName`
    /// @invariant   Constant
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.render.a_snapshot_becomes_a_scene
    [[nodiscard]] std::string_view name() const noexcept override { return kName; }

    /// @brief Whether the declaration is this kit's particle item.
    ///
    /// @param type_name The render node's type.
    ///
    /// @ownership   pure
    /// @thread      main
    /// @pre         none
    /// @post        True exactly for `render.particles`
    /// @invariant   Depends only on the type name
    /// @errors      noexcept
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.render.a_snapshot_becomes_a_scene
    [[nodiscard]] bool draws(std::string_view type_name) const noexcept override;

    /// @brief Turns a position snapshot into a scene.
    ///
    /// @param request The graph, the declarations and the snapshot. A null or empty snapshot produces an empty
    ///                scene, which is a window that has not run yet rather than a failure.
    ///
    /// @ownership   owns the returned scene
    /// @thread      main
    /// @pre         `request.valid()`
    /// @post        One point per particle, `x` and `y` in earth radii, with bounds unless there are none
    /// @invariant   Does not modify the request
    /// @errors      Cannot fail: a request that cannot be drawn answers with an empty scene
    /// @complexity  O(particles)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.render.a_snapshot_becomes_a_scene
    [[nodiscard]] qp::graph::ViewScene scene(const qp::graph::ViewRequest& request) override;
};

}  // namespace qp::plugins::magnetosphere
