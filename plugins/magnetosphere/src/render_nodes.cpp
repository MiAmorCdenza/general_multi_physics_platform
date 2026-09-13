/**
 * @file render_nodes.cpp
 * @brief The declaration, and the two flags that make it a declaration rather than a computation.
 *
 * `has_compute` false is the whole mechanism: it is what `build_plan` reads when it decides that a declared
 * output belongs in `plan.render.declared` rather than in an evaluation plan, and it is what makes
 * `evaluate_graph` produce nothing for this node. A render type that set it true would be evaluated during a
 * bake -- the exact weld between the view layer's representation and the kernel that this design exists to
 * prevent.
 */
#include <qp/plugins/magnetosphere/render_nodes.hpp>

#include <utility>

namespace qp::plugins::magnetosphere {
namespace {

namespace graph = qp::graph;

}  // namespace

std::vector<graph::NodeDesc> RenderNodes::node_types() {
    graph::NodeDesc item;
    item.type_name = kParticlesType;
    item.label = "Particles";
    item.description = "Draws the particles arriving on the state wire, as points with an optional trail. A "
                       "declaration and not a computation: nothing is evaluated, and what draws it is a view "
                       "item.";
    item.category = "render";
    item.version = 1;
    // **No implementation.** See the file comment: this is what keeps the node out of every evaluation plan.
    item.has_compute = false;
    // The render domain has no realtime constraint of its own -- it is not evaluated -- so the two domain flags
    // say only "this is not bake content and not step content". Everything that matters is `has_compute`.
    item.allow_in_field_domain = false;
    item.allow_in_particle_domain = false;

    graph::PortDesc state;
    state.number = kPortState;
    state.name = "state";
    state.label = "Particle state";
    state.description = "The particles to draw. No value crosses this wire; it is how the graph says which "
                        "particles the item is about.";
    state.type = qp::ports::kParticleBuffer;
    state.connectable = true;
    state.required = true;
    item.inputs.push_back(state);

    graph::PortDesc trail;
    trail.number = kPortTrail;
    trail.name = "trail";
    trail.label = "Trail";
    trail.description = "How many past positions to draw behind each particle. Zero draws a point each, which "
                        "is what a snapshot view wants.";
    trail.type = qp::ports::kInt64;
    trail.connectable = false;
    trail.required = true;
    trail.step = 1.0;
    item.inputs.push_back(trail);

    graph::PortDesc out;
    out.number = kPortItem;
    out.name = "item";
    out.label = "Render item";
    out.description = "A name for the thing being drawn, so the graph can declare what it wants. No value is "
                      "ever produced for it.";
    out.type = qp::ports::kParticleBuffer;
    out.connectable = true;
    out.required = false;
    item.outputs.push_back(out);

    return {std::move(item)};
}

std::size_t RenderNodes::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (graph::NodeDesc& desc : node_types()) {
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

}  // namespace qp::plugins::magnetosphere
