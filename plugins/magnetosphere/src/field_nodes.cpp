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
#include <qp/plugins/magnetosphere/source_nodes.hpp>

#include <algorithm>
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

/// @brief Whether two dimensions say the same thing, field by field.
///
/// `abi::FieldDim` carries no operators, and that is the same division of labour that keeps `LatticeDesc` free of
/// positions: the ABI answers "what is this value", and "these two are the same quantity" is a model question one
/// layer up. Written once here rather than remembered differently by each caller that has two fields in hand.
[[nodiscard]] bool same_dimension(const qp::abi::FieldDim& a, const qp::abi::FieldDim& b) noexcept {
    return a.L == b.L && a.M == b.M && a.T == b.T && a.I == b.I && a.Th == b.Th && a.N == b.N && a.J == b.J;
}

/// @brief The port a field type's grid starts at, or false for a type that has no grid of its own.
///
/// One table, and it is deliberately here rather than in each caller: these are the same numbers each type's
/// descriptor publishes, and a caller that guessed an offset would read `count_z` where `origin_x` belongs --
/// producing a grid that is internally consistent and in the wrong place, which is the failure `plan.hpp` warns
/// about at length.
[[nodiscard]] bool grid_origin_port(const std::string& type_name, graph::PortNumber& out) noexcept {
    if (type_name == FieldNodes::kDipoleType || type_name == FieldNodes::kSumType) {
        out = FieldNodes::kPortOrigin0;   // the dipole's numbering; the sum shares it
        return true;
    }
    if (type_name == FieldNodes::kUniformType) {
        out = FieldNodes::kPortUniformOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kUniformElectricType) {
        out = FieldNodes::kPortElectricOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kMaskType) {
        out = FieldNodes::kPortMaskOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kConvectionType) {
        out = FieldNodes::kPortConvectionOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kAtmosphereType) {
        out = FieldNodes::kPortAtmosphereOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kCurrentSheetType) {
        out = FieldNodes::kPortSheetOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kResampleType) {
        // The resampler **is** a grid source: its own nine ports describe where its samples are, which is the
        // target lattice. It is the one type whose whole job is to move samples, so a caller that reaches it has
        // reached the answer rather than a node to walk past.
        out = FieldNodes::kPortResampleOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kMagnetopauseType) {
        out = FieldNodes::kPortMagnetopauseOrigin0;
        return true;
    }
    if (type_name == FieldNodes::kShieldType) {
        out = FieldNodes::kPortShieldOrigin0;
        return true;
    }
    // `field.mul` and `field.blend` are the types that declare no grid at all: a product and a blend are both
    // defined on the lattice their inputs share, so they forward. A caller reaching here with one of them has
    // failed to follow the wire, which is what the walk below exists to do.
    return false;
}

}  // namespace

bool resolve_field_origin(const graph::Graph& graph, graph::NodeId consumer, graph::PortNumber socket,
                          GridSpec& out) noexcept {
    graph::NodeId current = consumer;
    graph::PortNumber port = socket;
    // Bounded by the node count rather than by a constant: a graph cannot have a longer acyclic path than it has
    // nodes, and `graph/structure` refuses cycles at connect time. The bound is here so that this function is
    // total even if that refusal is ever loosened -- a hung editor is worse than a refusal.
    for (std::size_t hop = 0; hop <= graph.slots().size(); ++hop) {
        const graph::Edge* edge = graph.incoming(graph::PortRef{current, port, graph::PortDirection::input});
        if (edge == nullptr) return false;   // nothing wired
        const graph::NodeId source_id = edge->from.node;
        const graph::Node* source = graph.find_node(source_id);
        if (source == nullptr) return false;
        graph::PortNumber origin = 0;
        if (grid_origin_port(source->type_name, origin)) {
            out = FieldNodes::read_from(*source, origin);
            return true;
        }
        // A node with no grid of its own: follow what feeds *it*. `mul` reads the field on its first socket and
        // the weight on its second, and it is the first that carries the lattice.
        if (source->type_name == FieldNodes::kMulType) {
            current = source_id;
            port = FieldNodes::kPortMulField;
            continue;
        }
        // The blend has no grid either, and its first socket is the one that carries it: both inputs must be on
        // the same lattice for a blend to be built at all, so either would do and the first is the one a reader
        // is looking at when they ask. A second type added here rather than a rule invented for it -- the rule is
        // the one in the paragraph above: a node either declares a grid or forwards to whoever fed it.
        if (source->type_name == FieldNodes::kBlendType) {
            current = source_id;
            port = FieldNodes::kPortBlendInner;
            continue;
        }
        // The mix has no grid either, and it forwards on its **first** socket -- the field that carries the lattice
        // and the dimension, which is the one a reader is looking at when they ask.
        if (source->type_name == FieldNodes::kMixType) {
            current = source_id;
            port = FieldNodes::kPortMixA;
            continue;
        }
        return false;   // a type this build does not know how to ask
    }
    return false;
}

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

bool bake_convection(double amplitude_v_per_m2, const GridSpec& grid, gfield::FieldKey key,
                     gfield::FieldSet& fields) {
    if (!std::isfinite(amplitude_v_per_m2)) return false;
    if (!bakeable(grid)) return false;

    // One pass, no table of potentials: see the declaration for why the field is evaluated rather than differenced.
    BakedField table{grid.origin_m, grid.spacing_m, grid.nx, grid.ny, grid.nz};
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const Vec3 point = grid.node_position(i, j, k);
                table.set_node(i, j, k,
                               Vec3{-2.0 * amplitude_v_per_m2 * point.y, -2.0 * amplitude_v_per_m2 * point.x, 0.0});
            }
        }
    }
    const qp::abi::LatticeDesc desc = table.view(volt_per_metre_dimension()).desc;
    return fields.publish(key, desc, std::move(table.data()));
}

bool bake_shield(double r0_m, const GridSpec& grid, gfield::FieldKey key, gfield::FieldSet& fields) {
    if (!bakeable(grid)) return false;
    // Refused rather than clamped, for the reason the atmosphere node's scale height is: a zero or negative
    // shielding radius is not a smaller shield, it is an inverted or absent one, and clamping would hide a sign
    // error behind a picture that looks plausible.
    if (!std::isfinite(r0_m) || !(r0_m > 0.0)) return false;

    const qp::abi::LatticeDesc desc = qp::abi::make_lattice(
        qp::abi::LatticeKind::volume, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f64,
        qp::abi::kDimensionless, grid.nx, grid.ny, grid.nz);

    std::vector<double> coefficients(static_cast<std::size_t>(grid.point_count()), 1.0);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const double r = norm(grid.node_position(i, j, k));
                const double ratio = r / r0_m;
                // `min(1, ratio^2)` and **not** a branch with a floor in it: the formula is already zero at the
                // origin and already one outside, so the only thing a floor could add is a number nobody asked for.
                coefficients[at++] = ratio < 1.0 ? ratio * ratio : 1.0;
            }
        }
    }
    return fields.publish(key, desc, std::move(coefficients));
}

bool bake_corotation(const gfield::FieldValue& magnetic, const Vec3& origin_m, const Vec3& spacing_m,
                     const GridSpec& grid, gfield::FieldKey key, gfield::FieldSet& fields) {
    if (!gfield::is_readable(magnetic)) return false;
    if (magnetic.kind() != gfield::Kind::Volume || !magnetic.is_vector()) return false;
    if (magnetic.desc.element != qp::abi::ElementType::f64) return false;
    if (!bakeable(grid)) return false;

    BakedField table{grid.origin_m, grid.spacing_m, grid.nx, grid.ny, grid.nz};
    const Vec3 omega{0.0, 0.0, kEarthRotationRateSI};
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const Vec3 point = grid.node_position(i, j, k);
                const Vec3 b = sample_baked(magnetic, origin_m, spacing_m, point);
                // `E = -(Omega x r) x B`, written through the BAC-CAB identity as `B x (Omega x r)`, because the
                // expanded form is the one that can be checked by hand: `B x (Omega x r) = Omega (B . r) -
                // r (B . Omega)`. In the equatorial plane with the dipole's southward field that is `Omega B r`,
                // **radially outward** -- the field a plasma moving with the planet sees, whose `E x B` is the
                // rigid rotation. A zero field gives a zero electric field, which is the model's own answer
                // rather than a special case: no field to corotate in, no corotation.
                table.set_node(i, j, k, cross(b, cross(omega, point)));
            }
        }
    }
    const qp::abi::LatticeDesc desc = table.view(volt_per_metre_dimension()).desc;
    return fields.publish(key, desc, std::move(table.data()));
}

FieldNodes::AtmosphereSpec FieldNodes::read_atmosphere_from(const graph::InputView& inputs) noexcept {
    AtmosphereSpec spec;
    spec.nu0_per_s = real_or(inputs, kPortAtmosphereNu0, kDefaultAtmosphereNu0);
    spec.scale_height_m = real_or(inputs, kPortAtmosphereScaleHeight, kDefaultAtmosphereScaleHeightM);
    spec.reference_m = real_or(inputs, kPortAtmosphereReference, kDefaultAtmosphereReferenceM);
    return spec;
}

FieldNodes::AtmosphereSpec FieldNodes::read_atmosphere(const graph::Node& node) noexcept {
    graph::PortValues values;
    values.reserve(node.params.size());
    for (const graph::ParamValue& p : node.params) values.emplace_back(p.number, p.value);
    return read_atmosphere_from(graph::InputView{values});
}

bool bake_atmosphere(const FieldNodes::AtmosphereSpec& spec, const GridSpec& grid, gfield::FieldKey key,
                     gfield::FieldSet& fields) {
    if (!bakeable(grid)) return false;
    // Refused, not clamped: see the declaration. A rate below zero is a drag that adds energy and a scale height of
    // zero is an atmosphere that is a wall, and neither is a configuration this model describes.
    if (!std::isfinite(spec.nu0_per_s) || spec.nu0_per_s < 0.0) return false;
    if (!std::isfinite(spec.scale_height_m) || !(spec.scale_height_m > 0.0)) return false;
    if (!std::isfinite(spec.reference_m)) return false;

    // Hertz: a rate is an inverse time, which is a real `FieldDim` and the unit the drag socket documents.
    const qp::abi::LatticeDesc desc =
        qp::abi::make_lattice(qp::abi::LatticeKind::volume, qp::abi::ComponentKind::scalar,
                              qp::abi::ElementType::f64, per_second_dimension(), grid.nx, grid.ny, grid.nz);

    std::vector<double> rates(static_cast<std::size_t>(grid.point_count()), 0.0);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const double radius = norm(grid.node_position(i, j, k));
                rates[at++] = spec.nu0_per_s * std::exp(-(radius - spec.reference_m) / spec.scale_height_m);
            }
        }
    }
    return fields.publish(key, desc, std::move(rates));
}

FieldNodes::SheetSpec FieldNodes::read_sheet_from(const graph::InputView& inputs) noexcept {
    SheetSpec spec;
    spec.b0_tesla = real_or(inputs, kPortSheetB0, kDefaultSheetB0);
    spec.half_thickness_m = real_or(inputs, kPortSheetThickness, kDefaultSheetThicknessM);
    return spec;
}

FieldNodes::SheetSpec FieldNodes::read_sheet(const graph::Node& node) noexcept {
    graph::PortValues values;
    values.reserve(node.params.size());
    for (const graph::ParamValue& p : node.params) values.emplace_back(p.number, p.value);
    return read_sheet_from(graph::InputView{values});
}

FieldNodes::BlendSpec FieldNodes::read_blend_from(const graph::InputView& inputs) noexcept {
    BlendSpec spec;
    spec.transition_m = real_or(inputs, kPortBlendTransition, kDefaultBlendTransitionM);
    spec.width_m = real_or(inputs, kPortBlendWidth, kDefaultBlendWidthM);
    spec.correction = real_or(inputs, kPortBlendCorrection, kDefaultBlendCorrection);
    return spec;
}

FieldNodes::BlendSpec FieldNodes::read_blend(const graph::Node& node) noexcept {
    graph::PortValues values;
    values.reserve(node.params.size());
    for (const graph::ParamValue& p : node.params) values.emplace_back(p.number, p.value);
    return read_blend_from(graph::InputView{values});
}

FieldNodes::MagnetopauseSpec FieldNodes::read_magnetopause_from(const graph::InputView& inputs) noexcept {
    MagnetopauseSpec spec;
    spec.standoff_m = real_or(inputs, kPortMagnetopauseStandoff,
                              kDefaultMagnetopauseStandoffRe * kEarthRadiusM);
    spec.flaring = real_or(inputs, kPortMagnetopauseFlaring, kDefaultMagnetopauseFlaring);
    spec.width_m = real_or(inputs, kPortMagnetopauseWidth, kDefaultMagnetopauseWidthM);
    return spec;
}

FieldNodes::MagnetopauseSpec FieldNodes::read_magnetopause(const graph::Node& node) noexcept {
    graph::PortValues values;
    values.reserve(node.params.size());
    for (const graph::ParamValue& p : node.params) values.emplace_back(p.number, p.value);
    return read_magnetopause_from(graph::InputView{values});
}

bool bake_current_sheet(const FieldNodes::SheetSpec& spec, const GridSpec& grid, gfield::FieldKey key,
                        gfield::FieldSet& fields) {
    if (!bakeable(grid)) return false;
    if (!std::isfinite(spec.b0_tesla)) return false;
    if (!std::isfinite(spec.half_thickness_m) || !(spec.half_thickness_m > 0.0)) return false;

    BakedField table{grid.origin_m, grid.spacing_m, grid.nx, grid.ny, grid.nz};
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const double z = grid.node_position(i, j, k).z;
                // **Exact zeros in the two other components**, not expressions that ought to vanish: a sheet is
                // one-dimensional, and a `B_y` that came out as 1e-30 would be a number in the table that is not
                // the model. The case asserts the zeros at every node, which is what keeps this honest.
                table.set_node(i, j, k, Vec3{spec.b0_tesla * std::tanh(z / spec.half_thickness_m), 0.0, 0.0});
            }
        }
    }
    const qp::abi::LatticeDesc desc = table.view(tesla_dimension()).desc;
    return fields.publish(key, desc, std::move(table.data()));
}

bool bake_blend(const gfield::FieldValue& inner, const gfield::FieldValue& outer, const GridSpec& grid,
                const FieldNodes::BlendSpec& spec, gfield::FieldKey key, gfield::FieldSet& fields) {
    if (!gfield::is_readable(inner) || !gfield::is_readable(outer)) return false;
    if (inner.kind() != gfield::Kind::Volume || outer.kind() != gfield::Kind::Volume) return false;
    if (!inner.is_vector() || !outer.is_vector()) return false;
    if (inner.desc.element != qp::abi::ElementType::f64 || outer.desc.element != qp::abi::ElementType::f64) {
        return false;
    }
    // Two answers to "where are the samples" is one answer too many: the tables must agree with each other *and*
    // with the geometry the caller resolved, which is the only place positions exist at all.
    if (inner.desc.count[0] != outer.desc.count[0] || inner.desc.count[1] != outer.desc.count[1] ||
        inner.desc.count[2] != outer.desc.count[2]) {
        return false;
    }
    if (inner.desc.count[0] != grid.nx || inner.desc.count[1] != grid.ny || inner.desc.count[2] != grid.nz) {
        return false;
    }
    // A blend of tesla with volts per metre is refused here rather than caught by the port types, which cannot
    // see the difference: both are vector fields and both are legal on these sockets.
    if (!same_dimension(inner.desc.dimension, outer.desc.dimension)) return false;
    if (!bakeable(grid)) return false;
    if (!std::isfinite(spec.transition_m) || !std::isfinite(spec.width_m) || !std::isfinite(spec.correction)) {
        return false;
    }
    // Refused rather than clamped, for the reason the atmosphere node's rate is: a zero width makes the weight a
    // step and the correction a spike no table can carry, and a correction outside `[0, 1]` would be a blend that
    // adds divergence instead of removing it.
    if (!(spec.width_m > 0.0)) return false;
    if (spec.correction < 0.0 || spec.correction > 1.0) return false;

    const auto* a = static_cast<const double*>(inner.data);
    const auto* c = static_cast<const double*>(outer.data);
    if (a == nullptr || c == nullptr) return false;

    const std::uint32_t nx = grid.nx;
    const std::uint32_t ny = grid.ny;
    const std::uint32_t nz = grid.nz;
    const double dz = grid.spacing_m.z;
    BakedField table{grid.origin_m, grid.spacing_m, nx, ny, nz};

    for (std::uint32_t i = 0; i < nx; ++i) {
        const double x = grid.origin_m.x + static_cast<double>(i) * grid.spacing_m.x;
        const double weight = 1.0 / (1.0 + std::exp((x - spec.transition_m) / spec.width_m));
        const double keep_inner = 1.0 - weight;
        // The weight's derivative, taken from the weight: see the declaration for why it is not differenced.
        const double correction = -weight * keep_inner / spec.width_m * spec.correction;
        for (std::uint32_t j = 0; j < ny; ++j) {
            // `psi_outer - psi_inner` for this column of nodes, integrated upward in `z`. **The two fields are
            // differenced inside the recurrence**, so the anchor's constant -- a function of `x` and `y` that
            // neither table determines -- cancels before it can be chosen twice, and the only thing this loop
            // carries is the difference the correction actually needs.
            double dpsi = 0.0;
            const std::size_t column = (static_cast<std::size_t>(i) * ny + j) * nz;
            for (std::uint32_t k = 0; k < nz; ++k) {
                const std::size_t point = column + k;
                if (k > 0) {
                    const std::size_t before = point - 1;
                    const double now = c[point * 3 + 0] - a[point * 3 + 0];
                    const double earlier = c[before * 3 + 0] - a[before * 3 + 0];
                    dpsi -= 0.5 * (now + earlier) * dz;
                }
                table.set_node(i, j, k,
                               Vec3{keep_inner * a[point * 3 + 0] + weight * c[point * 3 + 0],
                                    keep_inner * a[point * 3 + 1] + weight * c[point * 3 + 1],
                                    keep_inner * a[point * 3 + 2] + weight * c[point * 3 + 2] + correction * dpsi});
            }
        }
    }
    // The lattice is the caller's, the dimension is the inner field's: the blend of two tesla tables is a tesla
    // table, and describing it as anything else would be a second answer to what this field is.
    const qp::abi::LatticeDesc desc = table.view(inner.desc.dimension).desc;
    return fields.publish(key, desc, std::move(table.data()));
}

bool bake_resample(const gfield::FieldValue& source, const GridSpec& source_grid, const GridSpec& target_grid,
                   gfield::FieldKey key, gfield::FieldSet& fields) {
    if (!gfield::is_readable(source)) return false;
    if (source.kind() != gfield::Kind::Volume || !source.is_vector()) return false;
    if (source.desc.element != qp::abi::ElementType::f64) return false;
    if (source.desc.count[0] != source_grid.nx || source.desc.count[1] != source_grid.ny ||
        source.desc.count[2] != source_grid.nz) {
        return false;
    }
    if (!bakeable(target_grid)) return false;

    // **Refused rather than clamped**, and exactly rather than with a tolerance: see the declaration. The far edge
    // is computed the way a caller computes it -- `origin + (count - 1) * spacing` -- so a target that *is* the
    // source, described by the same numbers, compares equal and is accepted.
    const auto far_edge = [](double origin, double spacing, std::uint32_t count) {
        return origin + static_cast<double>(count - 1) * spacing;
    };
    if (target_grid.origin_m.x < source_grid.origin_m.x || target_grid.origin_m.y < source_grid.origin_m.y ||
        target_grid.origin_m.z < source_grid.origin_m.z) {
        return false;
    }
    if (far_edge(target_grid.origin_m.x, target_grid.spacing_m.x, target_grid.nx) >
            far_edge(source_grid.origin_m.x, source_grid.spacing_m.x, source_grid.nx) ||
        far_edge(target_grid.origin_m.y, target_grid.spacing_m.y, target_grid.ny) >
            far_edge(source_grid.origin_m.y, source_grid.spacing_m.y, source_grid.ny) ||
        far_edge(target_grid.origin_m.z, target_grid.spacing_m.z, target_grid.nz) >
            far_edge(source_grid.origin_m.z, source_grid.spacing_m.z, source_grid.nz)) {
        return false;
    }

    BakedField table{target_grid.origin_m, target_grid.spacing_m, target_grid.nx, target_grid.ny, target_grid.nz};
    for (std::uint32_t i = 0; i < target_grid.nx; ++i) {
        for (std::uint32_t j = 0; j < target_grid.ny; ++j) {
            for (std::uint32_t k = 0; k < target_grid.nz; ++k) {
                const Vec3 point = target_grid.node_position(i, j, k);
                table.set_node(i, j, k,
                               sample_baked(source, source_grid.origin_m, source_grid.spacing_m, point));
            }
        }
    }
    // The source's dimension on the target's lattice: a resample moves samples, it does not change what they are.
    const qp::abi::LatticeDesc desc = table.view(source.desc.dimension).desc;
    return fields.publish(key, desc, std::move(table.data()));
}

bool bake_magnetopause(const FieldNodes::MagnetopauseSpec& spec, const GridSpec& grid, gfield::FieldKey key,
                       gfield::FieldSet& fields) {
    if (!bakeable(grid)) return false;
    if (!std::isfinite(spec.standoff_m) || !(spec.standoff_m > 0.0)) return false;
    if (!std::isfinite(spec.flaring) || spec.flaring < 0.0) return false;
    if (!std::isfinite(spec.width_m) || !(spec.width_m > 0.0)) return false;

    // A scalar table, not a vector one: this node publishes a **weight**, and the distinction is the one
    // `field.mask`'s case is built around -- a consumer that read three components out of a one-component table
    // would take other nodes' values rather than nothing.
    const qp::abi::LatticeDesc desc = qp::abi::make_lattice(
        qp::abi::LatticeKind::volume, qp::abi::ComponentKind::scalar, qp::abi::ElementType::f64,
        qp::abi::kDimensionless, grid.nx, grid.ny, grid.nz);

    std::vector<double> weights(static_cast<std::size_t>(grid.point_count()), 0.0);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const Vec3 point = grid.node_position(i, j, k);
                const double r = norm(point);
                // The angle is undefined at the origin and the surface is not describable at the antipode; both
                // clamps are the formula's own, and both leave the weight at one. See the declaration -- and note
                // that the clamp is on the **antipode side only**: flooring `cos theta` at both ends moves the nose
                // by `(2 / 1.9999)^alpha`, which is five parts in a hundred thousand of the standoff distance and
                // exactly the kind of error an exactness assertion is for.
                const double cosine =
                    r > 0.0 ? std::max(FieldNodes::kMagnetopauseMinCosine, point.x / r) : 0.0;
                const double surface = spec.standoff_m * std::pow(2.0 / (1.0 + cosine), spec.flaring);
                // One **inside**, zero outside: the orientation `field.mask` uses, so a wire reads the same way --
                // "this weight is one where the field it multiplies exists".
                weights[at++] = 1.0 / (1.0 + std::exp((r - surface) / spec.width_m));
            }
        }
    }
    return fields.publish(key, desc, std::move(weights));
}

bool bake_mix(const gfield::FieldValue& a, const gfield::FieldValue& b, const gfield::FieldValue& weight,
              const GridSpec& grid, double correction, gfield::FieldKey key, gfield::FieldSet& fields) {
    if (!gfield::is_readable(a) || !gfield::is_readable(b) || !gfield::is_readable(weight)) return false;
    if (a.kind() != gfield::Kind::Volume || b.kind() != gfield::Kind::Volume ||
        weight.kind() != gfield::Kind::Volume) {
        return false;
    }
    if (!a.is_vector() || !b.is_vector() || weight.is_vector()) return false;
    if (a.desc.element != qp::abi::ElementType::f64 || b.desc.element != qp::abi::ElementType::f64 ||
        weight.desc.element != qp::abi::ElementType::f64) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (a.desc.count[axis] != b.desc.count[axis] || a.desc.count[axis] != weight.desc.count[axis]) {
            return false;
        }
    }
    if (a.desc.count[0] != grid.nx || a.desc.count[1] != grid.ny || a.desc.count[2] != grid.nz) return false;
    if (!same_dimension(a.desc.dimension, b.desc.dimension)) return false;
    if (!bakeable(grid)) return false;
    if (!std::isfinite(correction) || correction < 0.0 || correction > 1.0) return false;

    const auto* first = static_cast<const double*>(a.data);
    const auto* second = static_cast<const double*>(b.data);
    const auto* weights = static_cast<const double*>(weight.data);
    if (first == nullptr || second == nullptr || weights == nullptr) return false;

    const std::uint32_t nx = grid.nx;
    const std::uint32_t ny = grid.ny;
    const std::uint32_t nz = grid.nz;
    // The weight's gradient, differenced on the table: central in the interior and one-sided on the six faces, where
    // there is only one side to look at. See the declaration for what that costs and where it is exact.
    const auto weight_at = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        return weights[(static_cast<std::size_t>(i) * ny + j) * nz + k];
    };
    const auto gradient = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        const std::uint32_t i0 = i > 0 ? i - 1 : i;
        const std::uint32_t i1 = i + 1 < nx ? i + 1 : i;
        const std::uint32_t j0 = j > 0 ? j - 1 : j;
        const std::uint32_t j1 = j + 1 < ny ? j + 1 : j;
        const std::uint32_t k0 = k > 0 ? k - 1 : k;
        const std::uint32_t k1 = k + 1 < nz ? k + 1 : k;
        const double dx = (static_cast<double>(i1) - static_cast<double>(i0)) * grid.spacing_m.x;
        const double dy = (static_cast<double>(j1) - static_cast<double>(j0)) * grid.spacing_m.y;
        const double dz = (static_cast<double>(k1) - static_cast<double>(k0)) * grid.spacing_m.z;
        return Vec3{(weight_at(i1, j, k) - weight_at(i0, j, k)) / dx,
                    (weight_at(i, j1, k) - weight_at(i, j0, k)) / dy,
                    (weight_at(i, j, k1) - weight_at(i, j, k0)) / dz};
    };

    BakedField table{grid.origin_m, grid.spacing_m, nx, ny, nz};

    // **The flux function needs its gauge fixed, and the other blend is why it did not notice.** Integrating
    // `-dB_x` in `z` recovers `psi` only up to a function of `x`; through `d psi / d z = -dB_x` the divergence is
    // cancelled whatever that function is, which is what makes `field.blend` exact with a one-dimensional weight --
    // there the correction has a single component and its `x` derivative never enters. With a weight that varies in
    // `x` **and** `z` the correction has an `x` component too, and its derivative does enter: the residual is
    // `d w / d z * dB_z(z_min)`, the gauge showing up as a source layer of its own. Adding the anchor plane's
    // integral, `G(x) = integral of dB_z in x along the bottom layer`, makes `d psi / d x = dB_z` as well, which is
    // what a flux function satisfies. The measurement that found this: without the gauge term the poloidal pair the
    // case builds came out at **0.55** of the uncorrected divergence -- the correction doubling the very layer it
    // was meant to remove.
    std::vector<double> gauge(static_cast<std::size_t>(nx) * ny, 0.0);
    for (std::uint32_t i = 1; i < nx; ++i) {
        for (std::uint32_t j = 0; j < ny; ++j) {
            const std::size_t here = (static_cast<std::size_t>(i) * ny + j) * nz;
            const std::size_t before = (static_cast<std::size_t>(i - 1) * ny + j) * nz;
            const double now = second[here * 3 + 2] - first[here * 3 + 2];
            const double earlier = second[before * 3 + 2] - first[before * 3 + 2];
            gauge[static_cast<std::size_t>(i) * ny + j] =
                gauge[static_cast<std::size_t>(i - 1) * ny + j] + 0.5 * (now + earlier) * grid.spacing_m.x;
        }
    }

    for (std::uint32_t i = 0; i < nx; ++i) {
        for (std::uint32_t j = 0; j < ny; ++j) {
            // `psi_b - psi_a` for this column, integrated upward in `z` with the two fields differenced **inside**
            // the recurrence -- the same anchor rule the other blend uses, and for the same reason: the constant a
            // table cannot determine cancels in the difference instead of being chosen twice. The gauge term is a
            // function of `x` and `y` alone, so it is the same at every node of the column.
            double dpsi = gauge[static_cast<std::size_t>(i) * ny + j];
            for (std::uint32_t k = 0; k < nz; ++k) {
                const std::size_t point = (static_cast<std::size_t>(i) * ny + j) * nz + k;
                const double w = weights[point];
                const Vec3 left{first[point * 3 + 0], first[point * 3 + 1], first[point * 3 + 2]};
                const Vec3 right{second[point * 3 + 0], second[point * 3 + 1], second[point * 3 + 2]};
                if (k > 0) {
                    const std::size_t before = point - 1;
                    const double now = right.x - left.x;
                    const double earlier = second[before * 3 + 0] - first[before * 3 + 0];
                    dpsi -= 0.5 * (now + earlier) * grid.spacing_m.z;
                }
                // The potential is the one a **poloidal** pair of fields has: `A = psi y_hat`. `grad w x (0, psi, 0)`
                // is then the term the product rule contributes, and with the gauge fixed above it is exact wherever
                // the inputs have no `y` structure. See the declaration for why this construction rather than the
                // reference's `(B x r) / 2`.
                const Vec3 potential{0.0, dpsi, 0.0};
                const Vec3 term = cross(gradient(i, j, k), potential) * correction;
                table.set_node(i, j, k,
                               Vec3{left.x * (1.0 - w) + right.x * w + term.x,
                                    left.y * (1.0 - w) + right.y * w + term.y,
                                    left.z * (1.0 - w) + right.z * w + term.z});
            }
        }
    }
    const qp::abi::LatticeDesc desc = table.view(a.desc.dimension).desc;
    return fields.publish(key, desc, std::move(table.data()));
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
    // The optional tilt driver, after the grid ports: a date on a wire decides the lean.
    graph::PortDesc tilt_driver;
    tilt_driver.number = kPortTiltDriver;
    tilt_driver.name = "tilt_from";
    tilt_driver.label = "Tilt (optional)";
    tilt_driver.description = "Wire a `source.day` here and the dipole leans by the tilt that day implies, in "
                              "degrees. Left empty, the parameter above is the lean -- so a graph written before "
                              "this socket existed keeps its field.";
    tilt_driver.type = qp::ports::kScalarF64;
    tilt_driver.connectable = true;
    tilt_driver.required = false;
    tilt_driver.unit_symbol = "deg";
    dipole.inputs.push_back(tilt_driver);

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

    // ---------------- the blend ----------------
    graph::NodeDesc blend;
    blend.type_name = kBlendType;
    blend.label = "Blend fields";
    blend.description = "Mixes the two fields on its sockets along x while keeping the result divergence-free. "
                        "Wire a dipole or an inner model into the sunward socket and the tail's current sheet "
                        "into the other, and the transition is a surface rather than a seam of monopoles: the "
                        "correction is the term the product rule contributes, and without it the blended field "
                        "has field lines that end in mid-air. No grid of its own: both inputs must already "
                        "share one.";
    blend.category = "field";
    blend.version = 1;
    blend.allow_in_field_domain = true;
    blend.allow_in_particle_domain = false;
    blend.has_compute = true;
    graph::PortDesc blend_inner;
    blend_inner.number = kPortBlendInner;
    blend_inner.name = "inner";
    blend_inner.label = "Inner field";
    blend_inner.description = "The field that keeps its meaning sunward of the transition (+x). It carries the "
                              "lattice and the dimension the blend is built and described in.";
    blend_inner.type = qp::ports::kVectorField;
    blend_inner.connectable = true;
    blend_inner.required = true;
    blend.inputs.push_back(blend_inner);
    graph::PortDesc blend_outer;
    blend_outer.number = kPortBlendOuter;
    blend_outer.name = "outer";
    blend_outer.label = "Outer field";
    blend_outer.description = "The field that takes over downwind of the transition (-x). It must sit on the same "
                              "lattice as the inner field and carry the same dimension: two lattices are refused "
                              "rather than fitted, and tesla blended with volts per metre is refused rather than "
                              "added.";
    blend_outer.type = qp::ports::kVectorField;
    blend_outer.connectable = true;
    blend_outer.required = true;
    blend.inputs.push_back(blend_outer);
    graph::PortDesc blend_transition =
        parameter(kPortBlendTransition, "transition_x", "Transition x", "m", kDefaultBlendTransitionM);
    blend_transition.description = "Where the weight is one half, in metres along x. The default, twenty earth "
                                   "radii downwind, is where the reference implementation puts it: the near-Earth "
                                   "field reaches that far and the tail's sheet model is meaningful beyond it.";
    blend.inputs.push_back(blend_transition);
    graph::PortDesc blend_width = parameter(kPortBlendWidth, "width", "Width", "m", kDefaultBlendWidthM);
    blend_width.description = "The scale over which the weight moves, in metres. It is the thickness of the "
                              "transition and the length the correction is proportional to: the source layer a "
                              "straight blend would leave behind is as thick as this number.";
    blend.inputs.push_back(blend_width);
    graph::PortDesc blend_correction =
        parameter(kPortBlendCorrection, "correction", "Correction", "1", kDefaultBlendCorrection);
    blend_correction.description = "How much of the divergence-free term to apply: 1 is all of it, 0 is the "
                                   "straight blend, and 0.1 is what the reference implementation applies -- kept "
                                   "as a value rather than a history note, because reproducing its fields is then "
                                   "one number away.";
    blend.inputs.push_back(blend_correction);
    graph::PortDesc blend_out;
    blend_out.number = kPortBlendOut;
    blend_out.name = "field";
    blend_out.label = "Field";
    blend_out.description = "The blended field, described exactly as the inner field is.";
    blend_out.type = qp::ports::kVectorField;
    blend_out.connectable = true;
    blend_out.required = false;
    blend_out.unit_symbol = "T";
    blend.outputs.push_back(blend_out);

    // ---------------- the resampler ----------------
    graph::NodeDesc resample;
    resample.type_name = kResampleType;
    resample.label = "Resample";
    resample.description = "Moves the field on its socket onto the lattice its own grid ports declare, "
                           "trilinearly -- the same interpolation a kernel reads a table with. It is the explicit "
                           "step every other combinator's refusal points at: a sum, a product or a blend needs "
                           "its inputs on one lattice, and this is the node that puts them there. A target that "
                           "reaches outside the source is refused rather than filled with the boundary value.";
    resample.category = "field";
    resample.version = 1;
    resample.allow_in_field_domain = true;
    resample.allow_in_particle_domain = false;
    resample.has_compute = true;
    graph::PortDesc resample_in;
    resample_in.number = kPortResampleField;
    resample_in.name = "field";
    resample_in.label = "Field";
    resample_in.description = "The field to move. Its samples are read through the sampler, so a field baked on a "
                              "coarse box and one baked on a fine box can be combined after this node has put "
                              "them together.";
    resample_in.type = qp::ports::kVectorField;
    resample_in.connectable = true;
    resample_in.required = true;
    resample.inputs.push_back(resample_in);
    // Its own nine grid ports, after the single field socket, and its own numbers -- the rule every type here
    // follows after a shared numbering produced a registration collision.
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z", "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",   "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",   "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x", "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.25 * kEarthRadiusM,
                                 0.25 * kEarthRadiusM, 0.25 * kEarthRadiusM, 1.0, 1.0, 1.0};
        graph::PortDesc port = parameter(kPortResampleOrigin0 + offset, names[offset], labels[offset],
                                         units[offset], steps[offset]);
        port.description = "The target lattice's own description. A box that reaches outside the source's is "
                           "refused: the sampler clamps for a particle that leaves the modelled region, and a "
                           "resample that clamped would fill a slab of the target with the boundary value and "
                           "call it a field.";
        resample.inputs.push_back(port);
    }
    graph::PortDesc resample_out;
    resample_out.number = kPortResampleOut;
    resample_out.name = "field";
    resample_out.label = "Field";
    resample_out.description = "The same field on the target lattice, described exactly as the input is.";
    resample_out.type = qp::ports::kVectorField;
    resample_out.connectable = true;
    resample_out.required = false;
    resample_out.unit_symbol = "T";
    resample.outputs.push_back(resample_out);

    // ---------------- the magnetopause ----------------
    graph::NodeDesc magnetopause;
    magnetopause.type_name = kMagnetopauseType;
    magnetopause.label = "Magnetopause";
    magnetopause.description = "The Shue surface as a smooth weight: one inside, zero outside. Wire it into a "
                               "multiplier beside a dipole and the dipole stops at the boundary instead of filling "
                               "the box -- the smooth version of a region mask, and the boundary that node said "
                               "would arrive as a model of its own. The surface stands at the standoff distance on "
                               "the sunward axis and flares away from it.";
    magnetopause.category = "field";
    magnetopause.version = 1;
    magnetopause.allow_in_field_domain = true;
    magnetopause.allow_in_particle_domain = false;
    magnetopause.has_compute = true;
    graph::PortDesc standoff =
        parameter(kPortMagnetopauseStandoff, "standoff", "Standoff distance", "m",
                  kDefaultMagnetopauseStandoffRe * kEarthRadiusM);
    standoff.description = "Where the surface crosses the sunward axis, in metres. Ten earth radii is the textbook "
                           "value at ordinary solar-wind pressure; the reference computes it from Kp, which is a "
                           "driver's job and becomes a socket when this kit has one.";
    magnetopause.inputs.push_back(standoff);
    graph::PortDesc flaring = parameter(kPortMagnetopauseFlaring, "flaring", "Flaring exponent", "1",
                                        kDefaultMagnetopauseFlaring);
    flaring.description = "The exponent alpha in `r_mp = r0 (2 / (1 + cos theta))^alpha`. It does nothing at the "
                          "nose -- the factor is one there -- and at the flank it is the whole shape: the surface "
                          "stands at `2^alpha r0`. Shue's 0.58 and the reference's 0.59 at Kp = 2 agree to two "
                          "percent, which is finer than the model.";
    magnetopause.inputs.push_back(flaring);
    graph::PortDesc boundary_width =
        parameter(kPortMagnetopauseWidth, "width", "Width", "m", kDefaultMagnetopauseWidthM);
    boundary_width.description = "How thick the transition is, in metres. The reference uses four earth radii "
                                 "because in its model this number is also the width of the magnetosheath draping "
                                 "layer; one earth radius is a boundary's own thickness, and the draping is a "
                                 "different node's job.";
    magnetopause.inputs.push_back(boundary_width);
    // The optional driver socket, after the grid ports: a Kp index on a wire decides the surface, and an empty
    // socket leaves the two parameters above as the model. See `kPortMagnetopauseKp`.
    graph::PortDesc magnetopause_kp;
    magnetopause_kp.number = kPortMagnetopauseKp;
    magnetopause_kp.name = "kp";
    magnetopause_kp.label = "Kp (optional)";
    magnetopause_kp.description = "Wire a `source.kp` here and the standoff distance and the flaring come from that "
                                  "index -- the reference's own model, in the one place that knows how Kp becomes "
                                  "geometry. Left empty, the two parameters above are the surface.";
    magnetopause_kp.type = qp::ports::kScalarF64;
    magnetopause_kp.connectable = true;
    magnetopause_kp.required = false;
    magnetopause_kp.unit_symbol = "1";
    magnetopause.inputs.push_back(magnetopause_kp);
    // Its own nine grid ports, after its three parameters.
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z", "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",   "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",   "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x", "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.2 * kEarthRadiusM,
                                 0.2 * kEarthRadiusM, 0.2 * kEarthRadiusM, 1.0, 1.0, 1.0};
        magnetopause.inputs.push_back(
            parameter(kPortMagnetopauseOrigin0 + offset, names[offset], labels[offset], units[offset],
                      steps[offset]));
    }
    graph::PortDesc magnetopause_out;
    magnetopause_out.number = kPortWeight;
    magnetopause_out.name = "weight";
    magnetopause_out.label = "Weight";
    magnetopause_out.description = "The boundary as a dimensionless weight: one inside, zero outside, one half on "
                                   "the surface.";
    magnetopause_out.type = qp::ports::kScalarField;
    magnetopause_out.connectable = true;
    magnetopause_out.required = false;
    magnetopause_out.unit_symbol = "1";
    magnetopause.outputs.push_back(magnetopause_out);

    // ---------------- the mix ----------------
    graph::NodeDesc mix;
    mix.type_name = kMixType;
    mix.label = "Mix by weight";
    mix.description = "Blends the two fields on its first two sockets by the weight on the third, and adds the "
                      "vector-potential term that keeps the blend divergence-free -- the reference's magnetopause "
                      "mixing. The weight can be any scalar table here, which is what the other blend cannot do: it "
                      "owns a sigmoid and therefore owns its derivative, while this one differentiates the table "
                      "and pays for it. No grid of its own: all three inputs must already share one.";
    mix.category = "field";
    mix.version = 1;
    mix.allow_in_field_domain = true;
    mix.allow_in_particle_domain = false;
    mix.has_compute = true;
    graph::PortDesc mix_first;
    mix_first.number = kPortMixA;
    mix_first.name = "a";
    mix_first.label = "Field at weight zero";
    mix_first.description = "The field that wins where the weight is zero. It carries the lattice and the dimension "
                            "the mix is built and described in.";
    mix_first.type = qp::ports::kVectorField;
    mix_first.connectable = true;
    mix_first.required = true;
    mix.inputs.push_back(mix_first);
    graph::PortDesc mix_second;
    mix_second.number = kPortMixB;
    mix_second.name = "b";
    mix_second.label = "Field at weight one";
    mix_second.description = "The field that wins where the weight is one. Same lattice and same dimension: two "
                             "lattices are refused rather than fitted, and tesla mixed with volts per metre is "
                             "refused rather than added.";
    mix_second.type = qp::ports::kVectorField;
    mix_second.connectable = true;
    mix_second.required = true;
    mix.inputs.push_back(mix_second);
    graph::PortDesc mix_weight;
    mix_weight.number = kPortMixWeight;
    mix_weight.name = "weight";
    mix_weight.label = "Weight";
    mix_weight.description = "The weight table, dimensionless, from a mask, a magnetopause or any other producer of "
                             "a scalar field. Its gradient is differenced from the table, so a weight that is "
                             "linear where it matters is reproduced exactly and a curved one is corrected to the "
                             "difference's own order.";
    mix_weight.type = qp::ports::kScalarField;
    mix_weight.connectable = true;
    mix_weight.required = true;
    mix.inputs.push_back(mix_weight);
    graph::PortDesc mix_correction =
        parameter(kPortMixCorrection, "correction", "Correction", "1", kDefaultMixCorrection);
    mix_correction.description = "How much of the vector-potential term to apply: 1 keeps the mix divergence-free "
                                 "where the potentials it can build are genuine, 0 is the straight blend the case "
                                 "measures against.";
    mix.inputs.push_back(mix_correction);
    graph::PortDesc mix_out;
    mix_out.number = kPortMixOut;
    mix_out.name = "field";
    mix_out.label = "Field";
    mix_out.description = "The mixed field, described exactly as the field on the first socket is.";
    mix_out.type = qp::ports::kVectorField;
    mix_out.connectable = true;
    mix_out.required = false;
    mix_out.unit_symbol = "T";
    mix.outputs.push_back(mix_out);

    // ---------------- the Volland-Stern shielding coefficient ----------------
    graph::NodeDesc shield;
    shield.type_name = kShieldType;
    shield.label = "Volland-Stern shield";
    shield.description = "The screening factor of the shielded convection field: one at and beyond the shielding "
                         "radius, and `(r / r0)^2` inside it. It is a **coefficient**, not a field, and the "
                         "reference's own composition shows why: `E = corotation + mul(convection, shield)`. Wire "
                         "it into a multiplier beside the convection field and the drift stops inside `r0`, which is "
                         "what makes the plasmapause a boundary rather than a guess.";
    shield.category = "field";
    shield.version = 1;
    shield.allow_in_field_domain = true;
    shield.allow_in_particle_domain = false;
    shield.has_compute = true;
    graph::PortDesc shield_radius = parameter(kPortShieldR0, "r0", "Shielding radius", "m", kDefaultShieldR0M);
    shield_radius.description = "Where the coefficient reaches one, in metres. Four earth radii is the reference "
                                "implementation's value, and it is the radius the reference's own range (one to ten) "
                                "puts in the middle of the competition between convection and corotation.";
    shield.inputs.push_back(shield_radius);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z", "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",   "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",   "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x", "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.1 * kEarthRadiusM,
                                 0.1 * kEarthRadiusM, 0.1 * kEarthRadiusM, 1.0, 1.0, 1.0};
        shield.inputs.push_back(
            parameter(kPortShieldOrigin0 + offset, names[offset], labels[offset], units[offset], steps[offset]));
    }
    graph::PortDesc shield_out;
    shield_out.number = kPortShieldOut;
    shield_out.name = "coef";
    shield_out.label = "Coefficient";
    shield_out.description = "The screening factor, dimensionless: one outside the shielding radius and falling to "
                             "zero at the centre.";
    shield_out.type = qp::ports::kScalarField;
    shield_out.connectable = true;
    shield_out.required = false;
    shield_out.unit_symbol = "1";
    shield.outputs.push_back(shield_out);

    // ---------------- the convection field ----------------
    graph::NodeDesc convection;
    convection.type_name = kConvectionType;
    convection.label = "Convection E field";
    convection.description = "The Volland-Stern dawn-dusk potential's field, E = (-2Ay, -2Ax, 0) with the "
                             "amplitude A. Wire it into a pusher's electric socket beside a dipole and the "
                             "particles convect: sunward on the dayside, antisunward on the nightside.";
    convection.category = "field";
    convection.version = 1;
    convection.allow_in_field_domain = true;
    convection.allow_in_particle_domain = false;
    convection.has_compute = true;
    graph::PortDesc amplitude = parameter(kPortConvectionA, "amplitude", "Amplitude", "V/m^2", 1.0e-13);
    amplitude.description = "The potential's amplitude A, in volts per metre squared. The field it produces is "
                            "|E| = 2 A r, so 1.5e-12 is 0.2 mV/m at ten earth radii -- the order of the real "
                            "cross-polar-cap field mapped to the equatorial plane.";
    convection.inputs.push_back(amplitude);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z", "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",   "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",   "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x", "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.1 * kEarthRadiusM,
                                 0.1 * kEarthRadiusM, 0.1 * kEarthRadiusM, 1.0, 1.0, 1.0};
        convection.inputs.push_back(parameter(kPortConvectionOrigin0 + offset, names[offset], labels[offset],
                                              units[offset], steps[offset]));
    }
    graph::PortDesc c_out;
    c_out.number = kPortField;
    c_out.name = "field";
    c_out.label = "Electric field";
    c_out.description = "The baked convection field, as a volume of volt-per-metre vectors.";
    c_out.type = qp::ports::kVectorField;
    c_out.connectable = true;
    c_out.required = false;
    c_out.unit_symbol = "V/m";
    convection.outputs.push_back(c_out);

    // ---------------- the corotation field ----------------
    graph::NodeDesc corotation;
    corotation.type_name = kCorotationType;
    corotation.label = "Corotation E field";
    corotation.description = "The electric field a plasma moving with the Earth sees: E = -(Omega x r) x B, from "
                             "the magnetic field wired into it. Beside the convection field it is the other half "
                             "of the real picture -- inside the radius where the two balance the plasma rotates "
                             "with the planet, outside it convects.";
    corotation.category = "field";
    corotation.version = 1;
    corotation.allow_in_field_domain = true;
    corotation.allow_in_particle_domain = false;
    corotation.has_compute = true;
    graph::PortDesc corotation_magnetic;
    corotation_magnetic.number = kPortCorotationMagnetic;
    corotation_magnetic.name = "magnetic";
    corotation_magnetic.label = "Magnetic field";
    corotation_magnetic.description = "The field to corotate in, wired from any field node's output. **This node "
                                      "has no grid of its own**: it bakes on the lattice of the field it reads, "
                                      "because `E` is defined pointwise from `B` and asking the user for a second "
                                      "grid would be asking for a way to put the two in different places.";
    corotation_magnetic.type = qp::ports::kVectorField;
    corotation_magnetic.connectable = true;
    corotation_magnetic.required = true;
    corotation.inputs.push_back(corotation_magnetic);
    graph::PortDesc r_out;
    r_out.number = kPortCorotationOut;
    r_out.name = "field";
    r_out.label = "Electric field";
    r_out.description = "The corotation field, as a volume of volt-per-metre vectors on the input's lattice.";
    r_out.type = qp::ports::kVectorField;
    r_out.connectable = true;
    r_out.required = false;
    r_out.unit_symbol = "V/m";
    corotation.outputs.push_back(r_out);

    // ---------------- the atmosphere ----------------
    graph::NodeDesc atmosphere;
    atmosphere.type_name = kAtmosphereType;
    atmosphere.label = "Atmosphere drag";
    atmosphere.description = "An exponential drag rate, nu = nu0 exp(-(r - r_ref)/H), as a scalar field in per "
                            "second. Wire it into a pusher's drag socket and turn that socket on: the first force "
                            "in this kit that takes energy away, and the first whose decay an experiment can "
                            "measure.";
    atmosphere.category = "field";
    atmosphere.version = 1;
    atmosphere.allow_in_field_domain = true;
    atmosphere.allow_in_particle_domain = false;
    atmosphere.has_compute = true;
    graph::PortDesc nu0 = parameter(kPortAtmosphereNu0, "nu0", "Rate at r_ref", "1/s", 1.0e-3);
    nu0.description = "The drag rate quoted at the reference radius. Zero, the default, is no atmosphere at all: "
                      "a graph that wires this node and does not set a rate gets a drag of nothing rather than a "
                      "guessed one.";
    atmosphere.inputs.push_back(nu0);
    graph::PortDesc height = parameter(kPortAtmosphereScaleHeight, "scale_height", "Scale height", "m", 1.0e4);
    height.description = "How far the rate falls by a factor of e. 100 km is the thermosphere's order at low "
                         "altitude; refuses to be zero, because an atmosphere that is a wall is not this model.";
    atmosphere.inputs.push_back(height);
    graph::PortDesc reference = parameter(kPortAtmosphereReference, "reference_radius", "Reference radius", "m",
                                          kEarthRadiusM);
    reference.description = "Where the rate above is quoted. A parameter rather than the surface, because what "
                            "\"the rate at the surface\" means depends on where the model's surface is.";
    atmosphere.inputs.push_back(reference);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z", "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",   "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",   "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x", "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.1 * kEarthRadiusM,
                                 0.1 * kEarthRadiusM, 0.1 * kEarthRadiusM, 1.0, 1.0, 1.0};
        atmosphere.inputs.push_back(parameter(kPortAtmosphereOrigin0 + offset, names[offset], labels[offset],
                                              units[offset], steps[offset]));
    }
    graph::PortDesc a_out;
    a_out.number = kPortAtmosphereOut;
    a_out.name = "drag";
    a_out.label = "Drag rate";
    a_out.description = "The baked rate, a scalar volume in per second, for a pusher's drag socket.";
    a_out.type = qp::ports::kScalarField;
    a_out.connectable = true;
    a_out.required = false;
    a_out.unit_symbol = "1/s";
    atmosphere.outputs.push_back(a_out);

    // ---------------- the tail's current sheet ----------------
    graph::NodeDesc sheet;
    sheet.type_name = kCurrentSheetType;
    sheet.label = "Tail current sheet";
    sheet.description = "A Harris current sheet, B = (B0 tanh(z/L), 0, 0): the analytic model of the stretched "
                        "nightside. Add it to a dipole for the classic magnetosphere cross-section -- closed lines "
                        "on the dayside and stretched ones on the nightside -- or use it alone to study the "
                        "neutral sheet, where the field is exactly zero.";
    sheet.category = "field";
    sheet.version = 1;
    sheet.allow_in_field_domain = true;
    sheet.allow_in_particle_domain = false;
    sheet.has_compute = true;
    graph::PortDesc b0 = parameter(kPortSheetB0, "b0", "Lobe field", "T", 1.0e-9);
    b0.description = "The field the sheet saturates to away from the plane. Five nanotesla is the quiet tail's "
                     "order, four orders below the surface field, which is why the sum of this and a dipole is a "
                     "dipole near the Earth and a sheet far from it.";
    sheet.inputs.push_back(b0);
    graph::PortDesc thickness = parameter(kPortSheetThickness, "half_thickness", "Half thickness", "m",
                                          kEarthRadiusM);
    thickness.description = "The sheet's half-thickness: how far from the plane the field has risen to tanh(1) of "
                            "its lobe value. A few earth radii at the distances a first course looks at.";
    sheet.inputs.push_back(thickness);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const char* names[9] = {"origin_x", "origin_y", "origin_z", "spacing_x", "spacing_y",
                                "spacing_z", "count_x",  "count_y",   "count_z"};
        const char* labels[9] = {"Grid origin x", "Grid origin y", "Grid origin z",   "Grid spacing x",
                                 "Grid spacing y", "Grid spacing z", "Nodes along x", "Nodes along y",
                                 "Nodes along z"};
        const char* units[9] = {"m", "m", "m", "m", "m", "m", "", "", ""};
        const double steps[9] = {kEarthRadiusM, kEarthRadiusM, kEarthRadiusM, 0.1 * kEarthRadiusM,
                                 0.1 * kEarthRadiusM, 0.1 * kEarthRadiusM, 1.0, 1.0, 1.0};
        sheet.inputs.push_back(parameter(kPortSheetOrigin0 + offset, names[offset], labels[offset], units[offset],
                                         steps[offset]));
    }
    graph::PortDesc s_out;
    s_out.number = kPortField;
    s_out.name = "field";
    s_out.label = "Magnetic field";
    s_out.description = "The baked sheet, as a volume of tesla vectors.";
    s_out.type = qp::ports::kVectorField;
    s_out.connectable = true;
    s_out.required = false;
    s_out.unit_symbol = "T";
    sheet.outputs.push_back(s_out);

    return {std::move(dipole),      std::move(uniform),    std::move(sum),        std::move(electric),
            std::move(mask),        std::move(mul),        std::move(convection), std::move(corotation),
            std::move(atmosphere),  std::move(sheet),      std::move(blend),      std::move(resample),
            std::move(magnetopause), std::move(mix),     std::move(shield)};
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
    if (desc.type_name == SourceNodes::kDayType) {
        // The date, through the same path as the index: the node's parameter arrives in `inputs`, and the whole of
        // the conversion -- the clamp and the cosine -- is the driver's own function, so a second consumer of the
        // tilt cannot disagree with this one about what day 200 means.
        const graph::InputView day_view{inputs};
        const double day = real_or(day_view, SourceNodes::kPortDay, SourceNodes::kDefaultDay);
        Outcome out;
        out.emplace_back(SourceNodes::kPortDayOut, qp::ports::Value{SourceNodes::tilt_degrees_for_day(day)});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == SourceNodes::kKpType) {
        // A driver is the shortest clause in this function and the reason there is no second evaluator: it publishes
        // the number it carries, and `EvalContext` allows this plugin exactly one evaluator for every type it
        // registers. The value is read from the node through `inputs`, which is where a node's own parameters
        // arrive -- so a parameter and a socket are the same code path here, which is what makes the driver's value
        // indistinguishable from a typed one once it is on the wire.
        const graph::InputView driver_view{inputs};
        const double kp = real_or(driver_view, SourceNodes::kPortKp, SourceNodes::kDefaultKp);
        Outcome out;
        out.emplace_back(SourceNodes::kPortKpOut,
                         qp::ports::Value{std::clamp(kp, SourceNodes::kMinKp, SourceNodes::kMaxKp)});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kCurrentSheetType) {
        const graph::InputView sheet_view{inputs};
        const FieldNodes::SheetSpec spec = FieldNodes::read_sheet_from(sheet_view);
        const GridSpec sheet_grid = FieldNodes::read_from(sheet_view, FieldNodes::kPortSheetOrigin0);
        const gfield::FieldKey sheet_key{id.index, FieldNodes::kPortField};
        if (!bake_current_sheet(spec, sheet_grid, sheet_key, *fields_)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortField, qp::ports::Value{fields_->view(sheet_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kAtmosphereType) {
        const graph::InputView atmosphere_view{inputs};
        const FieldNodes::AtmosphereSpec spec = FieldNodes::read_atmosphere_from(atmosphere_view);
        const GridSpec atmosphere_grid = FieldNodes::read_from(atmosphere_view, FieldNodes::kPortAtmosphereOrigin0);
        const gfield::FieldKey atmosphere_key{id.index, FieldNodes::kPortAtmosphereOut};
        if (!bake_atmosphere(spec, atmosphere_grid, atmosphere_key, *fields_)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortAtmosphereOut, qp::ports::Value{fields_->view(atmosphere_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kCorotationType) {
        // The field to corotate in, and **its** geometry: the node bakes on the lattice it reads, so the two
        // cannot be put in different places. `resolve_field_origin` is the resolver the plan builder uses, and it
        // walks back through the combinators that have no grid of their own.
        const gfield::FieldValue magnetic = input_field(id, FieldNodes::kPortCorotationMagnetic);
        GridSpec source_grid;
        if (graph_ == nullptr ||
            !resolve_field_origin(*graph_, id, FieldNodes::kPortCorotationMagnetic, source_grid)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        const gfield::FieldKey corotation_key{id.index, FieldNodes::kPortCorotationOut};
        if (!bake_corotation(magnetic, source_grid.origin_m, source_grid.spacing_m, source_grid, corotation_key,
                             *fields_)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortCorotationOut, qp::ports::Value{fields_->view(corotation_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kConvectionType) {
        const graph::InputView convection_view{inputs};
        const double amplitude = real_or(convection_view, FieldNodes::kPortConvectionA,
                                         FieldNodes::kDefaultConvectionA);
        const GridSpec convection_grid =
            FieldNodes::read_from(convection_view, FieldNodes::kPortConvectionOrigin0);
        const gfield::FieldKey convection_key{id.index, FieldNodes::kPortField};
        if (!bake_convection(amplitude, convection_grid, convection_key, *fields_)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortField, qp::ports::Value{fields_->view(convection_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
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
    if (desc.type_name == FieldNodes::kBlendType) {
        // No grid to read, and positions it cannot do without: the weight is a function of `x` and the flux is
        // integrated in `z`, so the geometry comes from `resolve_field_origin` -- the same walk the plan builder
        // uses -- rather than from the tables, which carry counts and not places.
        const graph::InputView blend_view{inputs};
        const FieldNodes::BlendSpec blend_spec = FieldNodes::read_blend_from(blend_view);
        GridSpec blend_grid;
        if (graph_ == nullptr ||
            !resolve_field_origin(*graph_, id, FieldNodes::kPortBlendInner, blend_grid)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        const gfield::FieldKey blend_key{id.index, FieldNodes::kPortBlendOut};
        const gfield::FieldValue inner = input_field(id, FieldNodes::kPortBlendInner);
        const gfield::FieldValue outer = input_field(id, FieldNodes::kPortBlendOuter);
        if (!bake_blend(inner, outer, blend_grid, blend_spec, blend_key, *fields_)) {
            // Either socket missing, two lattices that disagree, two dimensions that disagree, or a spec the
            // arithmetic cannot be built on: all of them are refusals rather than approximations.
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortBlendOut, qp::ports::Value{fields_->view(blend_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kResampleType) {
        // Two geometries and neither of them is in a table: the **source**'s comes from the node that baked it
        // (`resolve_field_origin`, the same walk the plan builder uses) and the **target**'s is this node's own
        // declaration. Nothing is inferred from the counts, which is the point -- a resample is the one operation
        // that must know both boxes.
        const graph::InputView resample_view{inputs};
        const GridSpec target_grid = FieldNodes::read_from(resample_view, FieldNodes::kPortResampleOrigin0);
        GridSpec source_grid;
        if (graph_ == nullptr ||
            !resolve_field_origin(*graph_, id, FieldNodes::kPortResampleField, source_grid)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        const gfield::FieldKey resample_key{id.index, FieldNodes::kPortResampleOut};
        const gfield::FieldValue source = input_field(id, FieldNodes::kPortResampleField);
        if (!bake_resample(source, source_grid, target_grid, resample_key, *fields_)) {
            // An unreadable socket, a source grid that disagrees with the table, or a target the source cannot
            // cover: all three are refusals rather than approximations.
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortResampleOut, qp::ports::Value{fields_->view(resample_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kShieldType) {
        // A producer with its own grid, like the mask: the coefficient is a function of position and of nothing
        // else, so it declares where it is sampled rather than borrowing a lattice it was not given.
        const graph::InputView shield_view{inputs};
        const double r0_m = real_or(shield_view, FieldNodes::kPortShieldR0, FieldNodes::kDefaultShieldR0M);
        const GridSpec shield_grid = FieldNodes::read_from(shield_view, FieldNodes::kPortShieldOrigin0);
        const gfield::FieldKey shield_key{id.index, FieldNodes::kPortShieldOut};
        if (!bake_shield(r0_m, shield_grid, shield_key, *fields_)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortShieldOut, qp::ports::Value{fields_->view(shield_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kMagnetopauseType) {
        // A producer with its own grid, like the mask: the smooth boundary declares where it is sampled rather than
        // borrowing the lattice of a field it is not given.
        const graph::InputView magnetopause_view{inputs};
        FieldNodes::MagnetopauseSpec magnetopause_spec = FieldNodes::read_magnetopause_from(magnetopause_view);
        // **The driver, if one is wired.** One place knows how `Kp` becomes geometry -- `SourceNodes` -- and this
        // asks it rather than repeating the formula, which is the whole reason the conversion lives on the driver.
        // An empty socket leaves the two parameters in charge, so a graph written before this port existed keeps
        // its surface to the last bit.
        const qp::ports::Value kp = magnetopause_view.get(FieldNodes::kPortMagnetopauseKp);
        if (kp.valid() && std::isfinite(kp.to_double())) {
            magnetopause_spec.standoff_m = SourceNodes::standoff_re_for_kp(kp.to_double()) * kEarthRadiusM;
            magnetopause_spec.flaring = SourceNodes::flaring_for_kp(kp.to_double());
        }
        const GridSpec magnetopause_grid =
            FieldNodes::read_from(magnetopause_view, FieldNodes::kPortMagnetopauseOrigin0);
        const gfield::FieldKey magnetopause_key{id.index, FieldNodes::kPortWeight};
        if (!bake_magnetopause(magnetopause_spec, magnetopause_grid, magnetopause_key, *fields_)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortWeight, qp::ports::Value{fields_->view(magnetopause_key).desc});
        return qp::diag::Result<Outcome>{std::move(out)};
    }
    if (desc.type_name == FieldNodes::kMixType) {
        // Three tables in and one out, no grid to read: like a sum or a product, a mix is defined on the lattice its
        // inputs share, and the geometry those tables cannot carry comes from the resolver.
        const graph::InputView mix_view{inputs};
        const double mix_correction = real_or(mix_view, FieldNodes::kPortMixCorrection,
                                              FieldNodes::kDefaultMixCorrection);
        GridSpec mix_grid;
        if (graph_ == nullptr || !resolve_field_origin(*graph_, id, FieldNodes::kPortMixA, mix_grid)) {
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        const gfield::FieldKey mix_key{id.index, FieldNodes::kPortMixOut};
        const gfield::FieldValue mix_a = input_field(id, FieldNodes::kPortMixA);
        const gfield::FieldValue mix_b = input_field(id, FieldNodes::kPortMixB);
        const gfield::FieldValue mix_weight = input_field(id, FieldNodes::kPortMixWeight);
        if (!bake_mix(mix_a, mix_b, mix_weight, mix_grid, mix_correction, mix_key, *fields_)) {
            // A missing socket, three lattices that do not agree, two dimensions that do not, or a correction
            // outside `[0, 1]`: refusals rather than approximations.
            return qp::diag::Result<Outcome>{qp::diag::ErrorCode::invalid_argument};
        }
        Outcome out;
        out.emplace_back(FieldNodes::kPortMixOut, qp::ports::Value{fields_->view(mix_key).desc});
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
    double tilt = real_or(view, FieldNodes::kPortTiltDegrees, kMagneticTiltDegrees);
    // **The driver, if one is wired**: a date decides the lean and the parameter becomes the fallback, which is the
    // optional-socket shape the magnetopause's Kp socket established. The value arrives in degrees because the port
    // says `deg`, so nothing is converted here -- a conversion in this line would be a second opinion about a unit
    // the port already states.
    const qp::ports::Value driven_tilt = view.get(FieldNodes::kPortTiltDriver);
    if (driven_tilt.valid() && std::isfinite(driven_tilt.to_double())) tilt = driven_tilt.to_double();
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
