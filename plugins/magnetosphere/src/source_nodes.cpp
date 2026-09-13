/**
 * @file source_nodes.cpp
 * @brief The driver's arithmetic and its type table: two formulas and one number.
 *
 * There is no evaluator here. A driver publishes a `ports::Value` and nothing else -- no table, no geometry, no
 * domain -- so the field evaluator's `evaluate` answers for it with one line, which is the whole of what a driver
 * costs this kit.
 */
#include <qp/plugins/magnetosphere/source_nodes.hpp>

#include <qp/ports/port_type.hpp>
#include <qp/ports/value.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace qp::plugins::magnetosphere {

namespace {

namespace graph = qp::graph;

/// @brief The index a node carries, before clamping: the parameter, or the default when it has none.
[[nodiscard]] double raw_kp(const graph::Node& node) noexcept {
    const qp::ports::Value value = node.param(SourceNodes::kPortKp);
    if (!value.valid()) return SourceNodes::kDefaultKp;
    const double kp = value.to_double();
    return std::isfinite(kp) ? kp : SourceNodes::kDefaultKp;
}

}  // namespace

double SourceNodes::standoff_re_for_kp(double kp) noexcept {
    const double clamped = std::clamp(kp, kMinKp, kMaxKp);
    const double pdyn = 2.0 + clamped * 0.5;
    // `pdyn` is at least 2 by construction, so the cube root is of a positive number and the result is finite.
    return 10.0 / std::cbrt(pdyn);
}

double SourceNodes::flaring_for_kp(double kp) noexcept {
    const double clamped = std::clamp(kp, kMinKp, kMaxKp);
    return 0.55 + clamped * 0.02;
}

double SourceNodes::read_kp(const graph::Node& node) noexcept {
    return std::clamp(raw_kp(node), kMinKp, kMaxKp);
}

std::vector<graph::NodeDesc> SourceNodes::node_types() {
    graph::NodeDesc kp;
    kp.type_name = kKpType;
    kp.label = "Kp index";
    kp.description = "The planetary K index, as a number on a wire. One driver feeding several models is the point: "
                     "wire it into a magnetopause and the standoff distance and the flaring both move together, "
                     "instead of each node being told the same day by hand.";
    kp.category = "source";
    kp.version = 1;
    // A field-domain node: it is evaluated with the field graph, and it declares no geometry because a number has
    // none. It is **not** a field table, which is why it publishes `kScalarF64` rather than `kScalarField`.
    kp.allow_in_field_domain = true;
    kp.allow_in_particle_domain = false;
    kp.has_compute = true;

    graph::PortDesc index;
    index.number = kPortKp;
    index.name = "kp";
    index.label = "Kp";
    index.description = "The index, 0 to 9. Two is a quiet-to-moderate day and nine is a severe storm; the value is "
                        "clamped into that range rather than refused, so a node being edited still evaluates.";
    index.type = qp::ports::kScalarF64;
    index.connectable = false;
    index.required = true;
    index.unit_symbol = "1";
    index.step = 0.333;
    kp.inputs.push_back(index);

    graph::PortDesc out;
    out.number = kPortKpOut;
    out.name = "kp";
    out.label = "Kp";
    out.description = "The index, on a wire. A consumer that wants it declares a socket and says what it does when "
                      "the socket is empty.";
    out.type = qp::ports::kScalarF64;
    out.connectable = true;
    out.required = false;
    out.unit_symbol = "1";
    kp.outputs.push_back(out);

    return {std::move(kp)};
}

std::size_t SourceNodes::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (graph::NodeDesc& desc : node_types()) {
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

}  // namespace qp::plugins::magnetosphere
