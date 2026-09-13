/**
 * @file field_lines_item.cpp
 * @brief Reads the declaration, follows its wire into the run's store, and traces what it finds there.
 *
 * The whole file is one lookup and one loop. The lookup is the part worth reading: a render node is never
 * evaluated, so the **only** way this item can find the field it was told to draw is to follow the graph's own
 * wire from the node's data socket to the field node behind it, and then to look that `(node, port)` pair up in
 * the store the run published. Three separate mechanisms -- the declaration, the wire, the published table --
 * have to agree about which field is meant, and the case that covers this asserts the pair rather than the curve,
 * because a wrong pair produces a perfectly plausible picture of a different field.
 */
#include <qp/plugins/magnetosphere/field_lines_item.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/render_nodes.hpp>
#include <qp/plugins/magnetosphere/trace.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qp::plugins::magnetosphere {
namespace {

namespace graph = qp::graph;
namespace gfield = qp::graph::field;

/// The most lines one declaration may ask for. A guardrail rather than a preference: the count is a number a user
/// types, each line costs up to two thousand points, and a picture with ten thousand curves is a black rectangle
/// that took a minute to produce. Sixty-four is more nesting than a magnetosphere picture can show.
constexpr std::uint32_t kMaxLines = 64;
/// The innermost a seed may sit, in earth radii: just above the surface the traces stop at.
constexpr double kMinSeedRe = 1.05;

/// @brief A numeric parameter, or `fallback` when the node does not carry one.
[[nodiscard]] double real_or(const graph::Node& node, graph::PortNumber port, double fallback) noexcept {
    const qp::ports::Value value = node.param(port);
    if (!value.valid()) return fallback;
    const double raw = value.to_double();
    return std::isfinite(raw) ? raw : fallback;
}

/// @brief The field node behind a render node's data socket, and the port it comes out of.
///
/// The same walk the composition root and the composition plan do, and for the same reason: the wire is what says
/// which field is meant. A value crossing it would have to carry the identity of a table, which `ports::Value`
/// deliberately cannot hold.
struct Wire final {
    graph::NodeId node{};
    graph::PortNumber port = 0;
};

[[nodiscard]] Wire field_behind(const graph::Graph& g, graph::NodeId render_node) noexcept {
    const graph::Edge* edge = g.incoming(
        graph::PortRef{render_node, RenderNodes::kPortField, graph::PortDirection::input});
    if (edge == nullptr) return Wire{};
    return Wire{edge->from.node, edge->from.port};
}

}  // namespace

bool FieldLinesViewItem::draws(std::string_view type_name) const noexcept {
    return type_name == RenderNodes::kFieldLinesType;
}

graph::ViewScene FieldLinesViewItem::scene(const graph::ViewRequest& request) {
    graph::ViewScene out;
    if (!request.valid() || request.graph == nullptr || request.declared == nullptr) return out;
    // A run that baked nothing has no samples to trace, and the answer is an empty scene rather than a complaint:
    // a window whose graph declares field lines but has not been run yet is the ordinary state of a window.
    if (request.fields == nullptr) return out;
    const graph::Graph& g = *request.graph;

    for (const graph::DeclaredOutput& declaration : *request.declared) {
        const graph::Node* render_node = g.find_node(declaration.node);
        if (render_node == nullptr || !draws(render_node->type_name)) continue;

        const Wire wire = field_behind(g, declaration.node);

        // The samples, by the pair the bake published them under. `is_readable` rather than a null check, for the
        // reason `field_set.hpp` gives: an absent field and a field of zeros are the same force and different
        // experiments, and this predicate is what tells them apart.
        const gfield::FieldValue table = request.fields->view(gfield::FieldKey{wire.node.index, wire.port});
        if (!gfield::is_readable(table)) continue;

        // **Where the samples are, asked of the node that baked them.** This used to read the grid off the node the
        // wire names -- `FieldNodes::read_from(*field_node)` -- which works for a dipole and fails, silently, for
        // every combinator: `field.mul`, `field.blend` and `field.mix` declare **no grid ports at all**, so the
        // read produced a default grid, `tracer.usable()` was false, and the panel said "nothing to draw yet --
        // press Run" about a run that had just baked the field. Found by running the application with a graph wired
        // through `field.mix`; the case wires one through `field.mul`, so the fix is asserted rather than intended.
        // `resolve_field_origin` is the resolver the plan builder uses, and it follows the combinators back.
        GridSpec grid;
        if (!resolve_field_origin(g, declaration.node, RenderNodes::kPortField, grid)) continue;

        TraceSpec spec;
        spec.tolerance = std::max(1.0e-9, real_or(*render_node, RenderNodes::kPortTolerance, spec.tolerance));
        spec.step_max_re = std::max(1.0e-4, real_or(*render_node, RenderNodes::kPortStepMax, spec.step_max_re));
        const FieldTracer tracer{table, grid.origin_m, grid.spacing_m, spec};
        if (!tracer.usable()) continue;

        const auto count = static_cast<std::uint32_t>(
            std::clamp(real_or(*render_node, RenderNodes::kPortLineCount,
                               static_cast<double>(RenderNodes::kDefaultLineCount)),
                       0.0, static_cast<double>(kMaxLines)));
        double first = real_or(*render_node, RenderNodes::kPortSeedStart, RenderNodes::kDefaultSeedStart);
        double last = real_or(*render_node, RenderNodes::kPortSeedEnd, RenderNodes::kDefaultSeedEnd);
        if (last < first) std::swap(first, last);
        // A seed inside the planet is a line that starts underground, so the range is lifted to just above the
        // surface the traces stop at rather than refused: a slider that reached zero should show the innermost
        // line it can, not nothing.
        first = std::max(first, kMinSeedRe);
        last = std::max(last, kMinSeedRe);

        for (std::uint32_t line = 0; line < count; ++line) {
            const double t = count <= 1 ? 0.0 : static_cast<double>(line) / static_cast<double>(count - 1);
            const double radius = first + (last - first) * t;
            const FieldLine traced = tracer.trace(Vec3{radius, 0.0, 0.0});
            if (!traced.usable()) continue;
            // **The meridional plane**: `x` across, `z` up. A dipole's lines leave the equatorial plane
            // immediately -- the field there points out of it -- so the equatorial projection the particle item
            // uses is not an option here, and the plane every textbook draws a magnetosphere in is this one.
            std::vector<graph::ViewScene::Point> curve;
            curve.reserve(traced.points_re.size());
            for (const Vec3& point : traced.points_re) {
                curve.push_back(graph::ViewScene::Point{point.x, point.z});
            }
            out.polylines.push_back(std::move(curve));
        }

        // The frame is the **seeds** and not the curves, so that a line which stopped early does not rescale the
        // picture: the same graph draws the same axes whether its outermost trace ran to the edge of the table or
        // stopped a third of the way there.
        const double half = std::max(last, 1.0) * kFitMargin;
        out.x_min = -half;
        out.x_max = half;
        out.y_min = -half;
        out.y_max = half;
        out.has_bounds = !out.polylines.empty();
        // The body the traces stop at, one earth radius across, drawn under the curves.
        out.body_radius = 1.0;
        return out;
    }
    return out;
}

}  // namespace qp::plugins::magnetosphere
