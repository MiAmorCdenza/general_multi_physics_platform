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

/// @brief A render item's own configuration: a parameter, not a socket.
///
/// The same shape `field_nodes.cpp` uses for a bake grid and for the same reason -- a wired parameter would have to
/// be recomputed whenever its source changed -- with one difference worth stating: a **render** node's parameters
/// are never read by an evaluator at all, because the node is never evaluated. They are read by the view item,
/// through the graph, which is the only place they live.
[[nodiscard]] graph::PortDesc parameter(graph::PortNumber number, const char* name, const char* label,
                                        const char* unit, double step) {
    graph::PortDesc port;
    port.number = number;
    port.name = name;
    port.label = label;
    port.description = "The item's own configuration. Read by the view item through the graph, never by an "
                       "evaluator: a render node has no implementation.";
    port.type = qp::ports::kScalarF64;
    port.connectable = false;
    port.required = true;
    port.unit_symbol = unit;
    port.step = step;
    return port;
}

/// @brief The ports both render items share: the thing being drawn, and the item's own name for itself.
///
/// Factored out because the two items differ only in **what** they draw and in which parameters they carry, and a
/// second hand-written copy of the output port is a second place "no value is ever produced" has to stay true.
[[nodiscard]] graph::PortDesc item_output() {
    graph::PortDesc out;
    out.number = RenderNodes::kPortFieldItem;
    out.name = "item";
    out.label = "Render item";
    out.description = "A name for the thing being drawn, so the graph can declare what it wants. No value is "
                      "ever produced for it.";
    out.type = qp::ports::kParticleBuffer;
    out.connectable = true;
    out.required = false;
    return out;
}

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

    // ---------------- the field-line item ----------------
    graph::NodeDesc lines;
    lines.type_name = kFieldLinesType;
    lines.label = "Field lines";
    lines.description = "Traces the field lines of the vector field arriving on the data wire and draws them in "
                        "the meridional plane. A declaration like the particle item -- nothing here is ever "
                        "evaluated -- but a declaration that **names a baked product**, so the item that draws it "
                        "reads the run's own field samples.";
    lines.category = "render";
    lines.version = 1;
    lines.has_compute = false;
    lines.allow_in_field_domain = false;
    lines.allow_in_particle_domain = false;

    graph::PortDesc data;
    data.number = kPortField;
    data.name = "field";
    data.label = "Field";
    data.description = "The vector field to trace. Wired from a field node's output; no value crosses it, and "
                       "the item follows the wire to the samples the bake published under that node.";
    data.type = qp::ports::kVectorField;
    data.connectable = true;
    data.required = true;
    lines.inputs.push_back(data);

    graph::PortDesc count = parameter(kPortLineCount, "line_count", "Lines", "", 1.0);
    count.description = "How many lines to trace. The seeds are spread evenly along the +x axis of the "
                        "equatorial plane, so a count of seven is the nested family a first course draws.";
    count.type = qp::ports::kInt64;
    lines.inputs.push_back(count);
    graph::PortDesc seed_start = parameter(kPortSeedStart, "seed_start", "First seed", "R_E", 0.5);
    seed_start.description = "Where the innermost line crosses the equator. Below about 1.5 the line's foot "
                             "points crowd into the polar cap and the inner shells stop being distinguishable.";
    lines.inputs.push_back(seed_start);
    graph::PortDesc seed_end = parameter(kPortSeedEnd, "seed_end", "Last seed", "R_E", 0.5);
    seed_end.description = "Where the outermost line crosses the equator. Past the table's own box the trace "
                           "stops as `left_table` and the line is simply short, which is visible rather than "
                           "wrong.";
    lines.inputs.push_back(seed_end);
    graph::PortDesc step_max = parameter(kPortStepMax, "step_max", "Step cap", "R_E", 0.05);
    step_max.description = "The largest step the tracer takes along a line. This is a drawing resolution and not "
                           "an accuracy: the tracer chooses its own step from its error estimate, and the case "
                           "that pins it traces one line at caps two orders apart and measures the same shape.";
    lines.inputs.push_back(step_max);
    graph::PortDesc tolerance = parameter(kPortTolerance, "tolerance", "Step error", "R_E", 1.0e-5);
    tolerance.description = "The error the tracer allows per step. This and not the cap decides where a line "
                            "goes; the default is four orders below the table's own interpolation error, so "
                            "tightening it resolves the interpolant rather than the model.";
    lines.inputs.push_back(tolerance);
    lines.outputs.push_back(item_output());

    return {std::move(item), std::move(lines)};
}

std::size_t RenderNodes::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (graph::NodeDesc& desc : node_types()) {
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

}  // namespace qp::plugins::magnetosphere
