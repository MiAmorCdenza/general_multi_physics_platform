/**
 * @file view_item.cpp
 * @brief The projection, and the fit that goes with it.
 *
 * Two decisions are visible in the arithmetic and worth stating where it is:
 *
 *   - **`x` and `y` are already earth radii.** The run reports positions in `R_E` -- that conversion happens at
 *     the kit's own boundary, in `MagnetosphereRun::positions` -- so this file does no unit work at all. An item
 *     that converted would be a second place the unit system lives.
 *   - **the fit is symmetric about the origin**, because the interesting thing in a magnetosphere picture is the
 *     Earth at the centre and a fit that followed the population's own centre would slide the planet off the
 *     frame the moment the population drifted. The half-width is the largest radius present, times a margin.
 */
#include <qp/plugins/magnetosphere/view_item.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

#include <qp/plugins/magnetosphere/render_nodes.hpp>

namespace qp::plugins::magnetosphere {
namespace {

namespace graph = qp::graph;

}  // namespace

bool ParticleViewItem::draws(std::string_view type_name) const noexcept {
    return type_name == RenderNodes::kParticlesType;
}

graph::ViewScene ParticleViewItem::scene(const graph::ViewRequest& request) {
    graph::ViewScene out;
    if (request.positions == nullptr) return out;
    const std::vector<double>& positions = *request.positions;
    if (positions.size() < 3) return out;

    double largest = 0.0;
    for (std::size_t i = 0; i + 2 < positions.size(); i += 3) {
        const double x = positions[i];
        const double y = positions[i + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        out.points.push_back(graph::ViewScene::Point{x, y});
        const double radius = std::sqrt(x * x + y * y);
        if (radius > largest) largest = radius;
    }
    if (out.points.empty()) return out;

    // A floor of one earth radius, so a population sitting near the origin still produces a frame with a scale
    // rather than a degenerate box that a host would have to special-case.
    const double half = largest > 1.0 ? largest * kFitMargin : kFitMargin;
    out.x_min = -half;
    out.x_max = half;
    out.y_min = -half;
    out.y_max = half;
    out.has_bounds = true;
    return out;
}

}  // namespace qp::plugins::magnetosphere
