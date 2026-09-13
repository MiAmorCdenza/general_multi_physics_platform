/**
 * @file render_nodes.hpp
 * @brief What the graph says it wants **drawn**: a pure declaration, read by a view.
 *
 * ## Why the render domain is a declaration and not an implementation
 *
 * `graph/domain`'s own header argues it: giving a render node a `compute()` drags the view layer's
 * representation into the kernel, and that is how the previous project welded one transfer format into the graph.
 * So a render node has **no implementation, only a declaration**, and `build_plan` puts a declared output whose
 * node has no compute into `plan.render.declared` instead of into an evaluation plan. This type is the kit's
 * first use of that: it is how a graph says "draw these particles", and what actually draws them is a view item
 * -- content, in `plugins/views_items`, which is a category this platform has not built yet.
 *
 * ## Why it has an output port it never computes
 *
 * A declared output is a `(node, port)` pair, so a node that cannot be declared cannot be asked for. The port
 * here is therefore a **name for the thing being drawn**, not a value that exists: nothing produces it, nothing
 * reads it as a value, and `has_compute` false is what tells the platform so. The alternative -- declaring the
 * *pusher's* state port and teaching the view to guess that a render node exists nearby -- would put the "what
 * to draw" decision in the view instead of in the graph, which is exactly the inversion this design avoids.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No value is ever produced for any port of these types
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.render.the_item_is_declared_and_never_evaluated,
 *              magnetosphere.render.the_field_lines_type_declares_the_ports_the_item_reads
 */
#pragma once

#include <qp/graph/ir/descriptor.hpp>
#include <qp/host/host.hpp>

#include <cstddef>
#include <vector>

namespace qp::plugins::magnetosphere {

/**
 * @brief The render-domain node types of this kit.
 *
 * @ownership   observes
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   Every type here has `has_compute` false and is allowed in the render domain
 * @errors      See each declaration
 * @frozen      no
 * @tests       magnetosphere.render.the_item_is_declared_and_never_evaluated
 */
class RenderNodes final {
public:
    /// @brief The particle render item: "draw the particles arriving on this wire".
    static constexpr const char* kParticlesType = "render.particles";

    /// @brief The field-line render item: "draw the field arriving on this wire".
    ///
    /// The **second** render type, and the one that made the rest of the platform move: a field line is drawn from
    /// the field's own samples, so a view item needs them, and until this type existed no interface could give
    /// them to one (`IGraphRun::fields()` says so where it used to say it could not). A render node is still a
    /// declaration -- `has_compute` is false here too -- but a declaration may now **name a baked product**, which
    /// is the difference between "draw these particles" and "draw this field".
    static constexpr const char* kFieldLinesType = "render.field_lines";

    /// @brief The state channel: the particles to draw. Wired from a pusher or an emitter, never evaluated.
    static constexpr qp::graph::PortNumber kPortState = 1;
    /// @brief How many past positions to draw behind each particle. A parameter, not a socket.
    static constexpr qp::graph::PortNumber kPortTrail = 2;
    /// @brief The item itself, so the graph can declare what it wants drawn.
    static constexpr qp::graph::PortNumber kPortItem = 1;

    /// @brief The field-line item's data socket: the vector field to trace. Wired from a field node.
    ///
    /// The same number as `kPortState`, and that is not a collision: ports are numbered **within a type**, and the
    /// two types are different types. What is shared is the position, which is the point -- in both items the
    /// first socket is the thing being drawn, so a reader who has wired one has wired the other.
    static constexpr qp::graph::PortNumber kPortField = 1;
    /// @brief How many lines to trace, spread along the `+x` axis of the equatorial plane.
    static constexpr qp::graph::PortNumber kPortLineCount = 2;
    /// @brief Where the first seed sits on that axis, in earth radii.
    static constexpr qp::graph::PortNumber kPortSeedStart = 3;
    /// @brief Where the last seed sits, in earth radii. Seeds are spaced evenly between the two.
    static constexpr qp::graph::PortNumber kPortSeedEnd = 4;
    /// @brief The largest step the tracer takes along a line, in earth radii.
    static constexpr qp::graph::PortNumber kPortStepMax = 5;
    /// @brief The step error the tracer is allowed, in earth radii.
    static constexpr qp::graph::PortNumber kPortTolerance = 6;
    /// @brief The item's own output, so the graph can declare what it wants drawn.
    static constexpr qp::graph::PortNumber kPortFieldItem = 1;

    /// @brief The default trail length, in samples. Zero draws a point per particle.
    static constexpr std::int64_t kDefaultTrail = 64;
    /// @brief The default line count: enough to show the nesting, few enough to stay a picture.
    static constexpr std::int64_t kDefaultLineCount = 7;
    /// @brief The default seed range, in earth radii. Six radii spans the closed shells a first course draws.
    static constexpr double kDefaultSeedStart = 2.0;
    static constexpr double kDefaultSeedEnd = 8.0;

    RenderNodes() = delete;

    /// @brief The node types this build ships, ready to register with a host.
    ///
    /// @ownership   owns the returned descriptions
    /// @thread      main
    /// @pre         none
    /// @post        One description for each item, with the ports the view will read
    /// @invariant   `has_compute` is false, which is what keeps the type out of every evaluation plan
    /// @errors      May allocate; allocation failure terminates
    /// @complexity  O(1)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.render.the_item_is_declared_and_never_evaluated,
    ///              magnetosphere.render.the_field_lines_type_declares_the_ports_the_item_reads
    [[nodiscard]] static std::vector<qp::graph::NodeDesc> node_types();

    /// @brief Registers the render types with `host` as built-ins.
    ///
    /// @param host The host. Borrowed.
    ///
    /// @ownership   observes `host`
    /// @thread      main
    /// @pre         none
    /// @post        Every type whose name was free is registered
    /// @invariant   A type already registered under its name is left alone rather than duplicated
    /// @errors      noexcept
    /// @complexity  O(types)
    /// @nondet      none
    /// @frozen      no
    /// @tests       magnetosphere.render.the_item_is_declared_and_never_evaluated
    static std::size_t mount(qp::host::PluginHost& host) noexcept;
};

}  // namespace qp::plugins::magnetosphere
