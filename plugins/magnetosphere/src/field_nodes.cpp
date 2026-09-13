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

GridSpec FieldNodes::read_from(const graph::Node& node, const graph::PortNumber origin_port) noexcept {
    // One reader for two sources: the node's parameters are put into the same `InputView` shape the evaluator is
    // handed, so there is exactly one place where "port 6 is spacing x" is interpreted. The view borrows
    // `values`, which lives until this call returns -- long enough, because the result is a value.
    graph::PortValues values;
    values.reserve(node.params.size());
    for (const graph::ParamValue& p : node.params) values.emplace_back(p.number, p.value);
    return read_from(graph::InputView{values}, origin_port);
}

GridSpec FieldNodes::read_from(const graph::InputView& inputs, const graph::PortNumber origin_port) noexcept {
    GridSpec grid;
    grid.origin_m = Vec3{real_or(inputs, origin_port, -kDefaultHalfExtentRe * kEarthRadiusM),
                         real_or(inputs, origin_port + 1, -kDefaultHalfExtentRe * kEarthRadiusM),
                         real_or(inputs, origin_port + 2, -kDefaultHalfExtentRe * kEarthRadiusM)};
    grid.spacing_m = Vec3{real_or(inputs, origin_port + 3, kDefaultSpacingRe * kEarthRadiusM),
                          real_or(inputs, origin_port + 4, kDefaultSpacingRe * kEarthRadiusM),
                          real_or(inputs, origin_port + 5, kDefaultSpacingRe * kEarthRadiusM)};
    grid.nx = count_or(inputs, origin_port + 6, kDefaultNodesPerAxis);
    grid.ny = count_or(inputs, origin_port + 7, kDefaultNodesPerAxis);
    grid.nz = count_or(inputs, origin_port + 8, kDefaultNodesPerAxis);
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

bool bake_uniform(const Vec3& value, const GridSpec& grid, gfield::FieldKey key, gfield::FieldSet& fields,
                  const qp::abi::FieldDim dimension) {
    if (!is_finite(value)) return false;
    if (!bakeable(grid)) return false;
    BakedField table{grid.origin_m, grid.spacing_m, grid.nx, grid.ny, grid.nz};
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) table.set_node(i, j, k, value);
        }
    }
    const qp::abi::LatticeDesc desc = table.view(dimension).desc;
    return fields.publish(key, desc, std::move(table.data()));
}

bool bake_sum(const gfield::FieldValue& a, const gfield::FieldValue& b, gfield::FieldKey key,
              gfield::FieldSet& fields) {    if (!gfield::is_readable(a) || !gfield::is_readable(b)) return false;
    if (a.kind() != gfield::Kind::Volume || b.kind() != gfield::Kind::Volume) return false;
    if (!a.is_vector() || !b.is_vector()) return false;
    if (a.desc.element != qp::abi::ElementType::f64 || b.desc.element != qp::abi::ElementType::f64) return false;
    // The lattices must **agree**, not merely be addable: two tables with different counts describe different
    // regions, and fitting one to the other would be inventing a field neither model produced.
    for (int axis = 0; axis < 3; ++axis) {
        if (a.desc.count[axis] != b.desc.count[axis]) return false;
    }
    const auto* left = static_cast<const double*>(a.data);
    const auto* right = static_cast<const double*>(b.data);
    if (left == nullptr || right == nullptr) return false;

    const std::size_t values = static_cast<std::size_t>(a.point_count()) * 3;
    std::vector<double> sums(values);
    for (std::size_t i = 0; i < values; ++i) sums[i] = left[i] + right[i];

    return fields.publish(key, a.desc, std::move(sums));
}

const char* FieldNodes::to_string(MaskRegion region) noexcept {
    switch (region) {
        case MaskRegion::sphere: return "sphere";
        case MaskRegion::shell: return "shell";
        case MaskRegion::dayside: return "dayside";
        case MaskRegion::nightside: return "nightside";
    }
    return "sphere";
}

FieldNodes::MaskSpec FieldNodes::read_mask_from(const graph::InputView& inputs) noexcept {
    MaskSpec mask;
    const double region = real_or(inputs, kPortMaskRegion, 0.0);
    // The choice is clamped rather than refused, for the reason the parameter readers give everywhere in this kit:
    // a half-filled node is a graph being edited, and refusing to bake it would make the picture vanish while the
    // user is still choosing.
    const auto index = static_cast<std::int32_t>(region);
    mask.region = index >= 0 && index < static_cast<std::int32_t>(kMaskRegionCount)
                      ? static_cast<MaskRegion>(index)
                      : MaskRegion::sphere;
    mask.r0_m = real_or(inputs, kPortMaskR0, kDefaultMaskR0Re * kEarthRadiusM);
    mask.r1_m = real_or(inputs, kPortMaskR1, kDefaultMaskR1Re * kEarthRadiusM);
    // Ordered here rather than rejected, which is the reference implementation's own choice and the right one: a
    // user who swaps the two radii has said something meaningful -- the same shell -- and an empty mask would look
    // like a broken node.
    if (mask.r1_m < mask.r0_m) std::swap(mask.r0_m, mask.r1_m);
    return mask;
}

FieldNodes::MaskSpec FieldNodes::read_mask(const graph::Node& node) noexcept {
    // One reader for two sources: the node's parameters are put into the same `InputView` shape the evaluator is
    // handed, so there is exactly one place where "port 2 is r0" is interpreted. The view borrows `values`, which
    // lives until this call returns -- long enough, because the result is a value.
    graph::PortValues values;
    values.reserve(node.params.size());
    for (const graph::ParamValue& p : node.params) values.emplace_back(p.number, p.value);
    return read_mask_from(graph::InputView{values});
}

bool bake_mask(const FieldNodes::MaskSpec& mask, const GridSpec& grid, gfield::FieldKey key,
               gfield::FieldSet& fields) {
    if (!bakeable(grid)) return false;
    if (!std::isfinite(mask.r0_m) || !std::isfinite(mask.r1_m)) return false;
    // **Refused rather than reordered, and the difference from the reader is the point.** `read_mask` orders the
    // two radii, because a user who swaps them in the property panel has described the same shell and the
    // invariant on `MaskSpec` says so. A spec that arrives here reversed was *built* reversed -- by a caller that
    // did not use the reader -- and baking it would produce an empty weight table, which is indistinguishable
    // from a region that happens to contain no nodes. That is the failure shape this repository refuses
    // everywhere else: a plausible-looking result of the wrong kind rather than a refusal.
    if (mask.r1_m < mask.r0_m) return false;

    // **Dimensionless**, and that is a real dimension rather than a missing one: a weight is a pure number. The
    // descriptor is built by the same `make_lattice` every other bake uses, so a consumer that reads the
    // component count sees one and not three.
    const qp::abi::FieldDim none{};
    const qp::abi::LatticeDesc desc =
        qp::abi::make_lattice(qp::abi::LatticeKind::volume, qp::abi::ComponentKind::scalar,
                              qp::abi::ElementType::f64, none, grid.nx, grid.ny, grid.nz);
    if (desc.component != qp::abi::ComponentKind::scalar) return false;

    std::vector<double> weights(static_cast<std::size_t>(grid.point_count()), 0.0);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const Vec3 point = grid.node_position(i, j, k);
                const double radius = norm(point);
                bool inside = false;
                switch (mask.region) {
                    case FieldNodes::MaskRegion::sphere: inside = radius < mask.r0_m; break;
                    case FieldNodes::MaskRegion::shell:
                        inside = radius >= mask.r0_m && radius < mask.r1_m;
                        break;
                    // The terminator plane through the centre, and the simplification is stated rather than
                    // hidden: the reference implementation puts it ten earth radii downwind, because that is where
                    // a *magnetopause* stands. That standoff is a property of the boundary model, not of a mask,
                    // and when the kit grows one this becomes its parameter.
                    case FieldNodes::MaskRegion::dayside: inside = point.x > 0.0; break;
                    case FieldNodes::MaskRegion::nightside: inside = point.x <= 0.0; break;
                }
                weights[at++] = inside ? 1.0 : 0.0;
            }
        }
    }
    return fields.publish(key, desc, std::move(weights));
}

bool bake_scaled(const gfield::FieldValue& a, const gfield::FieldValue& w, gfield::FieldKey key,
                 gfield::FieldSet& fields) {
    if (!gfield::is_readable(a) || !gfield::is_readable(w)) return false;
    if (a.kind() != gfield::Kind::Volume || w.kind() != gfield::Kind::Volume) return false;
    if (!a.is_vector() || w.is_vector()) return false;
    if (a.desc.element != qp::abi::ElementType::f64 || w.desc.element != qp::abi::ElementType::f64) return false;
    for (int axis = 0; axis < 3; ++axis) {
        if (a.desc.count[axis] != w.desc.count[axis]) return false;
    }
    const auto* vectors = static_cast<const double*>(a.data);
    const auto* weights = static_cast<const double*>(w.data);
    if (vectors == nullptr || weights == nullptr) return false;

    // **Two index spaces, and that is the whole of this loop.** The weight is indexed by *point* and the output by
    // *component*, so the point is what the weight's index is derived from. Walking one index across both -- which
    // reads naturally and is wrong -- would take every third weight and multiply the rest by whatever the third
    // one happened to be, producing a field that is smooth, plausible and about a third as strong as it should be.
    const std::size_t points = static_cast<std::size_t>(a.point_count());
    std::vector<double> product(points * 3);
    for (std::size_t point = 0; point < points; ++point) {
        const double weight = weights[point];
        product[point * 3 + 0] = vectors[point * 3 + 0] * weight;
        product[point * 3 + 1] = vectors[point * 3 + 1] * weight;
        product[point * 3 + 2] = vectors[point * 3 + 2] * weight;
    }
    // `a`'s own descriptor, dimension included: scaling by a pure number does not change what the field is.
    return fields.publish(key, a.desc, std::move(product));
}

std::vector<graph::NodeDesc> FieldNodes::node_types() {    graph::NodeDesc dipole;
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

    graph::NodeDesc uniform;
    uniform.type_name = kUniformType;
    uniform.label = "Uniform field";
    uniform.description = "The same magnetic field everywhere, in tesla. Its answer is exact under trilinear "
                          "interpolation at any spacing, which is what makes it the field a course checks a "
                          "pusher against.";
    uniform.category = "field";
    uniform.version = 1;
    uniform.allow_in_field_domain = true;
    uniform.allow_in_particle_domain = false;
    uniform.has_compute = true;
    graph::PortDesc bx = parameter(kPortField0, "b_x", "B x", "T", 1.0e-6);
    bx.description = "The field's x component, in tesla. Defaults to a microtesla, which is a visible gyration "
                     "at a thermal speed and costs nothing to compute.";
    graph::PortDesc by = parameter(kPortField1, "b_y", "B y", "T", 0.0);
    graph::PortDesc bz = parameter(kPortField2, "b_z", "B z", "T", 0.0);
    uniform.inputs.push_back(bx);
    uniform.inputs.push_back(by);
    uniform.inputs.push_back(bz);
    uniform.inputs.push_back(parameter(kPortUniformOrigin0, "origin_x", "Grid origin x", "m", kEarthRadiusM));
    uniform.inputs.push_back(parameter(kPortUniformOrigin1, "origin_y", "Grid origin y", "m", kEarthRadiusM));
    uniform.inputs.push_back(parameter(kPortUniformOrigin2, "origin_z", "Grid origin z", "m", kEarthRadiusM));
    uniform.inputs.push_back(parameter(kPortUniformSpacing0, "spacing_x", "Grid spacing x", "m", 0.1 * kEarthRadiusM));
    uniform.inputs.push_back(parameter(kPortUniformSpacing1, "spacing_y", "Grid spacing y", "m", 0.1 * kEarthRadiusM));
    uniform.inputs.push_back(parameter(kPortUniformSpacing2, "spacing_z", "Grid spacing z", "m", 0.1 * kEarthRadiusM));
    uniform.inputs.push_back(parameter(kPortUniformCount0, "count_x", "Nodes along x", "", 1.0));
    uniform.inputs.push_back(parameter(kPortUniformCount1, "count_y", "Nodes along y", "", 1.0));
    uniform.inputs.push_back(parameter(kPortUniformCount2, "count_z", "Nodes along z", "", 1.0));
    graph::PortDesc uniform_out;
    uniform_out.number = kPortField;
    uniform_out.name = "field";
    uniform_out.label = "Magnetic field";
    uniform_out.description = "The baked field, as a volume of tesla vectors.";
    uniform_out.type = qp::ports::kVectorField;
    uniform_out.connectable = true;
    uniform_out.required = false;
    uniform_out.unit_symbol = "T";
    uniform.outputs.push_back(uniform_out);

    graph::NodeDesc sum;
    sum.type_name = kSumType;
    sum.label = "Add fields";
    sum.description = "Adds two magnetic fields node by node. The composition principle as a node: wire the "
                      "models you want and add them, rather than looking for a node that already contains the "
                      "combination.";
    sum.category = "field";
    sum.version = 1;
    sum.allow_in_field_domain = true;
    sum.allow_in_particle_domain = false;
    sum.has_compute = true;
    const auto socket = [](graph::PortNumber number, const char* name, const char* label) {
        graph::PortDesc port;
        port.number = number;
        port.name = name;
        port.label = label;
        port.description = "A field to add. Both addends must be baked onto the same lattice; a sum of two "
                           "different geometries is refused rather than fitted.";
        port.type = qp::ports::kVectorField;
        port.connectable = true;
        // Required, unlike a pusher's optional sockets: a sum with one addend is not a sum, and a node that
        // silently produced its single input would be a graph that looks composed and is not.
        port.required = true;
        port.unit_symbol = "T";
        return port;
    };
    sum.inputs.push_back(socket(kPortAddendA, "a", "Field A"));
    sum.inputs.push_back(socket(kPortAddendB, "b", "Field B"));
    sum.inputs.push_back(parameter(kPortOrigin0, "origin_x", "Grid origin x", "m", kEarthRadiusM));
    sum.inputs.push_back(parameter(kPortOrigin1, "origin_y", "Grid origin y", "m", kEarthRadiusM));
    sum.inputs.push_back(parameter(kPortOrigin2, "origin_z", "Grid origin z", "m", kEarthRadiusM));
    sum.inputs.push_back(parameter(kPortSpacing0, "spacing_x", "Grid spacing x", "m", 0.1 * kEarthRadiusM));
    sum.inputs.push_back(parameter(kPortSpacing1, "spacing_y", "Grid spacing y", "m", 0.1 * kEarthRadiusM));
    sum.inputs.push_back(parameter(kPortSpacing2, "spacing_z", "Grid spacing z", "m", 0.1 * kEarthRadiusM));
    sum.inputs.push_back(parameter(kPortCount0, "count_x", "Nodes along x", "", 1.0));
    sum.inputs.push_back(parameter(kPortCount1, "count_y", "Nodes along y", "", 1.0));
    sum.inputs.push_back(parameter(kPortCount2, "count_z", "Nodes along z", "", 1.0));
    graph::PortDesc sum_out;
    sum_out.number = kPortField;
    sum_out.name = "field";
    sum_out.label = "Magnetic field";
    sum_out.description = "The sum, as a volume of tesla vectors on the same lattice as the addends.";
    sum_out.type = qp::ports::kVectorField;
    sum_out.connectable = true;
    sum_out.required = false;
    sum_out.unit_symbol = "T";
    sum.outputs.push_back(sum_out);

    graph::NodeDesc electric;
    electric.type_name = kUniformElectricType;
    electric.label = "Uniform E field";
    electric.description = "The same electric field everywhere, in volts per metre. Wire it into a pusher's "
                           "electric socket: with a magnetic field it produces the E x B drift, which is what "
                           "gives a magnetosphere its convection pattern.";
    electric.category = "field";
    electric.version = 1;
    electric.allow_in_field_domain = true;
    electric.allow_in_particle_domain = false;
    electric.has_compute = true;
    graph::PortDesc ex = parameter(kPortE0, "e_x", "E x", "V/m", 1.0e-5);
    ex.description = "The field's x component. A tenth of a millivolt per metre is the order of the "
                     "cross-polar-cap field mapped to the equatorial plane, so the drift it produces is visible "
                     "over a run rather than over a thousand.";
    electric.inputs.push_back(ex);
    electric.inputs.push_back(parameter(kPortE1, "e_y", "E y", "V/m", 1.0e-5));
    electric.inputs.push_back(parameter(kPortE2, "e_z", "E z", "V/m", 1.0e-5));
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z",     "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",      "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",     "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x",   "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.1 * kEarthRadiusM,
                                 0.1 * kEarthRadiusM, 0.1 * kEarthRadiusM, 1.0, 1.0, 1.0};
        electric.inputs.push_back(parameter(kPortElectricOrigin0 + offset, names[offset], labels[offset],
                                            units[offset], steps[offset]));
    }
    graph::PortDesc e_out;
    e_out.number = kPortField;
    e_out.name = "field";
    e_out.label = "Electric field";
    e_out.description = "The baked field, as a volume of volt-per-metre vectors.";
    e_out.type = qp::ports::kVectorField;
    e_out.connectable = true;
    e_out.required = false;
    e_out.unit_symbol = "V/m";
    electric.outputs.push_back(e_out);

    // ---------------- the region mask ----------------
    //
    // **A different kind of product**, and the descriptor says so: this is the kit's first node whose output is a
    // scalar field. Two sockets in this tree want one -- the pusher's drag socket and the `mul` this enables -- and
    // until this type existed nothing could produce one, so a graph could declare a drag socket it had no way to
    // fill. The weight is dimensionless, and a consumer that assumed three components per node would read its own
    // samples out of alignment.
    graph::NodeDesc mask;
    mask.type_name = kMaskType;
    mask.label = "Region mask";
    mask.description = "A weight field that is 1 inside a named region and 0 outside it: a sphere, a shell, or the "
                       "sunward or anti-sunward half-space. Wire it into a multiplier to modulate a field, or "
                       "straight into a pusher's drag socket for a drag that acts only where the atmosphere is.";
    mask.category = "field";
    mask.version = 1;
    mask.allow_in_field_domain = true;
    mask.allow_in_particle_domain = false;
    mask.has_compute = true;
    graph::PortDesc region;
    region.number = kPortMaskRegion;
    region.name = "region";
    region.label = "Region";
    region.description = "Which region the weight covers: 0 sphere, 1 shell, 2 dayside, 3 nightside.";
    region.type = qp::ports::kInt64;
    region.connectable = false;
    region.required = true;
    region.step = 1.0;
    mask.inputs.push_back(region);
    graph::PortDesc inner = parameter(kPortMaskR0, "r0", "Inner radius", "m", kEarthRadiusM);
    inner.description = "The region's inner radius. Used by `sphere` (everything inside) and `shell`.";
    mask.inputs.push_back(inner);
    graph::PortDesc outer = parameter(kPortMaskR1, "r1", "Outer radius", "m", kEarthRadiusM);
    outer.description = "The region's outer radius, used by `shell`. The two are ordered on reading, so swapping "
                        "them describes the same shell rather than an empty one.";
    mask.inputs.push_back(outer);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z", "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",   "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",   "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x", "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.1 * kEarthRadiusM,
                                 0.1 * kEarthRadiusM, 0.1 * kEarthRadiusM, 1.0, 1.0, 1.0};
        // Its **own** grid ports, and after the three parameters rather than after the vector nodes' three
        // components: the port numbers are per type, and this type's second socket is a radius rather than a field
        // component. Reusing the uniform node's numbering here is what produced a registration collision once
        // already (`§9.14`), and the rule that came out of it is that every type owns its own numbers.
        mask.inputs.push_back(
            parameter(kPortMaskOrigin0 + offset, names[offset], labels[offset], units[offset], steps[offset]));
    }
    graph::PortDesc w_out;
    w_out.number = kPortWeight;
    w_out.name = "weight";
    w_out.label = "Weight";
    w_out.description = "The baked weight: 0 or 1 at every node, dimensionless.";
    w_out.type = qp::ports::kScalarField;
    w_out.connectable = true;
    w_out.required = false;
    w_out.unit_symbol = "1";
    mask.outputs.push_back(w_out);

    // ---------------- the multiplier ----------------
    graph::NodeDesc mul;
    mul.type_name = kMulType;
    mul.label = "Scale by weight";
    mul.description = "The vector field on the first socket, multiplied node by node by the scalar weight on the "
                      "second. Wire a region mask into the weight and a field into the first socket and the field "
                      "exists only inside that region -- which is how a shielding field is built here: by "
                      "wiring, not by a switch inside a node. No grid of its own: the product lives on the "
                      "lattice its inputs share.";
    mul.category = "field";
    mul.version = 1;
    mul.allow_in_field_domain = true;
    mul.allow_in_particle_domain = false;
    mul.has_compute = true;
    graph::PortDesc mul_field;
    mul_field.number = kPortMulField;
    mul_field.name = "field";
    mul_field.label = "Field";
    mul_field.description = "The vector field to scale. Wired from any field node's output.";
    mul_field.type = qp::ports::kVectorField;
    mul_field.connectable = true;
    mul_field.required = true;
    mul.inputs.push_back(mul_field);
    graph::PortDesc mul_weight;
    mul_weight.number = kPortMulWeight;
    mul_weight.name = "weight";
    mul_weight.label = "Weight";
    mul_weight.description = "The scalar weight, 0 or 1 per node from a region mask or anything else that "
                             "publishes a scalar field. The port type is what refuses a vector here rather than "
                             "the code inside: a dipole wired into this socket is a connection error, one layer "
                             "below anything that could silently read its first component.";
    mul_weight.type = qp::ports::kScalarField;
    mul_weight.connectable = true;
    mul_weight.required = true;
    mul.inputs.push_back(mul_weight);
    graph::PortDesc mul_out;
    mul_out.number = kPortMulOut;
    mul_out.name = "field";
    mul_out.label = "Field";
    mul_out.description = "The product, described exactly as the input field is.";
    mul_out.type = qp::ports::kVectorField;
    mul_out.connectable = true;
    mul_out.required = false;
    mul.outputs.push_back(mul_out);

    return {std::move(dipole),   std::move(uniform), std::move(sum),
            std::move(electric), std::move(mask),    std::move(mul)};
}

gfield::FieldValue DipoleEvaluator::input_field(const graph::NodeId id, const graph::PortNumber port) const noexcept {
    if (graph_ == nullptr) return gfield::FieldValue{};
    const graph::Edge* edge = graph_->incoming(graph::PortRef{id, port, graph::PortDirection::input});
    if (edge == nullptr) return gfield::FieldValue{};
    return fields_->view(gfield::FieldKey{edge->from.node.index, edge->from.port});
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
    if (desc.type_name == FieldNodes::kMulType) {
        // No grid to read: the product lives on the lattice its inputs share, and a multiplier that asked for one
        // would be offering a way to describe a lattice the data does not have.
        const gfield::FieldKey mul_key{id.index, FieldNodes::kPortMulOut};
        const gfield::FieldValue field = input_field(id, FieldNodes::kPortMulField);
        const gfield::FieldValue weight = input_field(id, FieldNodes::kPortMulWeight);
        if (!bake_scaled(field, weight, mul_key, *fields_)) {
            // Either socket missing, a vector where the weight belongs, or two lattices that disagree: all three
            // are refused rather than approximated, and the refusal is an error code because the vocabulary for
            // "which socket" is the validator's, one layer up.
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortMulOut, qp::ports::Value{fields_->view(mul_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kMaskType) {
        // The mask reads its grid from a **different** port offset than the vector nodes -- after three parameters
        // rather than after three components -- which is why the reader takes the offset rather than assuming it.
        const graph::InputView mask_view{inputs};
        const FieldNodes::MaskSpec mask = FieldNodes::read_mask_from(mask_view);
        const GridSpec mask_grid = FieldNodes::read_from(mask_view, FieldNodes::kPortMaskOrigin0);
        const gfield::FieldKey mask_key{id.index, FieldNodes::kPortWeight};
        if (!bake_mask(mask, mask_grid, mask_key, *fields_)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        // The handle names a **scalar** lattice, and the port it comes out of is the weight port: the pair is what
        // a consumer looks the samples up by, so a mask that published under the vector port number would hand a
        // reader three-component samples it does not have.
        out.emplace_back(FieldNodes::kPortWeight, qp::ports::Value{fields_->view(mask_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kUniformElectricType) {
        const graph::InputView electric_view{inputs};
        const Vec3 value{real_or(electric_view, FieldNodes::kPortE0, 0.0),
                         real_or(electric_view, FieldNodes::kPortE1, 0.0),
                         real_or(electric_view, FieldNodes::kPortE2, 0.0)};
        const GridSpec electric_grid = FieldNodes::read_from(electric_view, FieldNodes::kPortElectricOrigin0);
        const gfield::FieldKey electric_key{id.index, FieldNodes::kPortField};
        // **Volts per metre**, and the dimension is the only difference from the magnetic bake: the pusher reads
        // both sockets through the same vocabulary, so a description that mixed them up would make an electric
        // field appear as a magnetic one with nothing downstream able to tell.
        if (!bake_uniform(value, electric_grid, electric_key, *fields_, volt_per_metre_dimension())) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortField, qp::ports::Value{fields_->view(electric_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kSumType) {
        const graph::InputView sum_view{inputs};
        const GridSpec sum_grid = FieldNodes::read_from(sum_view);
        const gfield::FieldKey sum_key{id.index, FieldNodes::kPortField};
        const gfield::FieldValue a = input_field(id, FieldNodes::kPortAddendA);
        const gfield::FieldValue b = input_field(id, FieldNodes::kPortAddendB);
        if (!bake_sum(a, b, sum_key, *fields_)) {
            // A sum whose addends are missing or whose lattices disagree is refused rather than approximated,
            // and the refusal travels as an error code because the run's own refusal vocabulary is one layer up.
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        (void)sum_grid;   // the addends' own lattice is the sum's: the grid ports are what a plan builder reads
        Outcome out;
        out.emplace_back(FieldNodes::kPortField, qp::ports::Value{fields_->view(sum_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kUniformType) {
        const graph::InputView uniform_view{inputs};
        const Vec3 value{real_or(uniform_view, FieldNodes::kPortField0, 0.0),
                         real_or(uniform_view, FieldNodes::kPortField1, 0.0),
                         real_or(uniform_view, FieldNodes::kPortField2, 0.0)};
        const GridSpec uniform_grid = FieldNodes::read_from(uniform_view, FieldNodes::kPortUniformOrigin0);
        const gfield::FieldKey uniform_key{id.index, FieldNodes::kPortField};
        if (!bake_uniform(value, uniform_grid, uniform_key, *fields_, tesla_dimension())) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortField, qp::ports::Value{fields_->view(uniform_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
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
