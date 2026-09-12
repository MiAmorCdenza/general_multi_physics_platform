/**
 * @file field_nodes.cpp
 * @brief The bake: one dipole evaluated at every node of a grid, and the port table that describes it.
 *
 * The arithmetic is `dipole.at`, which `dipole.cpp` already argues about at length -- including the sign
 * convention that cost this project three attempts and was finally settled by differentiating `B = curl A` at
 * six points. What is left here is the plumbing that turns "a model" into "a field a kernel can read", and it is
 * worth naming because it is where a bake can be quietly wrong:
 *
 *   - **the descriptor must be the one the store's samples actually have.** It is taken from
 *     `BakedField::view()` rather than built a second time, so the counts a kernel reads and the vector it reads
 *     them out of cannot disagree;
 *   - **the layout is the ABI's**, `(i * ny + j) * nz + k` then component, because `abi::LatticeDesc` says so and
 *     two other files depend on it (`BakedField::sample` and `BorisAdvancer`'s `sample_volume`);
 *   - **a grid that cannot be baked is refused before anything is allocated.** A count one digit too large is
 *     not a slow run, it is a request no machine can satisfy, and the vector's answer to that is to throw.
 */
#include <qp/plugins/magnetosphere/field_nodes.hpp>

#include <qp/plugins/magnetosphere/baked_field.hpp>
#include <qp/plugins/magnetosphere/dipole.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace qp::plugins::magnetosphere {
namespace {

namespace graph = qp::graph;
namespace gfield = qp::graph::field;

/// @brief A parameter's value, or `fallback` when the node does not carry one.
[[nodiscard]] double real_or(const graph::InputView& inputs, graph::PortNumber port, double fallback) noexcept {
    const qp::ports::Value value = inputs.get(port);
    return value.valid() ? value.to_double() : fallback;
}

/// @brief An integer parameter's value, or `fallback` when the node does not carry one.
[[nodiscard]] std::uint32_t count_or(const graph::InputView& inputs, graph::PortNumber port,
                                     std::uint32_t fallback) noexcept {
    const qp::ports::Value value = inputs.get(port);
    if (!value.valid()) return fallback;
    const double raw = value.to_double();
    if (!std::isfinite(raw) || raw < 0.0) return fallback;
    return static_cast<std::uint32_t>(raw);
}

/// @brief A port that is a parameter: an input with no socket, whose value comes from the node.
///
/// **No default value here, and that is the convention rather than an omission.** `PortDesc` has no member for
/// one, and the platform's rule is that a default lives in the reader that would otherwise read a missing value,
/// so there is exactly one place per node type where "what a fresh node does" is written down. Here that place
/// is `read_from`, and the type's own test asserts the description and the reader agree about the port numbers.
[[nodiscard]] graph::PortDesc parameter(graph::PortNumber number, const char* name, const char* label,
                                        const char* unit, double step) {
    graph::PortDesc port;
    port.number = number;
    port.name = name;
    port.label = label;
    port.description = "A field node's bake configuration: a parameter rather than a socket, because a bake grid "
                       "is chosen once for the whole run and a wired one would have to be recomputed whenever "
                       "its source changed -- which is the field domain's whole promise.";
    port.type = qp::ports::kScalarF64;
    // Not connectable: this is the unified Param/Port rule's other half.
    port.connectable = false;
    port.required = true;
    port.unit_symbol = unit;
    port.step = step;
    return port;
}

}  // namespace

GridSpec FieldNodes::read_from(const graph::Node& node) noexcept {
    // One reader for two sources: the node's parameters are put into the same `InputView` shape the evaluator is
    // handed, so there is exactly one place where "port 6 is spacing x" is interpreted. The view borrows
    // `values`, which lives until this call returns -- long enough, because the result is a value.
    graph::PortValues values;
    values.reserve(node.params.size());
    for (const graph::ParamValue& p : node.params) values.emplace_back(p.number, p.value);
    return read_from(graph::InputView{values});
}

GridSpec FieldNodes::read_from(const graph::InputView& inputs) noexcept {
    GridSpec grid;
    grid.origin_m = Vec3{real_or(inputs, kPortOrigin0, -kDefaultHalfExtentRe * kEarthRadiusM),
                         real_or(inputs, kPortOrigin1, -kDefaultHalfExtentRe * kEarthRadiusM),
                         real_or(inputs, kPortOrigin2, -kDefaultHalfExtentRe * kEarthRadiusM)};
    grid.spacing_m = Vec3{real_or(inputs, kPortSpacing0, kDefaultSpacingRe * kEarthRadiusM),
                          real_or(inputs, kPortSpacing1, kDefaultSpacingRe * kEarthRadiusM),
                          real_or(inputs, kPortSpacing2, kDefaultSpacingRe * kEarthRadiusM)};
    grid.nx = count_or(inputs, kPortCount0, kDefaultNodesPerAxis);
    grid.ny = count_or(inputs, kPortCount1, kDefaultNodesPerAxis);
    grid.nz = count_or(inputs, kPortCount2, kDefaultNodesPerAxis);
    return grid;
}

namespace {

/// @brief Whether a grid can be baked: two nodes an axis, a positive spacing, and a size a machine has.
[[nodiscard]] bool bakeable(const GridSpec& grid) noexcept {
    if (grid.nx < 2 || grid.ny < 2 || grid.nz < 2) return false;
    if (grid.point_count() > FieldNodes::kMaxPoints) return false;
    const Vec3& s = grid.spacing_m;
    if (!(s.x > 0.0) || !(s.y > 0.0) || !(s.z > 0.0)) return false;
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z)) return false;
    return is_finite(grid.origin_m);
}

}  // namespace

bool bake_dipole(double tilt_degrees, double moment_am2, const GridSpec& grid, gfield::FieldKey key,
                 gfield::FieldSet& fields) {
    if (!std::isfinite(tilt_degrees) || !std::isfinite(moment_am2)) return false;
    if (!bakeable(grid)) return false;

    const DipoleField dipole{tilt_degrees, moment_am2};
    BakedField table{grid.origin_m, grid.spacing_m, grid.nx, grid.ny, grid.nz};
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                table.set_node(i, j, k, dipole.at(grid.node_position(i, j, k)));
            }
        }
    }

    // The descriptor is the table's own, taken **before** the samples move, and the samples move rather than
    // copy: a 100 MB field copied once per bake for no reason is a cost a user would feel as a pause with
    // nothing to explain it.
    const qp::abi::LatticeDesc desc = table.view().desc;
    return fields.publish(key, desc, std::move(table.data()));
}

std::vector<graph::NodeDesc> FieldNodes::node_types() {
    graph::NodeDesc dipole;
    dipole.type_name = kDipoleType;
    dipole.label = "Dipole field";
    dipole.description = "The Earth's field as a tilted dipole: B = (mu0/4pi) (3 (m.rhat) rhat - m) / r^3, "
                        "baked onto a uniform grid in tesla. About 11% of the real surface field is not a "
                        "dipole, which is what a report prints beside this node.";
    dipole.category = "field";
    dipole.version = 1;
    // Field domain: yes, and that is the point -- a bake may allocate, may block and may call a foreign
    // solver, which is exactly what the particle domain forbids.
    dipole.allow_in_field_domain = true;
    dipole.allow_in_particle_domain = false;
    // It has an implementation, but not one `evaluate_graph` can describe as a port value alone: what it
    // produces is samples in a store plus a handle. `has_compute` is true because the node **is** computed.
    dipole.has_compute = true;

    graph::PortDesc tilt = parameter(kPortTiltDegrees, "tilt_degrees", "Tilt", "deg", 0.5);
    tilt.description = "The magnetic latitude of the dipole axis. 11.5 degrees is the Earth's, and it is the "
                       "reason the aurora is not centred on the geographic pole.";
    graph::PortDesc moment = parameter(kPortMomentAm2, "moment_am2", "Moment", "A m^2", 1.0e21);
    moment.description = "The dipole moment. The default is the geomagnetic one, 7.708e22 A m^2, which puts "
                         "29 709 nT on the magnetic equator.";
    dipole.inputs.push_back(tilt);
    dipole.inputs.push_back(moment);
    dipole.inputs.push_back(parameter(kPortOrigin0, "origin_x", "Grid origin x", "m", kEarthRadiusM));
    dipole.inputs.push_back(parameter(kPortOrigin1, "origin_y", "Grid origin y", "m", kEarthRadiusM));
    dipole.inputs.push_back(parameter(kPortOrigin2, "origin_z", "Grid origin z", "m", kEarthRadiusM));
    dipole.inputs.push_back(parameter(kPortSpacing0, "spacing_x", "Grid spacing x", "m", 0.1 * kEarthRadiusM));
    dipole.inputs.push_back(parameter(kPortSpacing1, "spacing_y", "Grid spacing y", "m", 0.1 * kEarthRadiusM));
    dipole.inputs.push_back(parameter(kPortSpacing2, "spacing_z", "Grid spacing z", "m", 0.1 * kEarthRadiusM));
    dipole.inputs.push_back(parameter(kPortCount0, "count_x", "Nodes along x", "", 1.0));
    dipole.inputs.push_back(parameter(kPortCount1, "count_y", "Nodes along y", "", 1.0));
    dipole.inputs.push_back(parameter(kPortCount2, "count_z", "Nodes along z", "", 1.0));

    graph::PortDesc field;
    field.number = kPortField;
    field.name = "field";
    field.label = "Magnetic field";
    field.description = "The baked field, as a volume of tesla vectors. Wire it into a pusher's magnetic input; "
                        "the port type is what stops it being wired into anything else.";
    field.type = qp::ports::kVectorField;
    field.connectable = true;
    field.required = false;
    field.unit_symbol = "T";
    dipole.outputs.push_back(field);

    return {std::move(dipole)};
}

std::size_t FieldNodes::mount(qp::host::PluginHost& host) noexcept {
    std::size_t registered = 0;
    for (graph::NodeDesc& desc : node_types()) {
        if (host.add_builtin_node_type(std::move(desc)) == qp::diag::ErrorCode::ok) ++registered;
    }
    return registered;
}

qp::diag::Result<std::vector<std::pair<graph::PortNumber, qp::ports::Value>>> DipoleEvaluator::evaluate(
    graph::NodeId id, const graph::NodeDesc& desc,
    const std::vector<std::pair<graph::PortNumber, qp::ports::Value>>& inputs) {
    using Outcome = std::vector<std::pair<graph::PortNumber, qp::ports::Value>>;
    if (desc.type_name != FieldNodes::kDipoleType) {
        // Not mine. See the header: the bake is called for every node in the graph, including the particle and
        // render domains, and a bake that failed on them would make a mixed graph unbakeable.
        return qp::diag::Result<Outcome>{Outcome{}};
    }

    const graph::InputView view{inputs};
    const GridSpec grid = FieldNodes::read_from(view);
    const double tilt = real_or(view, FieldNodes::kPortTiltDegrees, kMagneticTiltDegrees);
    const double moment = real_or(view, FieldNodes::kPortMomentAm2, kDipoleMomentAm2);
    if (!std::isfinite(tilt) || !std::isfinite(moment)) {
        return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
    }

    const gfield::FieldKey key{id.index, FieldNodes::kPortField};
    if (!bake_dipole(tilt, moment, grid, key, *fields_)) {
        return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
    }

    // The handle is the store's own descriptor. Building a second one here would be a second definition of the
    // lattice, and the two would agree until somebody changed one of them -- at which point a consumer would
    // read a 17-node grid out of a 33-node buffer.
    Outcome out;
    out.emplace_back(FieldNodes::kPortField, qp::ports::Value{fields_->view(key).desc});
    return qp::diag::Result<Outcome>{std::move(out)};
}

}  // namespace qp::plugins::magnetosphere
