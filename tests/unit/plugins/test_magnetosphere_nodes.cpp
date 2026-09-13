/**
 * @file test_magnetosphere_nodes.cpp
 * @brief The field node, the bake, and the binding: from a graph to a particle that moves the way the field
 *        points.
 *
 * Test case ids match the @tests fields in `plugins/magnetosphere/include`.
 *
 * ## What this file is for
 *
 * The other half of this plugin's tests measures physics against closed forms. This half measures the **bridge**:
 * a node type registered with a host, a bake that publishes into a store, a plan builder that follows a wire, and
 * an executor that drives the kernel the plan named. Every one of those steps is a place where the run can be
 * wrong without any physics being wrong -- a field bound to the wrong node, a grid that describes a different box
 * from the samples, a plan that quietly dropped a node.
 *
 * The last case is the one that ties them together: a graph whose field node is a dipole and whose particle is a
 * proton, integrated by the executor the graph produced. It asserts the **sense** of the gyration against the
 * field's own sign, which is a prediction that no single half of this kit can make: the dipole's sign convention
 * and the pusher's rotation direction have to agree, and if either one is flipped the particle orbits the wrong
 * way around a field that still points south.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/eval/evaluator.hpp>
#include <qp/graph/field/field_set.hpp>
#include <qp/graph/kernels/kernel.hpp>
#include <qp/graph/particles/executor.hpp>
#include <qp/graph/particles/particle_state.hpp>
#include <qp/graph/structure.hpp>
#include <qp/host/host.hpp>

#include <qp/plugins/magnetosphere/boris.hpp>
#include <qp/plugins/magnetosphere/dipole.hpp>
#include <qp/plugins/magnetosphere/field_nodes.hpp>
#include <qp/plugins/magnetosphere/geomagnetic.hpp>
#include <qp/plugins/magnetosphere/plan.hpp>
#include <qp/plugins/magnetosphere/source_nodes.hpp>
#include <qp/plugins/magnetosphere/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace qp::plugins::magnetosphere;

namespace {

namespace graph = qp::graph;
namespace gfield = qp::graph::field;
namespace pk = qp::graph::kernels;
namespace pp = qp::graph::particles;

/// @brief The relative difference between two numbers, guarded against a zero denominator.
///
/// A local copy rather than a shared header: the two test files of this plugin are one binary and two
/// translation units, and a helper that had to be moved into a header to be shared would be a header that
/// exists for the tests' convenience rather than for the platform's contract.
[[nodiscard]] double relative_to(double a, double b) {
    const double scale = std::abs(b) > 0.0 ? std::abs(b) : 1.0;
    return std::abs(a - b) / scale;
}

/// @brief `div B` at one node of a table, by central differences in all three axes.
///
/// Interior nodes only: a one-sided difference at the boundary is a different operator with a first-order error,
/// and mixing the two would make it impossible to say which one a number came from. The fields the blend's case
/// uses are independent of `y`, so the middle term is exactly zero -- which is a fact about those fields rather
/// than about this operator, and the case asserts it instead of skipping it.
[[nodiscard]] double divergence_at(const gfield::FieldValue& table, const Vec3& spacing_m, std::uint32_t i,
                                   std::uint32_t j, std::uint32_t k) {
    const std::uint32_t ny = static_cast<std::uint32_t>(table.desc.count[1]);
    const std::uint32_t nz = static_cast<std::uint32_t>(table.desc.count[2]);
    const auto at = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        return (static_cast<std::uint64_t>(a) * ny + b) * nz + c;
    };
    const double dx = (gfield::get_component(table, at(i + 1, j, k), 0) -
                       gfield::get_component(table, at(i - 1, j, k), 0)) /
                      (2.0 * spacing_m.x);
    const double dy = (gfield::get_component(table, at(i, j + 1, k), 1) -
                       gfield::get_component(table, at(i, j - 1, k), 1)) /
                      (2.0 * spacing_m.y);
    const double dz = (gfield::get_component(table, at(i, j, k + 1), 2) -
                       gfield::get_component(table, at(i, j, k - 1), 2)) /
                      (2.0 * spacing_m.z);
    return dx + dy + dz;
}

/// @brief The largest `|div B|` over the interior of a table, in tesla per metre.
[[nodiscard]] double worst_divergence(const gfield::FieldValue& table, const Vec3& spacing_m) {
    const std::uint32_t nx = static_cast<std::uint32_t>(table.desc.count[0]);
    const std::uint32_t ny = static_cast<std::uint32_t>(table.desc.count[1]);
    const std::uint32_t nz = static_cast<std::uint32_t>(table.desc.count[2]);
    const std::uint32_t j = ny / 2;
    double worst = 0.0;
    for (std::uint32_t i = 1; i + 1 < nx; ++i) {
        for (std::uint32_t k = 1; k + 1 < nz; ++k) {
            const double value = std::abs(divergence_at(table, spacing_m, i, j, k));
            if (value > worst) worst = value;
        }
    }
    return worst;
}

/// @brief A graph, the catalog that describes its types, and the store its bake publishes into.
///
/// Members are declared in the order they depend on each other: the store outlives the evaluator that borrows it,
/// and the host's catalog outlives every evaluation. Constructed in place and never copied, because an
/// `EvalContext` holds pointers into all three.
struct Scene final {
    qp::host::PluginHost host{qp::plugin::Capability::node_types | qp::plugin::Capability::field_domain |
                              qp::plugin::Capability::particle_domain};
    graph::Graph g{};
    gfield::FieldSet fields{};
    DipoleEvaluator evaluator{fields};
    graph::EvalCache cache{64};
    graph::EvalResult result{};

    Scene() {
        // Fourteen field models now: the dipole, the uniform field, the sum, the uniform electric field, the region
        // mask, the multiplier, the convection field, the corotation field, the atmosphere, the current sheet, the
        // blend, the resampler, the magnetopause and the mix. Each is a **type of its own** with its own port
        // numbers, which is the composition principle -- a shielding field is `mul(convection, shield)` -- and three
        // of them could not be composed out of the others: no wiring of sums and products keeps a field
        // divergence-free (the two blends), and none of them moves a field onto another lattice (the resampler).
        REQUIRE(FieldNodes::mount(host) == 15);
        REQUIRE(SourceNodes::mount(host) == 2);
        REQUIRE(PusherNodes::mount(host) == 2);
    }

    [[nodiscard]] graph::EvalContext ctx() noexcept {
        return graph::EvalContext{&host.node_types(), &qp::ports::builtin_registry(), &evaluator, &cache};
    }

    [[nodiscard]] graph::NodeId add(const char* type) {
        const auto r = g.add_node(type);
        REQUIRE(r.has_value());
        return r.value();
    }

    void set(graph::NodeId id, graph::PortNumber port, double v) {
        g.find_node_mutable(id)->set_param(port, qp::ports::Value{v});
        g.bump_version();
    }

    void wire(graph::NodeId from, graph::PortNumber from_port, graph::NodeId to, graph::PortNumber to_port) {
        REQUIRE(g.connect(graph::PortRef{from, from_port, graph::PortDirection::output},
                          graph::PortRef{to, to_port, graph::PortDirection::input}));
    }

    /// @brief Every occupied slot, in slot order -- the order a plan builder is handed.
    ///
    /// Not `build_plan`, because that prunes to a set of **declared outputs** and this kit has no output node yet.
    /// The builder takes an order rather than computing one precisely so that this stands in today and
    /// `graph/domain`'s plan stands in tomorrow, with nothing else changing.
    [[nodiscard]] std::vector<graph::NodeId> order() const {
        std::vector<graph::NodeId> out;
        for (std::size_t i = 1; i < g.slots().size(); ++i) {
            if (g.slots()[i].occupied) out.push_back(g.slots()[i].node.id);
        }
        return out;
    }

    [[nodiscard]] qp::diag::Result<graph::EvalStats> bake() {
        result = graph::EvalResult{};
        // The evaluator borrows the graph, exactly as `MagnetosphereRun` hands it over: a node with field inputs
        // can reach its inputs' samples only by following their wires.
        evaluator.set_graph(g);
        return graph::evaluate_graph(g, ctx(), result);
    }
};

/// @brief A dipole node configured for a test: tilt, moment and a grid the caller chooses.
[[nodiscard]] graph::NodeId add_dipole(Scene& scene, double tilt_degrees, double half_extent_re,
                                       double spacing_re, std::uint32_t nodes) {
    const graph::NodeId id = scene.add(FieldNodes::kDipoleType);
    scene.set(id, FieldNodes::kPortTiltDegrees, tilt_degrees);
    scene.set(id, FieldNodes::kPortMomentAm2, kDipoleMomentAm2);
    scene.set(id, FieldNodes::kPortOrigin0, -half_extent_re * kEarthRadiusM);
    scene.set(id, FieldNodes::kPortOrigin1, -half_extent_re * kEarthRadiusM);
    scene.set(id, FieldNodes::kPortOrigin2, -half_extent_re * kEarthRadiusM);
    scene.set(id, FieldNodes::kPortSpacing0, spacing_re * kEarthRadiusM);
    scene.set(id, FieldNodes::kPortSpacing1, spacing_re * kEarthRadiusM);
    scene.set(id, FieldNodes::kPortSpacing2, spacing_re * kEarthRadiusM);
    scene.set(id, FieldNodes::kPortCount0, static_cast<double>(nodes));
    scene.set(id, FieldNodes::kPortCount1, static_cast<double>(nodes));
    scene.set(id, FieldNodes::kPortCount2, static_cast<double>(nodes));
    return id;
}

/// @brief The angle the velocity swept in the `xy` plane over a run, accumulated step by step.
[[nodiscard]] double swept_xy(pp::ParticleState& state, pp::ParticleExecutor& executor,
                              pk::AdvanceContext& ctx, std::size_t steps) {
    double total = 0.0;
    double previous = std::atan2(state.at(0, 1, pp::ParticleState::Slot::velocity),
                                 state.at(0, 0, pp::ParticleState::Slot::velocity));
    for (std::size_t step = 0; step < steps; ++step) {
        REQUIRE(executor.advance(ctx).has_value());
        const double angle = std::atan2(state.at(0, 1, pp::ParticleState::Slot::velocity),
                                        state.at(0, 0, pp::ParticleState::Slot::velocity));
        double delta = angle - previous;
        if (delta > 3.14159265358979323846) delta -= 2.0 * 3.14159265358979323846;
        if (delta < -3.14159265358979323846) delta += 2.0 * 3.14159265358979323846;
        total += delta;
        previous = angle;
    }
    return total;
}

}  // namespace

TEST_CASE("magnetosphere.field_nodes.the_type_declares_the_ports_the_evaluator_reads", "[magnetosphere]") {
    // A node type and the code that reads it are two halves of one fact, and the failure mode of a split is the
    // one this repository keeps finding: a declaration nothing consumes, or a reader consuming a number the
    // declaration never made. The check is that every port the reader asks for exists, with the type the reader
    // needs, and that the output is the field port a pusher can be wired to.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    // Seven field models: the dipole, the uniform magnetic field, the sum, the uniform electric field, the region
    // mask, the multiplier and the convection field. Each is a **type of its own** with its own port numbers,
    // which is the composition principle -- a shielding field is `mul(convection, shield)`, not a switch inside a
    // node.
    REQUIRE(types.size() == 15);
    REQUIRE(types[0].type_name == FieldNodes::kDipoleType);
    REQUIRE(types[1].type_name == FieldNodes::kUniformType);
    REQUIRE(types[2].type_name == FieldNodes::kSumType);
    REQUIRE(types[3].type_name == FieldNodes::kUniformElectricType);
    // The mask is last, and its output is the one thing that distinguishes it from every other type here: a
    // **scalar** field. The declaration and the bake have to agree about that, because a consumer reads its
    // samples by the component count the registry publishes.
    REQUIRE(types[4].type_name == FieldNodes::kMaskType);
    REQUIRE(types[4].has_compute);
    REQUIRE(types[4].allow_in_field_domain);
    REQUIRE_FALSE(types[4].allow_in_particle_domain);
    const graph::PortDesc* weight = types[4].find_port(FieldNodes::kPortWeight, /*is_output=*/true);
    REQUIRE(weight != nullptr);
    REQUIRE(weight->connectable);
    REQUIRE(weight->type == qp::ports::kScalarField);
    const qp::ports::PortTypeDesc* scalar = qp::ports::builtin_registry().find(qp::ports::kScalarField);
    REQUIRE(scalar != nullptr);
    REQUIRE(scalar->is_field());
    REQUIRE(scalar->field_components == 1);
    const graph::NodeDesc& dipole = types.front();
    REQUIRE(dipole.type_name == FieldNodes::kDipoleType);
    REQUIRE(dipole.valid());
    REQUIRE(dipole.has_compute);
    // The domain flags: a bake may allocate and block, and the particle domain forbids both. A field node allowed
    // in the particle domain would be a bake on a frame boundary.
    REQUIRE(dipole.allow_in_field_domain);
    REQUIRE_FALSE(dipole.allow_in_particle_domain);

    const graph::PortNumber expected_inputs[] = {
        FieldNodes::kPortTiltDegrees, FieldNodes::kPortMomentAm2, FieldNodes::kPortOrigin0,
        FieldNodes::kPortOrigin1,     FieldNodes::kPortOrigin2,     FieldNodes::kPortSpacing0,
        FieldNodes::kPortSpacing1,    FieldNodes::kPortSpacing2,    FieldNodes::kPortCount0,
        FieldNodes::kPortCount1,      FieldNodes::kPortCount2};
    for (const graph::PortNumber number : expected_inputs) {
        const graph::PortDesc* port = dipole.find_port(number, /*is_output=*/false);
        REQUIRE(port != nullptr);
        // A bake grid is a **parameter**, so it has no socket. That is the unified Param/Port rule's other half,
        // and it is what makes the grid reachable from the evaluator at all: `evaluate_graph` hands an unwired
        // input port the node's own parameter value.
        REQUIRE_FALSE(port->connectable);
        REQUIRE(port->type == qp::ports::kScalarF64);
        REQUIRE_FALSE(port->name.empty());
    }
    // Every port number the reader uses is distinct, or two of them would be the same parameter read twice.
    for (std::size_t i = 0; i < std::size(expected_inputs); ++i) {
        for (std::size_t j = i + 1; j < std::size(expected_inputs); ++j) {
            REQUIRE(expected_inputs[i] != expected_inputs[j]);
        }
    }

    const graph::PortDesc* out = dipole.find_port(FieldNodes::kPortField, /*is_output=*/true);
    REQUIRE(out != nullptr);
    REQUIRE(out->connectable);
    REQUIRE(out->type == qp::ports::kVectorField);

    // And the port type really does describe three components. `check_value` compares the descriptor's component
    // count against the type's, so a `kVectorField` that carried 1 would make every bake a plugin fault -- and
    // the message would be about the plugin rather than about the registry entry that is wrong.
    const qp::ports::PortTypeDesc* vector = qp::ports::builtin_registry().find(qp::ports::kVectorField);
    REQUIRE(vector != nullptr);
    REQUIRE(vector->is_field());
    REQUIRE(vector->field_components == 3);

    // Mounting is what makes the type reachable from a running program rather than only from a test fixture. The
    // second mount registers nothing, because a name that is taken is left alone rather than duplicated.
    qp::host::PluginHost host{qp::plugin::Capability::node_types};
    // Fourteen field models now, and the count is asserted rather than assumed: it is the one place a new type
    // announces itself in the test suite, so a type that silently failed to register is a failure here.
    REQUIRE(FieldNodes::mount(host) == 15);
    REQUIRE(host.node_types().find(FieldNodes::kDipoleType) != nullptr);
    REQUIRE(FieldNodes::mount(host) == 0);
}

TEST_CASE("magnetosphere.field_nodes.a_mask_weights_the_region_it_names", "[magnetosphere]") {
    // **The kit's first scalar product**, and the case is built around the two things that can be wrong about one:
    // the **region test** (which nodes are inside) and the **shape of the table** (one number per node, not three).
    // The second is the one that would go unnoticed for a while -- a consumer reading three-component samples out
    // of a one-component table reads its own values out of alignment, and the numbers it gets are other nodes'
    // values rather than nothing.
    //
    // Every node is checked rather than sampled: a region test that is right at the origin and wrong at one corner
    // is a weight with a hole in it, and a picture would not show it.
    const GridSpec grid{Vec3{-2.0 * kEarthRadiusM, -2.0 * kEarthRadiusM, -2.0 * kEarthRadiusM},
                        Vec3{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM},
                        9, 9, 9};
    gfield::FieldSet fields;
    const gfield::FieldKey key{3, FieldNodes::kPortWeight};

    FieldNodes::MaskSpec mask;
    mask.r0_m = 1.5 * kEarthRadiusM;
    mask.r1_m = 2.5 * kEarthRadiusM;

    // The shape: a **scalar** f64 volume, one sample per node, dimensionless.
    mask.region = FieldNodes::MaskRegion::sphere;
    REQUIRE(bake_mask(mask, grid, key, fields));
    const gfield::FieldValue weights = fields.view(key);
    REQUIRE(gfield::is_readable(weights));
    REQUIRE(weights.kind() == gfield::Kind::Volume);
    REQUIRE_FALSE(weights.is_vector());
    REQUIRE(weights.desc.component == qp::abi::ComponentKind::scalar);
    REQUIRE(weights.desc.element == qp::abi::ElementType::f64);
    REQUIRE(weights.point_count() == grid.point_count());

    // The region test, node by node, against the same predicate the model is written in.
    const auto agrees = [&](FieldNodes::MaskRegion region, double r0_re, double r1_re,
                            const std::function<bool(const Vec3&)>& inside) {
        FieldNodes::MaskSpec spec;
        spec.region = region;
        spec.r0_m = r0_re * kEarthRadiusM;
        spec.r1_m = r1_re * kEarthRadiusM;
        REQUIRE(bake_mask(spec, grid, key, fields));
        const gfield::FieldValue table = fields.view(key);
        REQUIRE(gfield::is_readable(table));
        for (std::uint32_t i = 0; i < grid.nx; ++i) {
            for (std::uint32_t j = 0; j < grid.ny; ++j) {
                for (std::uint32_t k = 0; k < grid.nz; ++k) {
                    const Vec3 point = grid.node_position(i, j, k);
                    const std::uint64_t at = (static_cast<std::uint64_t>(i) * grid.ny + j) * grid.nz + k;
                    const double expected = inside(point) ? 1.0 : 0.0;
                    // **Exactly** 0 or 1: the weight is a region test, so an intermediate value would be a claim
                    // about a boundary this node does not model.
                    REQUIRE(gfield::get_component(table, at, 0) == expected);
                }
            }
        }
    };

    agrees(FieldNodes::MaskRegion::sphere, 1.5, 2.5, [](const Vec3& p) { return norm(p) < 1.5 * kEarthRadiusM; });
    agrees(FieldNodes::MaskRegion::shell, 1.5, 2.5, [](const Vec3& p) {
        const double r = norm(p);
        return r >= 1.5 * kEarthRadiusM && r < 2.5 * kEarthRadiusM;
    });
    // The terminator plane through the centre, for the reason the implementation states: the reference puts it ten
    // earth radii downwind because that is where a magnetopause stands, and a standoff belongs to the boundary
    // model rather than to a mask.
    agrees(FieldNodes::MaskRegion::dayside, 1.0, 3.0, [](const Vec3& p) { return p.x > 0.0; });
    agrees(FieldNodes::MaskRegion::nightside, 1.0, 3.0, [](const Vec3& p) { return p.x <= 0.0; });

    // One rule in two places, and the difference between them is the assertion. The **reader** orders the two radii
    // -- a user who swaps them in the panel has described the same shell, and `MaskSpec`'s own invariant says an
    // ordered pair. The **bake** refuses a pair that arrives reversed, because such a spec was built by a caller
    // that did not use the reader, and baking it would produce an empty table indistinguishable from a region that
    // happens to hold no nodes.
    qp::graph::Node swapped_node;
    swapped_node.type_name = FieldNodes::kMaskType;
    swapped_node.set_param(FieldNodes::kPortMaskRegion, qp::ports::Value{std::int64_t{1}});
    swapped_node.set_param(FieldNodes::kPortMaskR0, qp::ports::Value{2.5 * kEarthRadiusM});
    swapped_node.set_param(FieldNodes::kPortMaskR1, qp::ports::Value{1.5 * kEarthRadiusM});
    const FieldNodes::MaskSpec ordered = FieldNodes::read_mask(swapped_node);
    REQUIRE(ordered.r0_m == 1.5 * kEarthRadiusM);
    REQUIRE(ordered.r1_m == 2.5 * kEarthRadiusM);
    REQUIRE(bake_mask(ordered, grid, key, fields));
    const gfield::FieldValue ordered_table = fields.view(key);
    std::uint64_t inside_count = 0;
    for (std::uint64_t at = 0; at < ordered_table.point_count(); ++at) {
        if (gfield::get_component(ordered_table, at, 0) == 1.0) ++inside_count;
    }
    REQUIRE(inside_count > 0);

    FieldNodes::MaskSpec reversed;
    reversed.region = FieldNodes::MaskRegion::shell;
    reversed.r0_m = 2.5 * kEarthRadiusM;
    reversed.r1_m = 1.5 * kEarthRadiusM;
    REQUIRE_FALSE(bake_mask(reversed, grid, key, fields));

    // A grid that cannot be baked is refused rather than approximated: one node an axis has no interior, and a
    // zero spacing has no distances.
    const GridSpec too_small{Vec3{}, Vec3{1.0, 1.0, 1.0}, 1, 1, 1};
    REQUIRE_FALSE(bake_mask(mask, too_small, key, fields));
    const GridSpec no_spacing{Vec3{}, Vec3{0.0, 1.0, 1.0}, 4, 4, 4};
    REQUIRE_FALSE(bake_mask(mask, no_spacing, key, fields));

    // The reader is the one the evaluator uses, so "which port is r0" has one answer in the file rather than two.
    // A node carrying only a region index therefore gets the **defaults** for the radii, which is the state a
    // freshly placed node is in.
    qp::graph::Node node;
    node.type_name = FieldNodes::kMaskType;
    node.set_param(FieldNodes::kPortMaskRegion, qp::ports::Value{std::int64_t{2}});
    const FieldNodes::MaskSpec read = FieldNodes::read_mask(node);
    REQUIRE(read.region == FieldNodes::MaskRegion::dayside);
    REQUIRE(read.r0_m == FieldNodes::kDefaultMaskR0Re * kEarthRadiusM);
    REQUIRE(read.r1_m == FieldNodes::kDefaultMaskR1Re * kEarthRadiusM);
    // And an index nobody defined takes the first region rather than reading past the end of a switch: the region
    // is a choice, and a document from a build with more regions must not become undefined behaviour here.
    node.set_param(FieldNodes::kPortMaskRegion, qp::ports::Value{std::int64_t{99}});
    REQUIRE(FieldNodes::read_mask(node).region == FieldNodes::MaskRegion::sphere);
    REQUIRE(std::string{FieldNodes::to_string(FieldNodes::MaskRegion::nightside)} == "nightside");
}

TEST_CASE("magnetosphere.field_nodes.a_field_scales_by_its_weight", "[magnetosphere]") {
    // **The composition the header names**: "a shielding field is `mul(convection, shield)`, not a switch inside a
    // node". This case builds exactly that out of the two nodes whose pair it is -- a region mask and the
    // multiplier -- and checks the result node by node.
    //
    // The failure this shape guards against is not an arithmetic slip but an **index-space** one: a scalar table
    // has one value where a vector table has three, so a loop that walked one index across both would take every
    // third weight. The product would be smooth, plausible and about a third as strong in places, which is the
    // kind of wrong that survives a picture.
    const GridSpec grid{Vec3{-3.0 * kEarthRadiusM, -3.0 * kEarthRadiusM, -3.0 * kEarthRadiusM},
                        Vec3{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM},
                        13, 13, 13};
    gfield::FieldSet fields;
    const gfield::FieldKey field_key{4, FieldNodes::kPortField};
    const gfield::FieldKey weight_key{5, FieldNodes::kPortWeight};
    const gfield::FieldKey product_key{6, FieldNodes::kPortMulOut};

    REQUIRE(bake_uniform(Vec3{0.0, 0.0, 2.0e-5}, grid, field_key, fields, tesla_dimension()));
    FieldNodes::MaskSpec mask;
    mask.region = FieldNodes::MaskRegion::sphere;
    mask.r0_m = 1.5 * kEarthRadiusM;
    mask.r1_m = 1.5 * kEarthRadiusM;
    REQUIRE(bake_mask(mask, grid, weight_key, fields));

    const gfield::FieldValue field = fields.view(field_key);
    const gfield::FieldValue weight = fields.view(weight_key);
    REQUIRE(bake_scaled(field, weight, product_key, fields));
    const gfield::FieldValue product = fields.view(product_key);
    REQUIRE(gfield::is_readable(product));
    REQUIRE(product.is_vector());
    // The dimension travels unchanged: scaling by a pure number does not change what the field is, and a product
    // labelled with a different `FieldDim` would be a second answer to "what is this table".
    REQUIRE(product.desc.dimension.M == field.desc.dimension.M);
    REQUIRE(product.desc.dimension.T == field.desc.dimension.T);
    REQUIRE(product.desc.dimension.I == field.desc.dimension.I);
    REQUIRE(product.point_count() == field.point_count());

    // Every node, both halves: inside the sphere the product is the field, outside it the product is zero, and the
    // **input is untouched** -- a node that wrote into its input's buffer would make the graph's second reader see
    // the first reader's arithmetic.
    std::uint64_t inside = 0;
    std::uint64_t outside = 0;
    for (std::uint64_t point = 0; point < product.point_count(); ++point) {
        const double w = gfield::get_component(weight, point, 0);
        const bool is_inside = w == 1.0;
        for (std::uint64_t component = 0; component < 3; ++component) {
            const double expected = is_inside ? gfield::get_component(field, point, component) : 0.0;
            REQUIRE(gfield::get_component(product, point, component) == expected);
        }
        is_inside ? ++inside : ++outside;
    }
    REQUIRE(inside > 0);
    REQUIRE(outside > 0);
    for (std::uint64_t point = 0; point < field.point_count(); ++point) {
        REQUIRE(gfield::get_component(field, point, 2) == 2.0e-5);
    }

    // A weight that is not a scalar volume is refused: a vector in the weight socket would otherwise be read as
    // one number per point -- its first component -- and the product would be a field scaled by another field's x.
    REQUIRE_FALSE(bake_scaled(field, field, product_key, fields));
    // And two lattices that disagree about their counts are refused rather than fitted, which is the same rule
    // `field.sum` follows and for the same reason: fitting one to the other invents a field neither model made.
    const GridSpec other{Vec3{-3.0 * kEarthRadiusM, -3.0 * kEarthRadiusM, -3.0 * kEarthRadiusM},
                         Vec3{1.0 * kEarthRadiusM, 1.0 * kEarthRadiusM, 1.0 * kEarthRadiusM},
                         7, 7, 7};
    const gfield::FieldKey coarse_key{7, FieldNodes::kPortWeight};
    REQUIRE(bake_mask(mask, other, coarse_key, fields));
    REQUIRE_FALSE(bake_scaled(field, fields.view(coarse_key), product_key, fields));

    // The port types are the first refusal, one layer below the code: a scalar where the field belongs and a vector
    // where the weight belongs are both refused by `check_connection`, so the multiplier's own check is the second
    // line of defence rather than the only one.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[5].type_name == FieldNodes::kMulType);
    REQUIRE(types[5].has_compute);
    const graph::PortDesc* mul_field = types[5].find_port(FieldNodes::kPortMulField, false);
    const graph::PortDesc* mul_weight = types[5].find_port(FieldNodes::kPortMulWeight, false);
    REQUIRE(mul_field != nullptr);
    REQUIRE(mul_weight != nullptr);
    REQUIRE(mul_field->type == qp::ports::kVectorField);
    REQUIRE(mul_weight->type == qp::ports::kScalarField);
    const qp::ports::PortTypeDesc* vector_type = qp::ports::builtin_registry().find(qp::ports::kVectorField);
    const qp::ports::PortTypeDesc* scalar_type = qp::ports::builtin_registry().find(qp::ports::kScalarField);
    REQUIRE(vector_type != nullptr);
    REQUIRE(scalar_type != nullptr);
    REQUIRE_FALSE(qp::ports::check_connection(*scalar_type, qp::ports::PortDirection::output, *vector_type,
                                              qp::ports::PortDirection::input)
                      .acceptable());
    REQUIRE_FALSE(qp::ports::check_connection(*vector_type, qp::ports::PortDirection::output, *scalar_type,
                                              qp::ports::PortDirection::input)
                      .acceptable());
    // And the legal direction is accepted, so the two refusals above are about the mismatch rather than about this
    // call being unable to say yes to anything.
    REQUIRE(qp::ports::check_connection(*vector_type, qp::ports::PortDirection::output, *vector_type,
                                        qp::ports::PortDirection::input)
                .acceptable());
}

TEST_CASE("magnetosphere.field_nodes.a_blend_does_not_open_a_divergence", "[magnetosphere]") {
    // **The reference's `tail_blend` and `internal_blend` are one operation**, and the operation is not the
    // average: `(1 - w) A + w C` can be wired out of `field.mul` and `field.sum` the moment something publishes
    // `1 - w`, so the arithmetic is not why this type exists. What makes it a node is the term the product rule
    // contributes -- `w'(x) (psi_outer - psi_inner)` on `B_z` -- without which a blend of two divergence-free
    // fields has a source layer as thick as the transition. Field lines end in mid-air, the picture still looks
    // like a magnetosphere, and nothing else in this kit would say a word.
    //
    // The pair of fields here is chosen so that the *failure* has a closed form: the inner side is a uniform
    // field along `z`, so its `B_x` is zero and its flux vanishes under the lattice's own anchor, and the outer
    // side is a Harris sheet. A straight blend's divergence is then `w'(x) B0 tanh(z/L)` and nothing else, which
    // is a prediction this case checks **before** it checks that the correction removes it.
    const double inner_field_t = 1.0e-8;
    const double b0_tesla = 1.0e-8;
    const double half_thickness_m = 2.0 * kEarthRadiusM;
    const Vec3 spacing{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.1 * kEarthRadiusM};
    const GridSpec grid{Vec3{-30.0 * kEarthRadiusM, -0.5 * kEarthRadiusM, -6.0 * kEarthRadiusM}, spacing, 81, 3,
                        121};

    gfield::FieldSet fields;
    const gfield::FieldKey inner_key{21, FieldNodes::kPortField};
    const gfield::FieldKey outer_key{22, FieldNodes::kPortField};
    REQUIRE(bake_uniform(Vec3{0.0, 0.0, inner_field_t}, grid, inner_key, fields, tesla_dimension()));
    FieldNodes::SheetSpec sheet;
    sheet.b0_tesla = b0_tesla;
    sheet.half_thickness_m = half_thickness_m;
    REQUIRE(bake_current_sheet(sheet, grid, outer_key, fields));
    const gfield::FieldValue inner = fields.view(inner_key);
    const gfield::FieldValue outer = fields.view(outer_key);

    // The premise, measured rather than assumed: **both inputs are divergence-free**, so whatever the blend's own
    // table shows is the blend's doing. The sheet's divergence is zero because it varies in `z` alone and its only
    // component is `x`; the uniform field's because it is constant.
    REQUIRE(worst_divergence(inner, spacing) == 0.0);
    REQUIRE(worst_divergence(outer, spacing) == 0.0);

    FieldNodes::BlendSpec blend;
    blend.transition_m = -20.0 * kEarthRadiusM;
    blend.width_m = 2.5 * kEarthRadiusM;
    const gfield::FieldKey naive_key{23, FieldNodes::kPortBlendOut};
    const gfield::FieldKey corrected_key{24, FieldNodes::kPortBlendOut};
    const gfield::FieldKey reference_key{25, FieldNodes::kPortBlendOut};
    blend.correction = 0.0;
    REQUIRE(bake_blend(inner, outer, grid, blend, naive_key, fields));
    blend.correction = 1.0;
    REQUIRE(bake_blend(inner, outer, grid, blend, corrected_key, fields));
    blend.correction = FieldNodes::kReferenceBlendCorrection;
    REQUIRE(bake_blend(inner, outer, grid, blend, reference_key, fields));

    const gfield::FieldValue straight = fields.view(naive_key);
    const gfield::FieldValue corrected = fields.view(corrected_key);
    const gfield::FieldValue reference = fields.view(reference_key);
    REQUIRE(gfield::is_readable(straight));
    REQUIRE(gfield::is_readable(corrected));
    REQUIRE(gfield::is_readable(reference));
    REQUIRE(corrected.is_vector());
    // Tesla, described from the inner field: a blend of two tesla tables is a tesla table.
    REQUIRE(corrected.desc.dimension.M == 1);
    REQUIRE(corrected.desc.dimension.T == -2);
    REQUIRE(corrected.point_count() == inner.point_count());

    // Every node of both tables. The `x` and `y` components are the convex combination **exactly** -- the inner
    // field's `x` is zero, so the product that survives is one multiplication -- and the `z` rows are compared
    // with a tolerance rather than an equality, because `A + C dpsi` is a multiply-add a compiler may contract
    // into one operation: the last bit is not part of the model and asserting it would be asserting the code
    // generator. The two tables differ **only** in `z`, which is the structural statement: the correction is a
    // term of one component, not a reshaping of the field.
    const std::uint32_t nx = grid.nx;
    const std::uint32_t ny = grid.ny;
    const std::uint32_t nz = grid.nz;
    const double dz = spacing.z;
    for (std::uint32_t i = 0; i < nx; ++i) {
        const double x = grid.origin_m.x + static_cast<double>(i) * spacing.x;
        const double w = 1.0 / (1.0 + std::exp((x - blend.transition_m) / blend.width_m));
        const double slope = -w * (1.0 - w) / blend.width_m;
        for (std::uint32_t j = 0; j < ny; ++j) {
            double dpsi = 0.0;
            for (std::uint32_t k = 0; k < nz; ++k) {
                const std::uint64_t point = (static_cast<std::uint64_t>(i) * ny + j) * nz + k;
                const double sheet_x = gfield::get_component(outer, point, 0);
                if (k > 0) {
                    // `psi_outer - psi_inner`, trapezoid in `z` from the box's lowest node, anchored at zero --
                    // the same recurrence the bake runs, rebuilt here from the *table* rather than from the model.
                    dpsi -= 0.5 * (sheet_x + gfield::get_component(outer, point - 1, 0)) * dz;
                }
                REQUIRE(gfield::get_component(corrected, point, 0) == w * sheet_x);
                REQUIRE(gfield::get_component(straight, point, 0) == w * sheet_x);
                REQUIRE(gfield::get_component(corrected, point, 1) == 0.0);
                REQUIRE(gfield::get_component(straight, point, 1) == 0.0);
                REQUIRE(relative_to(gfield::get_component(straight, point, 2), (1.0 - w) * inner_field_t) <
                        1.0e-15);
                REQUIRE(relative_to(gfield::get_component(corrected, point, 2),
                                    (1.0 - w) * inner_field_t + slope * dpsi) < 1.0e-15);
            }
        }
    }

    // The same term against the **model's** flux, not the table's: `psi = -B0 L ln cosh(z/L)`, anchored at the
    // box's lower `z` so the two agree by construction at the first node. The trapezoid sum and this closed form
    // differ by the quadrature's truncation error, `O(dz^2 f')`, which is the gap this measures -- and a gap much
    // larger than that would mean the recurrence had drifted rather than rounded.
    double worst_model_gap = 0.0;
    for (std::uint32_t i = 0; i < nx; ++i) {
        const double x = grid.origin_m.x + static_cast<double>(i) * spacing.x;
        const double w = 1.0 / (1.0 + std::exp((x - blend.transition_m) / blend.width_m));
        const double slope = -w * (1.0 - w) / blend.width_m;
        for (std::uint32_t k = 0; k < nz; ++k) {
            const double z = grid.origin_m.z + static_cast<double>(k) * spacing.z;
            const double psi_model = -b0_tesla * half_thickness_m *
                                     (std::log(std::cosh(z / half_thickness_m)) -
                                      std::log(std::cosh(grid.origin_m.z / half_thickness_m)));
            const std::uint64_t point = (static_cast<std::uint64_t>(i) * ny + ny / 2) * nz + k;
            const double gap = std::abs(gfield::get_component(corrected, point, 2) -
                                        ((1.0 - w) * inner_field_t + slope * psi_model));
            if (gap > worst_model_gap) worst_model_gap = gap;
        }
    }

    // **The measurement the node exists for.** The straight blend's divergence is the closed form below; the
    // corrected blend's is the quadrature's own floor. Three numbers, three different statements:
    //   - the closed form is right (the failure is understood, not merely observed);
    //   - the correction removes it (and by how much: the ratio is asserted, not asserted *away*);
    //   - the reference's factor is **linear** in the residual -- `(1 - c)` of the layer -- which is what makes
    //     `0.1` a tenth of the correction rather than a different model.
    const double worst_straight = worst_divergence(straight, spacing);
    const double worst_corrected = worst_divergence(corrected, spacing);
    const double worst_reference = worst_divergence(reference, spacing);
    double worst_predicted = 0.0;
    for (std::uint32_t i = 1; i + 1 < nx; ++i) {
        const double x = grid.origin_m.x + static_cast<double>(i) * spacing.x;
        const double w = 1.0 / (1.0 + std::exp((x - blend.transition_m) / blend.width_m));
        const double slope = -w * (1.0 - w) / blend.width_m;
        for (std::uint32_t k = 1; k + 1 < nz; ++k) {
            const double z = grid.origin_m.z + static_cast<double>(k) * spacing.z;
            const double predicted = std::abs(slope * b0_tesla * std::tanh(z / half_thickness_m));
            if (predicted > worst_predicted) worst_predicted = predicted;
        }
    }
    CAPTURE(worst_straight, worst_corrected, worst_reference, worst_predicted, worst_model_gap);
    REQUIRE(worst_predicted > 0.0);
    REQUIRE(worst_straight > 0.0);
    // The failure is understood rather than merely observed: the straight blend's worst divergence is the closed
    // form `w'(x) B0 tanh(z/L)` to within the two percent a central difference over half an earth radius is
    // allowed to be wrong by. Measured 1.6e-16 T/m at the middle of the transition, which is `B0 / (4 * width)`.
    REQUIRE(relative_to(worst_straight, worst_predicted) < 0.02);
    // The correction removes it -- all but a **three-hundredth** of it. What is left is not zero because the flux
    // is integrated by the trapezoid rule, whose truncation error is `(dz^2/12) f'`: the residual is the
    // quadrature's floor rather than the model's, and it is asserted as that ratio so that a correction which
    // stopped working (a sign flip, a factor dropped) cannot hide inside a small absolute number.
    REQUIRE(worst_corrected * 100.0 < worst_straight);
    REQUIRE(relative_to(worst_corrected / worst_straight, 3.3e-3) < 0.3);
    // And the reference's factor is **linear** in what is left: `(1 - c)` of the layer, so `0.1` is a tenth of
    // the correction rather than a different model. That is the whole content of keeping it as a value.
    REQUIRE(relative_to(worst_reference, 0.9 * worst_straight) < 0.001);
    // The table's trapezoid and the model's closed-form flux agree to four parts in a hundred thousand of `B0`
    // (measured 4.1e-13 T against a lobe field of 1e-8), which is the same quadrature error seen from the other
    // side: a recurrence that had drifted rather than rounded would be orders of magnitude past this. The constant
    // here is the measured one -- the analytic bound `(dz^2/12) f'` had predicted four times it, and the machine
    // was right.
    REQUIRE(relative_to(worst_model_gap, 4.1e-5 * b0_tesla) < 0.3);

    // Refusals. Two lattices are refused rather than fitted -- the rule `field.sum` follows and for the same
    // reason -- and so is a grid that disagrees with the tables it was resolved for, which is the only place
    // positions exist at all.
    const GridSpec coarse{Vec3{-30.0 * kEarthRadiusM, -0.5 * kEarthRadiusM, -6.0 * kEarthRadiusM},
                          Vec3{1.0 * kEarthRadiusM, 1.0 * kEarthRadiusM, 0.2 * kEarthRadiusM}, 41, 3, 61};
    const gfield::FieldKey coarse_key{26, FieldNodes::kPortField};
    REQUIRE(bake_current_sheet(sheet, coarse, coarse_key, fields));
    REQUIRE_FALSE(bake_blend(inner, fields.view(coarse_key), coarse, blend, naive_key, fields));
    REQUIRE_FALSE(bake_blend(inner, outer, coarse, blend, naive_key, fields));
    // Tesla blended with volts per metre: both are vector fields, so the port types cannot catch this and the
    // bake does -- a blend of two different quantities is not a quantity.
    const gfield::FieldKey volts_key{27, FieldNodes::kPortField};
    REQUIRE(bake_uniform(Vec3{0.0, 0.0, 1.0}, grid, volts_key, fields, volt_per_metre_dimension()));
    REQUIRE_FALSE(bake_blend(inner, fields.view(volts_key), grid, blend, naive_key, fields));
    // A scalar where a vector belongs, and specs whose arithmetic does not exist: a zero width would make the
    // weight a step and the correction a spike, and a factor outside `[0, 1]` would be a blend that adds the
    // divergence it is supposed to remove.
    const gfield::FieldKey weight_key{28, FieldNodes::kPortWeight};
    FieldNodes::MaskSpec mask;
    REQUIRE(bake_mask(mask, grid, weight_key, fields));
    REQUIRE_FALSE(bake_blend(inner, fields.view(weight_key), grid, blend, naive_key, fields));
    FieldNodes::BlendSpec zero_width = blend;
    zero_width.width_m = 0.0;
    REQUIRE_FALSE(bake_blend(inner, outer, grid, zero_width, naive_key, fields));
    FieldNodes::BlendSpec too_much = blend;
    too_much.correction = 1.5;
    REQUIRE_FALSE(bake_blend(inner, outer, grid, too_much, naive_key, fields));
    FieldNodes::BlendSpec negative = blend;
    negative.correction = -0.1;
    REQUIRE_FALSE(bake_blend(inner, outer, grid, negative, naive_key, fields));
    FieldNodes::BlendSpec not_a_number = blend;
    not_a_number.transition_m = std::nan("");
    REQUIRE_FALSE(bake_blend(inner, outer, grid, not_a_number, naive_key, fields));

    // The reader is the one the evaluator uses, so "which port is the width" has one answer in the file: a node
    // carrying only that parameter gets the defaults for the rest, which is the state a freshly placed node is in.
    qp::graph::Node node;
    node.type_name = FieldNodes::kBlendType;
    node.set_param(FieldNodes::kPortBlendWidth, qp::ports::Value{1234.0});
    const FieldNodes::BlendSpec read = FieldNodes::read_blend(node);
    REQUIRE(read.width_m == 1234.0);
    REQUIRE(read.transition_m == FieldNodes::kDefaultBlendTransitionM);
    REQUIRE(read.correction == FieldNodes::kDefaultBlendCorrection);
    // **All of the correction is the default**, and the reference's number has a name rather than a comment.
    REQUIRE(FieldNodes::kDefaultBlendCorrection == 1.0);
    REQUIRE(FieldNodes::kReferenceBlendCorrection == 0.1);

    // The declaration: the type's own port numbers, both sockets vector fields, and three parameters that are
    // typed into a panel rather than wired from a node.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[10].type_name == FieldNodes::kBlendType);
    REQUIRE(types[10].has_compute);
    REQUIRE(types[10].allow_in_field_domain);
    REQUIRE_FALSE(types[10].allow_in_particle_domain);
    const graph::PortDesc* inner_port = types[10].find_port(FieldNodes::kPortBlendInner, false);
    const graph::PortDesc* outer_port = types[10].find_port(FieldNodes::kPortBlendOuter, false);
    REQUIRE(inner_port != nullptr);
    REQUIRE(outer_port != nullptr);
    REQUIRE(inner_port->type == qp::ports::kVectorField);
    REQUIRE(outer_port->type == qp::ports::kVectorField);
    REQUIRE(inner_port->required);
    REQUIRE(outer_port->required);
    const graph::PortDesc* blend_out = types[10].find_port(FieldNodes::kPortBlendOut, true);
    REQUIRE(blend_out != nullptr);
    REQUIRE(blend_out->type == qp::ports::kVectorField);
    REQUIRE(blend_out->unit_symbol == std::string{"T"});
    for (graph::PortNumber number : {FieldNodes::kPortBlendTransition, FieldNodes::kPortBlendWidth,
                                     FieldNodes::kPortBlendCorrection}) {
        const graph::PortDesc* port = types[10].find_port(number, false);
        REQUIRE(port != nullptr);
        REQUIRE_FALSE(port->connectable);
    }
}

TEST_CASE("magnetosphere.field_nodes.the_convection_field_is_the_potentials_gradient", "[magnetosphere]") {
    // **A model whose answer is known in closed form, which is why gamma is fixed at 2.** The Volland-Stern
    // potential `phi = A r^2 sin(2 azimuth)` collapses to `2 A x y` in Cartesian coordinates, so the field is
    // `E = -grad phi = (-2Ay, -2Ax, 0)`: **linear** in position. A linear field on a uniform lattice is reproduced
    // by trilinear interpolation exactly, which turns this case from "the bake is close to the model" into "the
    // bake *is* the model" -- at the nodes and between them.
    const GridSpec grid{Vec3{-4.0 * kEarthRadiusM, -4.0 * kEarthRadiusM, -4.0 * kEarthRadiusM},
                        Vec3{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM},
                        17, 17, 17};
    const double amplitude = 1.5e-12;
    gfield::FieldSet fields;
    const gfield::FieldKey key{9, FieldNodes::kPortField};
    REQUIRE(bake_convection(amplitude, grid, key, fields));

    const gfield::FieldValue field = fields.view(key);
    REQUIRE(gfield::is_readable(field));
    REQUIRE(field.is_vector());
    // Volts per metre, which is what tells this apart from the dipole's tesla: the pusher reads both sockets
    // through one vocabulary and cannot tell them apart by the value.
    REQUIRE(field.desc.dimension.L == 1);
    REQUIRE(field.desc.dimension.T == -3);
    REQUIRE(field.desc.dimension.I == -1);

    // Every node, against the closed form, as an **equality**: `-2 A y` and `-2 A x` are two multiplications, and
    // the bake performs the same two, so a difference here would be a difference in the model rather than in the
    // arithmetic.
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const Vec3 point = grid.node_position(i, j, k);
                const std::uint64_t at = (static_cast<std::uint64_t>(i) * grid.ny + j) * grid.nz + k;
                REQUIRE(gfield::get_component(field, at, 0) == -2.0 * amplitude * point.y);
                REQUIRE(gfield::get_component(field, at, 1) == -2.0 * amplitude * point.x);
                // `z` is untouched: the potential is defined in the equatorial plane, and a field that grew with
                // |z| would be one with a divergence nobody asked for.
                REQUIRE(gfield::get_component(field, at, 2) == 0.0);
            }
        }
    }

    // **Between the nodes too**, which is the claim the exactness rests on: a linear field's trilinear blend is
    // the field. Sampled at a point that is deliberately not a node in any axis.
    //
    // The tolerance here is **not** the same as at the nodes, and the difference is the honest part: at a node the
    // value *is* the closed form's two multiplications, while between nodes the blend evaluates `a(1-f) + bf`,
    // which rounds. So the claim is a relative error of a few ulps rather than equality -- and stating it as
    // equality was this case's first version, which the machine refused. A tolerance of 1e-12 is still eight
    // orders tighter than the interpolation error a *non*-linear field would show here, so it separates "the
    // model is reproduced" from "the model is approximated" without pretending the arithmetic is exact.
    const Vec3 between = Vec3{1.7 * kEarthRadiusM, -2.3 * kEarthRadiusM, 0.9 * kEarthRadiusM};
    const Vec3 sampled = sample_baked(field, grid.origin_m, grid.spacing_m, between);
    const auto close = [](double got, double want) {
        const double scale = std::max(std::abs(want), 1.0e-300);
        return std::abs(got - want) / scale < 1.0e-12;
    };
    REQUIRE(close(sampled.x, -2.0 * amplitude * between.y));
    REQUIRE(close(sampled.y, -2.0 * amplitude * between.x));
    REQUIRE(sampled.z == 0.0);
    // `|E| = 2 A r` in the plane, which is the observable the default amplitude was chosen by.
    const double radius = std::hypot(between.x, between.y);
    REQUIRE(close(std::hypot(sampled.x, sampled.y), 2.0 * amplitude * radius));

    // **The physics, through the cross product rather than through the numbers.** Against the dipole's southward
    // equatorial field the drift `E x B` must be sunward on the dayside and antisunward on the nightside -- the
    // observed convection pattern, and the reason this node exists. Asserting the numbers would assert the model;
    // this asserts what the model is for.
    //
    // The magnetic field is the **local** dipole value, `B(r) = B_surface / r_re^3`, and the first version of this
    // case used the surface value at every radius. That is a factor of twenty-seven at three earth radii, and it
    // showed up as a drift speed of 1.93 m/s where the arithmetic said 52 -- which is the whole value of asserting
    // a speed rather than only a sign.
    const auto dipole_equator_t = [](const Vec3& point) {
        const double r_re = std::hypot(point.x, point.y) / kEarthRadiusM;
        return Vec3{0.0, 0.0, -(kEquatorialSurfaceFieldT / (r_re * r_re * r_re))};
    };
    const auto drift_x = [&](const Vec3& point) {
        const Vec3 e = sample_baked(field, grid.origin_m, grid.spacing_m, point);
        const Vec3 b = dipole_equator_t(point);
        const Vec3 drift = cross(e, b);
        return drift.x / norm2(b);   // the sign is what matters here; the speed is checked below
    };
    // `+x` is sunward: the same convention the region mask's `dayside` uses.
    REQUIRE(drift_x(Vec3{2.0 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.0}) > 0.0);
    REQUIRE(drift_x(Vec3{-2.0 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.0}) < 0.0);
    // And it is the `E x B` speed, `|E| / |B|` at that radius.
    const Vec3 dusk = Vec3{0.0, 3.0 * kEarthRadiusM, 0.0};
    const Vec3 e_dusk = sample_baked(field, grid.origin_m, grid.spacing_m, dusk);
    const Vec3 b_dusk = dipole_equator_t(dusk);
    const double speed = std::sqrt(norm2(cross(e_dusk, b_dusk))) / norm2(b_dusk);
    REQUIRE(close(speed, norm(e_dusk) / norm(b_dusk)));
    // 0.2 mV/m at ten earth radii is the order the default amplitude was chosen for; at three the drift is
    // `|E|/|B| = 5.7e-5 / 1.1e-6` = **52 m/s**, which is slow for a reason worth knowing: in this model `|E|`
    // grows with `r` while the dipole's `|B|` falls as `r^3`, so the drift speed goes as `r^4` and convection is a
    // phenomenon of the outer magnetosphere. It is also why the inner region corotates in the real thing.
    REQUIRE(close(norm(e_dusk), 2.0 * amplitude * 3.0 * kEarthRadiusM));
    REQUIRE(speed > 20.0);
    REQUIRE(speed < 200.0);

    // The type is declared like the others and allowed only where a bake is.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[6].type_name == FieldNodes::kConvectionType);
    REQUIRE(types[6].has_compute);
    REQUIRE(types[6].allow_in_field_domain);
    REQUIRE_FALSE(types[6].allow_in_particle_domain);
    REQUIRE(types[6].find_port(FieldNodes::kPortConvectionA, false) != nullptr);
    REQUIRE(types[6].find_port(FieldNodes::kPortField, true) != nullptr);
    REQUIRE(types[6].find_port(FieldNodes::kPortField, true)->type == qp::ports::kVectorField);

    // A grid that cannot be baked, and a non-finite amplitude, are refused rather than approximated.
    const GridSpec too_small{Vec3{}, Vec3{1.0, 1.0, 1.0}, 1, 1, 1};
    REQUIRE_FALSE(bake_convection(amplitude, too_small, key, fields));
    REQUIRE_FALSE(bake_convection(std::numeric_limits<double>::quiet_NaN(), grid, key, fields));
}

TEST_CASE("magnetosphere.field_nodes.corotation_is_the_rotation_the_field_allows", "[magnetosphere]") {
    // **The second E-field model, and the one that makes the first mean something.** In the real magnetosphere
    // convection and corotation compete, and the radius where they balance is the plasmapause; a graph with only
    // the convection field shows a magnetosphere that never rotates.
    //
    // `E = -(Omega x r) x B` has one property that can be checked without trusting anything: `|E| / |B|` is the
    // **rigid rotation speed** `Omega r`. That is what "corotating" means, so the case asserts it directly -- and
    // it asserts it as a speed in metres per second, because a number a textbook quotes is a number a reader can
    // disagree with.
    const GridSpec grid{Vec3{-10.0 * kEarthRadiusM, -10.0 * kEarthRadiusM, -10.0 * kEarthRadiusM},
                        Vec3{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM},
                        41, 41, 41};
    const double tilt = 0.0;
    gfield::FieldSet fields;
    const gfield::FieldKey dipole_key{11, FieldNodes::kPortField};
    const gfield::FieldKey corotation_key{12, FieldNodes::kPortCorotationOut};
    REQUIRE(bake_dipole(tilt, kDipoleMomentAm2, grid, dipole_key, fields));
    const gfield::FieldValue dipole = fields.view(dipole_key);
    REQUIRE(bake_corotation(dipole, grid.origin_m, grid.spacing_m, grid, corotation_key, fields));
    const gfield::FieldValue corotation = fields.view(corotation_key);
    REQUIRE(gfield::is_readable(corotation));
    REQUIRE(corotation.is_vector());
    REQUIRE(corotation.desc.dimension.T == -3);
    REQUIRE(corotation.desc.dimension.I == -1);

    const auto close = [](double got, double want) {
        const double scale = std::max(std::abs(want), 1.0e-300);
        return std::abs(got - want) / scale < 1.0e-9;
    };
    const auto sample = [&](const gfield::FieldValue& table, const Vec3& point) {
        return sample_baked(table, grid.origin_m, grid.spacing_m, point);
    };

    // The equatorial plane at four radii: `E` radial, `|E|/|B| = Omega r`, and the drift purely azimuthal.
    for (const double r_re : {2.0, 4.0, 6.0, 9.0}) {
        const Vec3 point{r_re * kEarthRadiusM, 0.0, 0.0};
        const Vec3 b = sample(dipole, point);
        const Vec3 e = sample(corotation, point);
        REQUIRE(norm2(b) > 0.0);
        // Radial: `E` is along `+x` at this point, which is outward.
        REQUIRE(e.x > 0.0);
        REQUIRE(close(e.y, 0.0));
        REQUIRE(close(e.z, 0.0));
        // **The rigid rotation speed**, which is the definition of corotation rather than a consequence of it.
        const double speed = norm(e) / norm(b);
        REQUIRE(close(speed, kEarthRotationRateSI * r_re * kEarthRadiusM));
        // And the drift it produces is azimuthal: `E x B` has no radial component in the equatorial plane.
        const Vec3 drift = cross(e, b);
        REQUIRE(close(dot(drift, Vec3{1.0, 0.0, 0.0}), 0.0));
    }
    // At four earth radii that speed is **1.86 km/s** -- the number a course quotes for the outer plasmasphere,
    // and it falls out of `Omega r` with no free parameter anywhere in this node.
    const double speed_at_four =
        norm(sample(corotation, Vec3{4.0 * kEarthRadiusM, 0.0, 0.0})) /
        norm(sample(dipole, Vec3{4.0 * kEarthRadiusM, 0.0, 0.0}));
    REQUIRE(speed_at_four > 1500.0);
    REQUIRE(speed_at_four < 2200.0);

    // **Where the two fields balance.** `|E_corot| = Omega B0 / r_re^2` (in earth radii) and `|E_conv| = 2 A r`,
    // so the crossing solves `r^3 = Omega B0 / (2A)`. Both models are baked here and the crossing is **measured**
    // by scanning, then compared with the algebra -- two independent models meeting where theory says, which is
    // the only kind of agreement worth asserting.
    const double amplitude = FieldNodes::kDefaultConvectionA;
    const gfield::FieldKey convection_key{13, FieldNodes::kPortField};
    REQUIRE(bake_convection(amplitude, grid, convection_key, fields));
    const gfield::FieldValue convection = fields.view(convection_key);
    const auto ratio_at = [&](double r_re) {
        const Vec3 point{r_re * kEarthRadiusM, 0.0, 0.0};
        const double corot = norm(sample(corotation, point));
        const double conv = norm(sample(convection, point));
        return conv / corot;
    };
    // Inside the balance radius corotation wins (ratio < 1), outside convection does.
    REQUIRE(ratio_at(2.0) < 1.0);
    REQUIRE(ratio_at(20.0) > 1.0);
    // Bisection, so the measured crossing is a number rather than a guess at which sample to read.
    double low = 2.0;
    double high = 20.0;
    for (int step = 0; step < 60; ++step) {
        const double mid = 0.5 * (low + high);
        if (ratio_at(mid) < 1.0) {
            low = mid;
        } else {
            high = mid;
        }
    }
    const double measured = 0.5 * (low + high);
    const double predicted =
        std::cbrt(kEarthRotationRateSI * kEquatorialSurfaceFieldT / (2.0 * amplitude));
    REQUIRE(std::abs(measured - predicted) / predicted < 0.02);
    // With this kit's default amplitude that radius is 8.97 earth radii -- inside the dipole's ten, so the picture
    // a graph of these two nodes draws has a corotating core and a convecting outer region.
    REQUIRE(predicted > 8.0);
    REQUIRE(predicted < 10.0);

    // A field that is zero where it is sampled gives a zero electric field rather than a refusal: no field to
    // corotate in is no corotation, and that is the model's own answer.
    const gfield::FieldKey zero_key{14, FieldNodes::kPortField};
    REQUIRE(bake_uniform(Vec3{0.0, 0.0, 0.0}, grid, zero_key, fields, tesla_dimension()));
    const gfield::FieldKey zero_corotation{15, FieldNodes::kPortCorotationOut};
    REQUIRE(bake_corotation(fields.view(zero_key), grid.origin_m, grid.spacing_m, grid, zero_corotation, fields));
    const gfield::FieldValue nothing = fields.view(zero_corotation);
    REQUIRE(gfield::is_readable(nothing));
    for (std::uint64_t point = 0; point < nothing.point_count(); ++point) {
        REQUIRE(gfield::get_component(nothing, point, 0) == 0.0);
    }
    // And an unreadable input, or a grid that cannot be baked, is refused.
    REQUIRE_FALSE(bake_corotation(gfield::FieldValue{}, grid.origin_m, grid.spacing_m, grid, corotation_key, fields));
    const GridSpec too_small{Vec3{}, Vec3{1.0, 1.0, 1.0}, 1, 1, 1};
    REQUIRE_FALSE(bake_corotation(dipole, grid.origin_m, grid.spacing_m, too_small, corotation_key, fields));

    // The type declares one socket and no grid parameters, which is the decision this node makes: it bakes on the
    // lattice of the field it reads, so there is no second grid to disagree with the first.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[7].type_name == FieldNodes::kCorotationType);
    REQUIRE(types[7].has_compute);
    REQUIRE(types[7].inputs.size() == 1);
    REQUIRE(types[7].inputs.front().number == FieldNodes::kPortCorotationMagnetic);
    REQUIRE(types[7].inputs.front().type == qp::ports::kVectorField);
    REQUIRE(types[7].inputs.front().connectable);
    REQUIRE(types[7].inputs.front().required);
}

TEST_CASE("magnetosphere.field_nodes.a_wired_field_reports_the_grid_it_was_baked_on", "[magnetosphere]") {
    // **One resolver, three callers.** `abi::LatticeDesc` carries counts and not positions, so anything that has to
    // sample a published field needs the geometry of the node that baked it -- and there are now three such
    // callers: the plan builder filling a pusher's grid slots, the corotation bake sampling `B`, and any future
    // node that reads a field pointwise.
    //
    // The behaviour this case pins is a **widening**: `plan.cpp` used to do the walk itself and refuse any source
    // that was not a dipole (`grid_unknown`), which was honest and meant a pusher could only ever be wired to a
    // dipole's field. The kit has eight field types now, and six of them declare a grid.
    Scene scene;
    const graph::NodeId uniform = scene.add(FieldNodes::kUniformType);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(uniform, FieldNodes::kPortUniformOrigin0 + axis, -5.0 * kEarthRadiusM);
        scene.set(uniform, FieldNodes::kPortUniformSpacing0 + axis, 0.25 * kEarthRadiusM);
        scene.set(uniform, FieldNodes::kPortUniformCount0 + axis, 41.0);
    }
    const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(uniform, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);

    // The solver the plan builder uses now says which grid it is, rather than refusing.
    GridSpec resolved;
    REQUIRE(resolve_field_origin(scene.g, pusher, PusherNodes::kPortMagnetic, resolved));
    REQUIRE(resolved.nx == 41);
    REQUIRE(resolved.spacing_m.x == 0.25 * kEarthRadiusM);
    REQUIRE(resolved.origin_m.x == -5.0 * kEarthRadiusM);
    // And `resolve_field` -- the plan builder's own entry point -- agrees, which is what makes the widening real
    // rather than a second code path that happens to work.
    REQUIRE(scene.bake().has_value());
    gfield::FieldValue bound;
    GridSpec planned;
    REQUIRE(resolve_field(scene.g, pusher, PusherNodes::kPortMagnetic, scene.fields, bound, planned) ==
            PlanBuildRefusal::ok);
    REQUIRE(gfield::is_readable(bound));
    REQUIRE(planned.nx == resolved.nx);

    // **The walk continues through a node that has no grid of its own.** A multiplier is defined on the lattice its
    // inputs share, so asking *it* for a grid is asking the wrong node: the resolver follows its first socket back
    // to the uniform field, and a caller that stopped at the multiplier would get a refusal it cannot explain.
    const graph::NodeId mask = scene.add(FieldNodes::kMaskType);
    // The mask's own nine grid ports, by offset: origin, then spacing, then counts -- the order its descriptor
    // declares and the order every reader of this kit assumes.
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const double value = offset < 3 ? -5.0 * kEarthRadiusM
                                         : (offset < 6 ? 0.25 * kEarthRadiusM : 41.0);
        scene.set(mask, FieldNodes::kPortMaskOrigin0 + offset, value);
    }
    const graph::NodeId product = scene.add(FieldNodes::kMulType);
    scene.wire(uniform, FieldNodes::kPortField, product, FieldNodes::kPortMulField);
    scene.wire(mask, FieldNodes::kPortWeight, product, FieldNodes::kPortMulWeight);
    const graph::NodeId second_pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(product, FieldNodes::kPortMulOut, second_pusher, PusherNodes::kPortMagnetic);
    GridSpec through_mul;
    REQUIRE(resolve_field_origin(scene.g, second_pusher, PusherNodes::kPortMagnetic, through_mul));
    REQUIRE(through_mul.nx == 41);
    REQUIRE(through_mul.origin_m.x == -5.0 * kEarthRadiusM);

    // **And through the other node that has no grid of its own.** The blend forwards on its *first* socket for
    // the same reason the multiplier does: both inputs must sit on one lattice for the node to be buildable at
    // all, so either socket answers, and the one a reader is looking at is the one the walk follows. The two
    // types were added to the walk one at a time rather than by inventing a rule for each.
    const graph::NodeId sheet = scene.add(FieldNodes::kCurrentSheetType);
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const double value = offset < 3 ? -5.0 * kEarthRadiusM : (offset < 6 ? 0.25 * kEarthRadiusM : 41.0);
        scene.set(sheet, FieldNodes::kPortSheetOrigin0 + offset, value);
    }
    const graph::NodeId blend = scene.add(FieldNodes::kBlendType);
    scene.wire(uniform, FieldNodes::kPortField, blend, FieldNodes::kPortBlendInner);
    scene.wire(sheet, FieldNodes::kPortField, blend, FieldNodes::kPortBlendOuter);
    const graph::NodeId third_pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(blend, FieldNodes::kPortBlendOut, third_pusher, PusherNodes::kPortMagnetic);
    GridSpec through_blend;
    REQUIRE(resolve_field_origin(scene.g, third_pusher, PusherNodes::kPortMagnetic, through_blend));
    REQUIRE(through_blend.nx == 41);
    REQUIRE(through_blend.origin_m.x == -5.0 * kEarthRadiusM);
    // The evaluator path is the third caller and the one that has to build the table: the same graph bakes, and
    // the blend's own output is in the store described as the inner field is. Both nodes declare the same box,
    // which is what the one-lattice rule asks of a blend and what this wiring was arranged to satisfy.
    REQUIRE(scene.bake().has_value());
    const gfield::FieldValue blended = scene.fields.view(gfield::FieldKey{blend.index, FieldNodes::kPortBlendOut});
    REQUIRE(gfield::is_readable(blended));
    REQUIRE(blended.is_vector());
    REQUIRE(blended.desc.count[0] == 41);
    REQUIRE(blended.desc.dimension.M == 1);
    REQUIRE(blended.desc.dimension.T == -2);

    // **And through the one node here that has a grid of its own without baking a model.** The resampler is a grid
    // *source*: a pusher wired to it must be handed the **target** lattice, because that is where its samples are.
    // This is what makes it the answer to every other combinator's refusal -- `sum(resample(a), b)` is how two
    // boxes become one -- and a resolver that walked *past* it to the original source would hand the pusher six
    // numbers describing a lattice the samples are not on.
    const graph::NodeId finer = scene.add(FieldNodes::kResampleType);
    const double target_origin = -2.5 * kEarthRadiusM;
    const double target_spacing = 0.125 * kEarthRadiusM;
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(finer, FieldNodes::kPortResampleOrigin0 + axis, target_origin);
        scene.set(finer, FieldNodes::kPortResampleOrigin0 + 3 + axis, target_spacing);
        scene.set(finer, FieldNodes::kPortResampleOrigin0 + 6 + axis, 41.0);
    }
    scene.wire(uniform, FieldNodes::kPortField, finer, FieldNodes::kPortResampleField);
    const graph::NodeId fourth_pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(finer, FieldNodes::kPortResampleOut, fourth_pusher, PusherNodes::kPortMagnetic);
    GridSpec through_resample;
    REQUIRE(resolve_field_origin(scene.g, fourth_pusher, PusherNodes::kPortMagnetic, through_resample));
    REQUIRE(through_resample.nx == 41);
    REQUIRE(through_resample.origin_m.x == target_origin);
    REQUIRE(through_resample.spacing_m.x == target_spacing);
    // The bake has to run for the store to hold it, and the resampler's own input is the uniform field it was
    // wired to -- the evaluator resolves that socket with the same walk.
    REQUIRE(scene.bake().has_value());
    const gfield::FieldValue resampled =
        scene.fields.view(gfield::FieldKey{finer.index, FieldNodes::kPortResampleOut});
    REQUIRE(gfield::is_readable(resampled));
    REQUIRE(resampled.desc.count[0] == 41);
    REQUIRE(resampled.desc.dimension.T == -2);

    // A socket with nothing wired is not a grid, and neither is a wire to a node that is gone: the resolver
    // answers false so that the caller can make its own refusal rather than sampling a box it invented.
    const graph::NodeId lonely = scene.add(PusherNodes::kBorisType);
    GridSpec nothing;
    REQUIRE_FALSE(resolve_field_origin(scene.g, lonely, PusherNodes::kPortMagnetic, nothing));
    const graph::Graph empty;
    REQUIRE_FALSE(resolve_field_origin(empty, graph::NodeId{}, PusherNodes::kPortMagnetic, nothing));
}

TEST_CASE("magnetosphere.plan.two_sockets_on_two_lattices_are_refused", "[magnetosphere]") {
    // **The kernel has one grid, and until this check nothing said so.** Its parameter block carries six numbers --
    // an origin and a spacing -- because that is what a per-sub-step sampler can afford, and the plan fills them
    // from the **magnetic** slot. Every other bound table is then sampled at those coordinates, so a graph that
    // wired a drag field baked on a different lattice got a particle dragged by the wrong cell's rate: a plausible
    // number from the wrong place, which no case of either half could catch.
    //
    // The check is new because the possibility is: until this kit could publish a scalar field there was no second
    // kind of table to wire into a second socket.
    Scene scene;
    const graph::NodeId dipole = scene.add(FieldNodes::kDipoleType);
    scene.set(dipole, FieldNodes::kPortTiltDegrees, 0.0);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(dipole, FieldNodes::kPortOrigin0 + axis, -8.0 * kEarthRadiusM);
        scene.set(dipole, FieldNodes::kPortSpacing0 + axis, 0.5 * kEarthRadiusM);
        scene.set(dipole, FieldNodes::kPortCount0 + axis, 33.0);
    }

    // A mask as the drag source -- it is a 0/1 rate, which is a crude atmosphere and a legal one -- and the two
    // lattices **deliberately** differ in one number: a quarter of an earth radius against the dipole's half.
    const graph::NodeId mask = scene.add(FieldNodes::kMaskType);
    const auto set_mask_grid = [&scene, mask](double spacing_re) {
        for (graph::PortNumber offset = 0; offset < 9; ++offset) {
            const double value = offset < 3 ? -8.0 * kEarthRadiusM
                                             : (offset < 6 ? spacing_re * kEarthRadiusM : 33.0);
            scene.set(mask, FieldNodes::kPortMaskOrigin0 + offset, value);
        }
    };
    set_mask_grid(0.25);

    const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(dipole, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);
    scene.wire(mask, FieldNodes::kPortWeight, pusher, PusherNodes::kPortDrag);

    REQUIRE(scene.bake().has_value());
    BuiltPlan plan;
    REQUIRE(build_particle_plan(scene.g, scene.order(), scene.fields, plan) == PlanBuildRefusal::grid_mismatch);
    // Named, so that a caller can say what to fix rather than "the plan was rejected".
    REQUIRE(std::string{to_string(PlanBuildRefusal::grid_mismatch)} == "grid_mismatch");
    REQUIRE(plan.steps.empty());

    // And it is the **lattices** that were refused, not the wiring: the same graph with the mask on the dipole's
    // own grid plans cleanly, drag slot and all.
    set_mask_grid(0.5);
    REQUIRE(scene.bake().has_value());
    BuiltPlan agreeing;
    REQUIRE(build_particle_plan(scene.g, scene.order(), scene.fields, agreeing) == PlanBuildRefusal::ok);
    REQUIRE(agreeing.steps.size() == 1);
    REQUIRE(agreeing.pushers == 1);
    // Both slots are bound, so the case above was about a real graph rather than about a socket that was never
    // wired: the drag slot is what makes the mismatch possible in the first place.
    const pp::StepPlan& step = agreeing.steps.front();
    // **Read through the accessor, not through an index.** `fields[]` is indexed by the enumerator's own value
    // (0, 1, 2) while `BatchView::in` -- what a *kernel* indexes -- puts the same fields at `slot_index(name)`,
    // which is 4, 5 and 6. This case's first version used the second form against the first array and read eleven
    // bytes past its end; the accessor exists because of that, and using it here is the point of having it.
    REQUIRE(gfield::is_readable(step.field(pp::SlotName::magnetic)));
    REQUIRE(gfield::is_readable(step.field(pp::SlotName::drag)));
    REQUIRE(step.field(pp::SlotName::magnetic).is_vector());
    REQUIRE_FALSE(step.field(pp::SlotName::drag).is_vector());
    // The two index spaces, asserted as the fact that made the accessor necessary rather than only as prose.
    REQUIRE(pp::slot_index(pp::SlotName::magnetic) != static_cast<std::size_t>(pp::SlotName::magnetic));
    REQUIRE(static_cast<std::size_t>(pp::SlotName::magnetic) == 0);
}

TEST_CASE("magnetosphere.field_nodes.an_atmosphere_thins_the_way_an_exponential_does", "[magnetosphere]") {
    // The kit's second scalar producer and its **first dissipative force**. The shape is checked before the physics:
    // a table whose values are wrong in a way a decay curve would hide -- the reference radius misplaced, or the
    // scale height inverted -- still produces something that looks like an atmosphere.
    const GridSpec grid{Vec3{-4.0 * kEarthRadiusM, -4.0 * kEarthRadiusM, -4.0 * kEarthRadiusM},
                        Vec3{0.25 * kEarthRadiusM, 0.25 * kEarthRadiusM, 0.25 * kEarthRadiusM},
                        33, 33, 33};
    gfield::FieldSet fields;
    const gfield::FieldKey key{31, FieldNodes::kPortAtmosphereOut};

    FieldNodes::AtmosphereSpec spec;
    spec.nu0_per_s = 0.02;
    spec.scale_height_m = 0.5 * kEarthRadiusM;
    spec.reference_m = kEarthRadiusM;
    REQUIRE(bake_atmosphere(spec, grid, key, fields));
    const gfield::FieldValue rates = fields.view(key);
    REQUIRE(gfield::is_readable(rates));
    REQUIRE_FALSE(rates.is_vector());
    // Per second, not dimensionless: the two have the same layout, so the dimension is the only thing that tells a
    // drag rate from a weight -- and a consumer that confused them would multiply a velocity by 0 or 1.
    REQUIRE(rates.desc.dimension.T == -1);
    REQUIRE(rates.desc.dimension.M == 0);

    // Every node, against the closed form: the rate at the reference radius is exactly `nu0`, one scale height up
    // it is `nu0/e`, and the value is the same at every node of the same radius -- which is what "spherically
    // symmetric" has to mean for a table on a Cartesian lattice.
    std::uint64_t checked = 0;
    for (std::uint64_t point = 0; point < rates.point_count(); ++point) {
        // The grid's node positions are recoverable from the table's own layout, so the expected value is computed
        // from the same indices the bake used rather than sampled from the result.
        const std::uint64_t nx = grid.nx;
        const std::uint64_t ny = grid.ny;
        const std::uint64_t i = point / (ny * grid.nz);
        const std::uint64_t j = (point / grid.nz) % ny;
        const std::uint64_t k = point % grid.nz;
        const Vec3 position = grid.node_position(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j),
                                                 static_cast<std::uint32_t>(k));
        const double radius = norm(position);
        const double expected = spec.nu0_per_s * std::exp(-(radius - spec.reference_m) / spec.scale_height_m);
        REQUIRE(gfield::get_component(rates, point, 0) == expected);
        REQUIRE(nx * ny * grid.nz == rates.point_count());
        ++checked;
    }
    REQUIRE(checked == rates.point_count());

    // The two anchor values, read from the table rather than from the formula, so that a bake which ignored one of
    // the three parameters fails here even if its own arithmetic is self-consistent.
    const auto rate_at_radius = [&](double radius_m) {
        // The equator along +x, so the radius is the coordinate and the index arithmetic is one division.
        const double x_re = radius_m / kEarthRadiusM;
        const double origin_re = grid.origin_m.x / kEarthRadiusM;
        const double spacing_re = grid.spacing_m.x / kEarthRadiusM;
        const auto i = static_cast<std::uint64_t>(std::lround((x_re - origin_re) / spacing_re));
        const std::uint64_t at = (i * grid.ny + grid.ny / 2) * grid.nz + grid.nz / 2;
        return gfield::get_component(rates, at, 0);
    };
    REQUIRE(std::abs(rate_at_radius(kEarthRadiusM) - spec.nu0_per_s) < 1.0e-15);
    REQUIRE(std::abs(rate_at_radius(1.5 * kEarthRadiusM) - spec.nu0_per_s / 2.718281828459045) < 1.0e-15);
    REQUIRE(rate_at_radius(2.0 * kEarthRadiusM) < rate_at_radius(1.5 * kEarthRadiusM));
    REQUIRE(rate_at_radius(1.0 * kEarthRadiusM) > rate_at_radius(1.5 * kEarthRadiusM));

    // A scale height far larger than the region is a **nearly** uniform atmosphere, and the word "nearly" is
    // measurable rather than rhetorical: the extremes of the table span exactly `exp(-(r_max - r_min)/H)`, and the
    // first version of this assertion demanded that the *corner* equal `nu0` -- which it does not, 3.7% down, since
    // the corner is seven earth radii from the reference radius even when H is a million kilometres. What a
    // closed-form decay check needs is the span, so the span is what is asserted.
    FieldNodes::AtmosphereSpec flat = spec;
    flat.scale_height_m = 1.0e10;
    const gfield::FieldKey flat_key{32, FieldNodes::kPortAtmosphereOut};
    REQUIRE(bake_atmosphere(flat, grid, flat_key, fields));
    const gfield::FieldValue flat_rates = fields.view(flat_key);
    double lowest = std::numeric_limits<double>::max();
    double highest = std::numeric_limits<double>::lowest();
    for (std::uint64_t point = 0; point < flat_rates.point_count(); ++point) {
        const double rate = gfield::get_component(flat_rates, point, 0);
        lowest = std::min(lowest, rate);
        highest = std::max(highest, rate);
    }
    const double corner_m = std::sqrt(3.0) * 4.0 * kEarthRadiusM;
    const double expected_span = std::exp(-(corner_m - 0.0) / flat.scale_height_m);
    REQUIRE(std::abs(highest / lowest - 1.0 / expected_span) / (1.0 / expected_span) < 1.0e-9);
    // Under a percent across the whole box, which is what makes this the uniform limit a decay check can use.
    REQUIRE(highest / lowest < 1.01);
    REQUIRE(lowest > 0.0);

    // Refusals: a rate that would add energy, a scale height of zero (an atmosphere that is a wall), and a grid
    // that cannot be baked. Clamping any of the first two would hide a sign error behind a plausible decay.
    FieldNodes::AtmosphereSpec negative = spec;
    negative.nu0_per_s = -0.01;
    REQUIRE_FALSE(bake_atmosphere(negative, grid, key, fields));
    FieldNodes::AtmosphereSpec wall = spec;
    wall.scale_height_m = 0.0;
    REQUIRE_FALSE(bake_atmosphere(wall, grid, key, fields));
    FieldNodes::AtmosphereSpec nan = spec;
    nan.nu0_per_s = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(bake_atmosphere(nan, grid, key, fields));
    const GridSpec too_small{Vec3{}, Vec3{1.0, 1.0, 1.0}, 1, 1, 1};
    REQUIRE_FALSE(bake_atmosphere(spec, too_small, key, fields));

    // And the type is declared with its own grid ports, three parameters first -- the same shape the mask has.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[8].type_name == FieldNodes::kAtmosphereType);
    REQUIRE(types[8].has_compute);
    REQUIRE(types[8].allow_in_field_domain);
    REQUIRE_FALSE(types[8].allow_in_particle_domain);
    const graph::PortDesc* drag = types[8].find_port(FieldNodes::kPortAtmosphereOut, true);
    REQUIRE(drag != nullptr);
    REQUIRE(drag->type == qp::ports::kScalarField);
    REQUIRE(drag->unit_symbol == std::string{"1/s"});
    for (const graph::PortNumber number : {FieldNodes::kPortAtmosphereNu0, FieldNodes::kPortAtmosphereScaleHeight,
                                           FieldNodes::kPortAtmosphereReference}) {
        const graph::PortDesc* parameter = types[8].find_port(number, false);
        REQUIRE(parameter != nullptr);
        REQUIRE_FALSE(parameter->connectable);
    }
}

TEST_CASE("magnetosphere.field_nodes.a_current_sheet_carries_the_current_it_implies", "[magnetosphere]") {
    // The tail's analytic model: `B = (B0 tanh(z/L), 0, 0)`. Three properties make it the model a course writes
    // down -- the reversal across the plane, the saturation away from it, and **exactly zero in the plane** -- and
    // a fourth is what turns it from arithmetic into physics: Ampere's law says what current the field implies, and
    // that current can be measured from the baked table by differentiating it.
    const GridSpec grid{Vec3{-2.0 * kEarthRadiusM, -2.0 * kEarthRadiusM, -8.0 * kEarthRadiusM},
                        Vec3{0.1 * kEarthRadiusM, 0.1 * kEarthRadiusM, 0.025 * kEarthRadiusM},
                        41, 41, 641};
    FieldNodes::SheetSpec spec;
    spec.b0_tesla = 5.0e-9;
    spec.half_thickness_m = 2.0 * kEarthRadiusM;
    gfield::FieldSet fields;
    const gfield::FieldKey key{41, FieldNodes::kPortField};
    REQUIRE(bake_current_sheet(spec, grid, key, fields));
    const gfield::FieldValue sheet = fields.view(key);
    REQUIRE(gfield::is_readable(sheet));
    REQUIRE(sheet.is_vector());
    REQUIRE(sheet.desc.dimension.M == 1);
    REQUIRE(sheet.desc.dimension.T == -2);

    // Every node, against the closed form, and the two other components **exactly zero**: a sheet is
    // one-dimensional, and a `B_y` that came out as 1e-30 would be a number in the table that is not the model.
    for (std::uint64_t point = 0; point < sheet.point_count(); ++point) {
        const std::uint64_t k = point % grid.nz;
        const double z = grid.origin_m.z + static_cast<double>(k) * grid.spacing_m.z;
        REQUIRE(gfield::get_component(sheet, point, 0) == spec.b0_tesla * std::tanh(z / spec.half_thickness_m));
        REQUIRE(gfield::get_component(sheet, point, 1) == 0.0);
        REQUIRE(gfield::get_component(sheet, point, 2) == 0.0);
    }

    // The three properties, read back from the table rather than recomputed: zero in the plane, opposite signs
    // either side of it, and saturation at the lobe value several half-thicknesses away.
    const auto bx_at_z = [&](double z_m) {
        const auto k = static_cast<std::uint64_t>(std::lround((z_m - grid.origin_m.z) / grid.spacing_m.z));
        const std::uint64_t at = (0 * grid.ny + grid.ny / 2) * grid.nz + k;
        return gfield::get_component(sheet, at, 0);
    };
    REQUIRE(bx_at_z(1.0 * kEarthRadiusM) > 0.0);
    REQUIRE(bx_at_z(-1.0 * kEarthRadiusM) < 0.0);
    // Odd symmetry, to a few parts in a billion rather than exactly: `tanh` is odd and the bake is one
    // multiplication, but the nodes at `+z` and `-z` are **not** exactly symmetric about the plane -- the spacing is
    // not a binary fraction of an earth radius, so `origin + k*spacing` and `origin + (n-k)*spacing` differ in their
    // last bits. The same rounding is why this case's first version failed on `bx_at_z(0.0) == 0.0`: the node
    // nearest the plane is a hundred-millionth of a metre *from* it, and `tanh` there is not zero but 8e-16.
    REQUIRE(std::abs(bx_at_z(1.0 * kEarthRadiusM) + bx_at_z(-1.0 * kEarthRadiusM)) /
                std::abs(bx_at_z(1.0 * kEarthRadiusM)) < 1.0e-9);
    // **Zero in the plane is a property of the continuum**, and the honest way to assert it on a lattice is to ask
    // the sampler for the plane itself: the trilinear blend of two nodes that are nearly equal and opposite is zero
    // to within what a double can say about it. And the bound the model gives at any node -- `|tanh u| <= |u|` -- is
    // asserted at the node nearest the plane, which is a statement about the model rather than about the spacing.
    const Vec3 in_the_plane = sample_baked(sheet, grid.origin_m, grid.spacing_m, Vec3{0.0, 0.0, 0.0});
    REQUIRE(std::abs(in_the_plane.x) < 1.0e-15 * spec.b0_tesla);
    const double nearest_node_z = grid.origin_m.z + static_cast<double>(grid.nz / 2) * grid.spacing_m.z;
    REQUIRE(std::abs(bx_at_z(nearest_node_z)) <=
            spec.b0_tesla * (std::abs(nearest_node_z) + grid.spacing_m.z) / spec.half_thickness_m);
    // `tanh(1) = 0.7616` at one half-thickness, and within a percent of the lobe value at three.
    REQUIRE(std::abs(bx_at_z(2.0 * kEarthRadiusM) - spec.b0_tesla * 0.7615941559557649) < 1.0e-20);
    REQUIRE(std::abs(bx_at_z(6.0 * kEarthRadiusM) - spec.b0_tesla) / spec.b0_tesla < 0.01);

    // **The current the field implies**, measured by differentiating the table: `J_y = -(1/mu0) dB_x/dz`, which for
    // this model is `-(B0/(mu0 L)) sech^2(z/L)` -- concentrated in the sheet, and carrying `B0/mu0` amperes per
    // metre in total. The measured centre value is compared against the closed form where the central difference's
    // truncation error is smallest.
    const double mu0 = 4.0 * 3.14159265358979323846 * kMu0OverFourPi;
    const double dz = grid.spacing_m.z;
    const double derivative = (bx_at_z(dz) - bx_at_z(-dz)) / (2.0 * dz);
    const double measured_jy = -derivative / mu0;
    const double predicted_jy = -spec.b0_tesla / (mu0 * spec.half_thickness_m);
    REQUIRE(std::abs(measured_jy - predicted_jy) / std::abs(predicted_jy) < 0.01);

    // The **total** sheet current, `integral J_y dz`, which must be `-2 B0/mu0` however the current is distributed:
    // the field swings from `-B0` to `+B0`, and `integral J_y dz = -(1/mu0) [B_x]` across the sheet is that whole
    // swing over `mu0`. The factor of two is the one this assertion's first version got wrong -- it predicted
    // `-B0/mu0` and the measurement came back at 1.9987 times that, which is the arithmetic being right and the
    // expectation being wrong. It holds for any scale height, so a bake that got `L` wrong but kept the shape still
    // fails here.
    double total_current = 0.0;
    for (std::uint64_t k = 0; k < grid.nz; ++k) {
        const double z = grid.origin_m.z + static_cast<double>(k) * grid.spacing_m.z;
        const double plus = std::tanh((z + dz) / spec.half_thickness_m);
        const double minus = std::tanh((z - dz) / spec.half_thickness_m);
        total_current += -(spec.b0_tesla * (plus - minus) / (2.0 * dz)) / mu0 * dz;
    }
    const double predicted_total = -2.0 * spec.b0_tesla / mu0;
    REQUIRE(std::abs(total_current - predicted_total) / std::abs(predicted_total) < 0.01);
    // Amperes per metre: five nanotesla over mu0 is four milliamperes per metre, and the reversal doubles it -- the
    // number a textbook gives for a quiet tail's cross-tail current.
    REQUIRE(std::abs(predicted_total) > 1.0e-3);
    REQUIRE(std::abs(predicted_total) < 1.0e-2);

    // **And the composition it exists for**: adding the sheet to a dipole is the classic cross-section, and the sum
    // is exactly the two fields added -- checked at a tailward point where both are present, which is where a
    // composition that quietly dropped one of them would show.
    const gfield::FieldKey dipole_key{42, FieldNodes::kPortField};
    REQUIRE(bake_dipole(0.0, kDipoleMomentAm2, grid, dipole_key, fields));
    const gfield::FieldKey combined{43, FieldNodes::kPortField};
    REQUIRE(bake_sum(fields.view(dipole_key), sheet, combined, fields));
    const gfield::FieldValue sum = fields.view(combined);
    const auto point_at = [&](double x_m, double z_m) {
        const auto i = static_cast<std::uint64_t>(std::lround((x_m - grid.origin_m.x) / grid.spacing_m.x));
        const auto k = static_cast<std::uint64_t>(std::lround((z_m - grid.origin_m.z) / grid.spacing_m.z));
        return (i * grid.ny + grid.ny / 2) * grid.nz + k;
    };
    const std::uint64_t tail = point_at(-1.5 * kEarthRadiusM, 0.0);
    const std::uint64_t lobe = point_at(-1.5 * kEarthRadiusM, 6.0 * kEarthRadiusM);
    for (std::uint64_t component = 0; component < 3; ++component) {
        REQUIRE(gfield::get_component(sum, lobe, component) ==
                gfield::get_component(fields.view(dipole_key), lobe, component) +
                    gfield::get_component(sheet, lobe, component));
    }
    // In the neutral plane the sheet's own field is exactly zero, so the sum's `x` component there is the dipole's
    // -- which at a tailward point on the equator is small but not zero, and the assertion says which of the two it
    // is rather than asserting a zero that only the sheet would give.
    // The sheet's own field in the plane is zero, and the node nearest the plane is **not in it**: the spacing is
    // not a binary fraction of an earth radius, so that node sits about three nanometres off the plane and holds
    // `tanh(z/L)` there -- measured as `-0x1.c3e7596b2bd7ap-80`, which is -1.5e-24 tesla. Catch2 prints that as
    // "-0.0" and the first version of this line asserted `== 0.0`, so it failed while *looking* like an
    // impossibility. The bound the model gives is what belongs here: `|tanh u| <= |u|`, and the node is within one
    // spacing of the plane.
    const double sheet_x_at_tail = gfield::get_component(sheet, tail, 0);
    REQUIRE(std::abs(sheet_x_at_tail) <= spec.b0_tesla * grid.spacing_m.z / spec.half_thickness_m);
    // In the neutral plane the sheet contributes nothing to any physical scale, so the sum's `x` there is the
    // dipole's -- asserted with the same model bound as above rather than as an equality, because "nothing" is
    // 1.5e-24 tesla on this lattice rather than zero.
    REQUIRE(std::abs(gfield::get_component(sum, tail, 0) - gfield::get_component(fields.view(dipole_key), tail, 0)) <=
            spec.b0_tesla * grid.spacing_m.z / spec.half_thickness_m);
    REQUIRE(gfield::get_component(sum, tail, 2) < 0.0);
    // **Relative sizes, measured rather than assumed.** At 1.5 earth radii downwind and 6 up, the dipole's `x`
    // component is 8.9e-8 T and the sheet's is 5e-9: the planet still wins by eighteen, and that is the honest
    // physics of a five-nanotesla tail -- it takes over tens of earth radii out, which this box (two radii either
    // way) deliberately does not reach, because the point of the node is the *sheet*, not a picture of the tail.
    // The line this replaced claimed the sheet dominated here by a hundred, a number nobody had measured.
    const double sheet_lobe = gfield::get_component(sheet, lobe, 0);
    const double dipole_lobe_x = gfield::get_component(fields.view(dipole_key), lobe, 0);
    REQUIRE(std::abs(sheet_lobe - spec.b0_tesla) / spec.b0_tesla < 0.01);
    REQUIRE(std::abs(dipole_lobe_x) > 5.0 * std::abs(sheet_lobe));

    // Refusals: a sheet of zero thickness is a discontinuity rather than a sheet, and a grid that cannot be baked.
    FieldNodes::SheetSpec knife = spec;
    knife.half_thickness_m = 0.0;
    REQUIRE_FALSE(bake_current_sheet(knife, grid, key, fields));
    FieldNodes::SheetSpec nan = spec;
    nan.b0_tesla = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(bake_current_sheet(nan, grid, key, fields));
    const GridSpec too_small{Vec3{}, Vec3{1.0, 1.0, 1.0}, 1, 1, 1};
    REQUIRE_FALSE(bake_current_sheet(spec, too_small, key, fields));

    // The type declares its own grid ports after its two parameters, and publishes tesla.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[9].type_name == FieldNodes::kCurrentSheetType);
    REQUIRE(types[9].has_compute);
    REQUIRE(types[9].allow_in_field_domain);
    REQUIRE_FALSE(types[9].allow_in_particle_domain);
    const graph::PortDesc* out = types[9].find_port(FieldNodes::kPortField, true);
    REQUIRE(out != nullptr);
    REQUIRE(out->type == qp::ports::kVectorField);
    REQUIRE(out->unit_symbol == std::string{"T"});
    REQUIRE(types[9].find_port(FieldNodes::kPortSheetOrigin0, false) != nullptr);
    REQUIRE_FALSE(types[9].find_port(FieldNodes::kPortSheetOrigin0, false)->connectable);
}

TEST_CASE("magnetosphere.field_nodes.a_resample_moves_the_samples_and_adds_no_information",
          "[magnetosphere]") {
    // **The node every other combinator's refusal points at.** A sum, a product and a blend are all defined on the
    // lattice their inputs share and refuse two -- which is honest and, without this node, a dead end. `resample`
    // is the explicit step that puts a field on the lattice the next node wants, and it is the one node here whose
    // whole job is to change where the samples are.
    //
    // What it costs is measured rather than hidden: resampling onto a **finer** grid adds no information, and the
    // case measures what a dipole actually loses and how that loss falls. Second order in the source spacing is
    // the claim `baked_field.hpp` makes about the interpolant, so a factor of four per halving is the prediction.
    const double re = kEarthRadiusM;
    const gfield::FieldKey source_key{31, FieldNodes::kPortField};
    const gfield::FieldKey same_key{32, FieldNodes::kPortResampleOut};
    const gfield::FieldKey moved_key{33, FieldNodes::kPortResampleOut};
    gfield::FieldSet fields;

    // A **uniform** field first, because it is the one case with an exact answer: trilinear interpolation
    // reproduces a constant, so a resample of one is that constant on the new lattice -- at every node, and not
    // only at the nodes the two boxes happen to share.
    const GridSpec uniform_grid{Vec3{-4.0 * re, -4.0 * re, -4.0 * re},
                                Vec3{0.5 * re, 0.5 * re, 0.5 * re}, 17, 17, 17};
    const Vec3 uniform_value{1.0e-6, -2.0e-6, 3.0e-6};
    REQUIRE(bake_uniform(uniform_value, uniform_grid, source_key, fields, tesla_dimension()));
    const gfield::FieldValue uniform_source = fields.view(source_key);

    // Onto **its own** lattice: the resample must be the identity, and exactly the identity rather than nearly --
    // a node that moved a field onto the lattice it was already on would otherwise be allowed to edit it.
    REQUIRE(bake_resample(uniform_source, uniform_grid, uniform_grid, same_key, fields));
    const gfield::FieldValue unchanged = fields.view(same_key);
    REQUIRE(gfield::is_readable(unchanged));
    REQUIRE(unchanged.point_count() == uniform_source.point_count());
    REQUIRE(unchanged.desc.dimension.M == 1);
    REQUIRE(unchanged.desc.dimension.T == -2);
    for (std::uint64_t point = 0; point < unchanged.point_count(); ++point) {
        for (std::uint64_t component = 0; component < 3; ++component) {
            REQUIRE(gfield::get_component(unchanged, point, component) ==
                    gfield::get_component(uniform_source, point, component));
        }
    }
    // And onto a **different** lattice -- offset by a value no spacing divides, so no target node coincides with a
    // source node and the answer comes from the blend rather than from a lookup.
    const GridSpec shifted{Vec3{-3.97 * re, -1.03 * re, -0.11 * re}, Vec3{0.3 * re, 0.3 * re, 0.3 * re}, 9, 9, 9};
    REQUIRE(bake_resample(uniform_source, uniform_grid, shifted, moved_key, fields));
    const gfield::FieldValue moved = fields.view(moved_key);
    REQUIRE(gfield::is_readable(moved));
    REQUIRE(moved.desc.count[0] == 9);
    for (std::uint64_t point = 0; point < moved.point_count(); ++point) {
        REQUIRE(relative_to(gfield::get_component(moved, point, 0), uniform_value.x) < 1.0e-15);
        REQUIRE(relative_to(gfield::get_component(moved, point, 1), uniform_value.y) < 1.0e-15);
        REQUIRE(relative_to(gfield::get_component(moved, point, 2), uniform_value.z) < 1.0e-15);
    }

    // **What a resample of a dipole loses, and how it falls.** One fixed target and five source spacings, halving
    // each time: the error is the source table's interpolation error, so it must fall by about four per halving
    // once the spacing is small against the field's own scale. The target sits **out at three and a half earth
    // radii and does not contain the origin**: the first version of this experiment spanned a box with the dipole
    // at its centre, where the field diverges, so the RMS was dominated by the nodes nearest the singularity and
    // the ratios stalled at 1.9 and 1.1 instead of converging. The assertions below caught the experiment rather
    // than the code, which is what a convergence study is for.
    const std::size_t steps = 5;
    const GridSpec target{Vec3{2.0 * re, 2.0 * re, 2.0 * re}, Vec3{0.02 * re, 0.02 * re, 0.02 * re}, 50, 50, 50};
    const DipoleField dipole{0.0, kDipoleMomentAm2};
    double rms[steps] = {0.0, 0.0, 0.0, 0.0, 0.0};
    double worst[steps] = {0.0, 0.0, 0.0, 0.0, 0.0};
    const double spacings[steps] = {1.0, 0.5, 0.25, 0.125, 0.0625};
    for (std::size_t step = 0; step < steps; ++step) {
        const double h = spacings[step] * re;
        // The source's counts, from a three-earth-radius box and the spacing: 4, 7, 13, 25 and 49 nodes an axis,
        // the whole box at `r >= sqrt(3)` earth radii so that no sample sits near the dipole's centre.
        const auto nodes = static_cast<std::uint32_t>(3.0 / spacings[step]) + 1;
        const GridSpec box{Vec3{1.0 * re, 1.0 * re, 1.0 * re}, Vec3{h, h, h}, nodes, nodes, nodes};
        const gfield::FieldKey box_key{40 + static_cast<graph::PortNumber>(step), FieldNodes::kPortField};
        REQUIRE(bake_dipole(0.0, kDipoleMomentAm2, box, box_key, fields));
        const gfield::FieldKey moved_dipole_key{50 + static_cast<graph::PortNumber>(step),
                                                FieldNodes::kPortResampleOut};
        REQUIRE(bake_resample(fields.view(box_key), box, target, moved_dipole_key, fields));
        const gfield::FieldValue resampled = fields.view(moved_dipole_key);
        REQUIRE(gfield::is_readable(resampled));
        double sum = 0.0;
        for (std::uint32_t i = 0; i < target.nx; ++i) {
            for (std::uint32_t j = 0; j < target.ny; ++j) {
                for (std::uint32_t k = 0; k < target.nz; ++k) {
                    const Vec3 point = target.node_position(i, j, k);
                    const Vec3 exact = dipole.at(point);
                    const std::uint64_t at = (static_cast<std::uint64_t>(i) * target.ny + j) * target.nz + k;
                    double gap = 0.0;
                    for (std::uint64_t component = 0; component < 3; ++component) {
                        const double got = gfield::get_component(resampled, at, component);
                        const double want = component == 0 ? exact.x : (component == 1 ? exact.y : exact.z);
                        const double difference = std::abs(got - want);
                        if (difference > gap) gap = difference;
                    }
                    const double relative = gap / norm(exact);
                    sum += relative * relative;
                    if (relative > worst[step]) worst[step] = relative;
                }
            }
        }
        rms[step] = std::sqrt(sum / static_cast<double>(resampled.point_count()));
        REQUIRE(worst[step] > 0.0);
    }
    CAPTURE(rms[0], rms[1], rms[2], rms[3], rms[4], worst[0], worst[1], worst[2], worst[3], worst[4]);
    // Second order, seen the way a convergence study sees it: each halving of the source spacing divides the error
    // by between 3.2 and 4.6, and the ratios come **down to four from above** -- 4.33, 4.08, 4.02, 4.00 as the
    // spacing falls from one earth radius to a sixteenth of one. A first-order scheme would sit at two, a
    // fourth-order one at sixteen, and a resample that returned the nearest node would not fall at all.
    for (std::size_t step = 0; step + 1 < steps; ++step) {
        const double ratio = rms[step] / rms[step + 1];
        REQUIRE(ratio > 3.2);
        REQUIRE(ratio < 4.6);
    }
    REQUIRE(relative_to(rms[3] / rms[4], 4.0) < 0.05);
    // The worst node's relative error, which is the number a course would quote for a table: six percent at one
    // earth radius and a quarter of a thousandth at a sixteenth of one. Both are measured, and both are the
    // reason a resample is worth doing at all -- what it keeps is what the coarse table had, and no more.
    REQUIRE(worst[0] < 0.08);
    REQUIRE(worst[4] < 4.0e-4);

    // Refusals. A target that reaches outside the source is refused rather than clamped: the sampler clamps for a
    // particle that leaves the modelled box, and a *static* resample that clamped would fill a slab of the target
    // with the boundary value and call it a field.
    const GridSpec bigger{Vec3{-7.0 * re, -4.0 * re, -4.0 * re}, Vec3{0.5 * re, 0.5 * re, 0.5 * re}, 17, 17, 17};
    REQUIRE_FALSE(bake_resample(uniform_source, uniform_grid, bigger, moved_key, fields));
    // One node further along `x` at the same spacing: the box is the source's plus half an earth radius.
    const GridSpec one_past{Vec3{-4.0 * re, -4.0 * re, -4.0 * re}, Vec3{0.5 * re, 0.5 * re, 0.5 * re}, 18, 17, 17};
    REQUIRE_FALSE(bake_resample(uniform_source, uniform_grid, one_past, moved_key, fields));
    // A source grid that disagrees with the table it is supposed to describe: a plausible field from the wrong
    // place, which is what the check is for.
    const GridSpec wrong_source{Vec3{-4.0 * re, -4.0 * re, -4.0 * re}, Vec3{0.5 * re, 0.5 * re, 0.5 * re}, 16, 17,
                                17};
    REQUIRE_FALSE(bake_resample(uniform_source, wrong_source, uniform_grid, moved_key, fields));
    // A scalar where a vector belongs, an unreadable socket, and a target with no interior.
    const gfield::FieldKey weight_key{34, FieldNodes::kPortWeight};
    FieldNodes::MaskSpec mask;
    REQUIRE(bake_mask(mask, uniform_grid, weight_key, fields));
    REQUIRE_FALSE(bake_resample(fields.view(weight_key), uniform_grid, uniform_grid, moved_key, fields));
    REQUIRE_FALSE(bake_resample(gfield::FieldValue{}, uniform_grid, uniform_grid, moved_key, fields));
    const GridSpec no_interior{Vec3{}, Vec3{re, re, re}, 1, 1, 1};
    REQUIRE_FALSE(bake_resample(uniform_source, uniform_grid, no_interior, moved_key, fields));

    // The declaration: its own numbers, nine grid ports that are typed rather than wired, and a grid of its own --
    // which is what makes it the node a pusher can be pointed at when the field it wants is on another lattice.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[11].type_name == FieldNodes::kResampleType);
    REQUIRE(types[11].has_compute);
    REQUIRE(types[11].allow_in_field_domain);
    REQUIRE_FALSE(types[11].allow_in_particle_domain);
    const graph::PortDesc* source_port = types[11].find_port(FieldNodes::kPortResampleField, false);
    REQUIRE(source_port != nullptr);
    REQUIRE(source_port->type == qp::ports::kVectorField);
    REQUIRE(source_port->required);
    const graph::PortDesc* resampled_out = types[11].find_port(FieldNodes::kPortResampleOut, true);
    REQUIRE(resampled_out != nullptr);
    REQUIRE(resampled_out->type == qp::ports::kVectorField);
    REQUIRE(resampled_out->unit_symbol == std::string{"T"});
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const graph::PortDesc* port = types[11].find_port(FieldNodes::kPortResampleOrigin0 + offset, false);
        REQUIRE(port != nullptr);
        REQUIRE_FALSE(port->connectable);
    }
}

TEST_CASE("magnetosphere.field_nodes.the_magnetopause_is_a_surface_with_a_nose", "[magnetosphere]") {
    // **The node `field.mask` said would come.** That case argues why a mask must stay an either/or region test --
    // a mask with intermediate values would be a claim about a boundary it does not model -- and its own comment
    // names the boundary that needs the smooth version: the magnetopause, "arriving as a node of its own". This is
    // it: Shue's surface, `r_mp(theta) = r0 (2 / (1 + cos theta))^alpha`, as a weight that is one inside and zero
    // outside, which is the orientation the mask uses and therefore the one a wire reads the same way.
    const double re = kEarthRadiusM;
    gfield::FieldSet fields;

    // **Two points where the closed form is exact, and the grid is built to land on them.** The nose is at `r0`
    // because the exponent multiplies a factor of one; the flank is at `2^alpha r0`. With `alpha = 1` and a standoff
    // of eight earth radii those are eight on the sunward axis and sixteen on the flank, so a lattice of whole earth
    // radii puts a node on each -- and the weight there is `1 / (1 + exp(0))`, which is one half **exactly** rather
    // than to a tolerance.
    const FieldNodes::MagnetopauseSpec exact_spec{8.0 * re, 1.0, 1.0 * re};
    const GridSpec coarse{Vec3{-16.0 * re, -16.0 * re, -16.0 * re}, Vec3{re, re, re}, 33, 33, 33};
    const gfield::FieldKey weight_key{61, FieldNodes::kPortWeight};
    REQUIRE(bake_magnetopause(exact_spec, coarse, weight_key, fields));
    const gfield::FieldValue weight = fields.view(weight_key);
    REQUIRE(gfield::is_readable(weight));
    // **A scalar**, and the shape of the table is asserted before anything is read out of it: a consumer that
    // assumed three components would read other nodes' weights and call them its own.
    REQUIRE(weight.is_scalar());
    REQUIRE_FALSE(weight.is_vector());
    REQUIRE(weight.desc.component == qp::abi::ComponentKind::scalar);
    REQUIRE(weight.point_count() == 33ull * 33ull * 33ull);
    REQUIRE(weight.desc.dimension.L == 0);
    REQUIRE(weight.desc.dimension.M == 0);

    const auto at_node = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        return gfield::get_component(weight, (static_cast<std::uint64_t>(i) * coarse.ny + j) * coarse.nz + k, 0);
    };
    // The nose: node (24, 16, 16) is `(+8, 0, 0)` in earth radii. The flank: node (16, 32, 16) is `(0, +16, 0)`.
    REQUIRE(at_node(24, 16, 16) == 0.5);
    REQUIRE(at_node(16, 32, 16) == 0.5);
    // And the two sides of each: inside is greater than a half, outside is less, which is the orientation.
    REQUIRE(at_node(23, 16, 16) > 0.5);
    REQUIRE(at_node(25, 16, 16) < 0.5);
    REQUIRE(at_node(16, 31, 16) > 0.5);
    REQUIRE(at_node(16, 33, 16) < 0.5);
    // Downwind the surface opens without bound, so the whole tail side is inside: the weight is one there, which is
    // what a paraboloid says and why the tail needs the current sheet rather than this node to be closed.
    REQUIRE(at_node(0, 16, 16) > 0.99);
    // Far outside it is zero, and every weight is in `[0, 1]`: a weight of 1.0000001 would scale a field up.
    for (std::uint64_t point = 0; point < weight.point_count(); ++point) {
        const double w = gfield::get_component(weight, point, 0);
        REQUIRE(w >= 0.0);
        REQUIRE(w <= 1.0);
    }

    // **The rest of the surface is found rather than sampled.** A fine slab through the equatorial plane, and a
    // bisection along each direction for the radius where the baked weight crosses a half -- which is the surface,
    // because along a ray the weight is a function of `r` alone. The comparison is against the closed form, so this
    // tests the whole shape rather than the values at nodes somebody chose.
    const GridSpec slab{Vec3{-25.0 * re, -25.0 * re, -0.2 * re}, Vec3{0.1 * re, 0.1 * re, 0.1 * re}, 501, 501, 5};
    const FieldNodes::MagnetopauseSpec default_spec;
    const gfield::FieldKey fine_key{62, FieldNodes::kPortWeight};
    REQUIRE(bake_magnetopause(default_spec, slab, fine_key, fields));
    const gfield::FieldValue fine = fields.view(fine_key);
    REQUIRE(gfield::is_readable(fine));
    const double degrees[5] = {0.0, 30.0, 60.0, 90.0, 105.0};
    for (double degree : degrees) {
        const double theta = degree * 3.14159265358979323846 / 180.0;
        const Vec3 direction{std::cos(theta), std::sin(theta), 0.0};
        const double closed_form =
            FieldNodes::kDefaultMagnetopauseStandoffRe *
            std::pow(2.0 / (1.0 + std::cos(theta)), FieldNodes::kDefaultMagnetopauseFlaring);
        // Six widths on either side of the surface the closed form predicts, so the bisection has a bracket rather
        // than an assumption, and so that "far" means far in the units of this node's own transition.
        REQUIRE(sample_baked_scalar(fine, slab.origin_m, slab.spacing_m, direction * (0.5 * re)) > 0.99);
        REQUIRE(sample_baked_scalar(fine, slab.origin_m, slab.spacing_m,
                                    direction * ((closed_form + 6.0) * re)) < 0.01);
        double low = 0.5 * re;
        double high = 24.5 * re;
        for (int step = 0; step < 80; ++step) {
            const double middle = 0.5 * (low + high);
            if (sample_baked_scalar(fine, slab.origin_m, slab.spacing_m, direction * middle) > 0.5) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const double found = 0.5 * (low + high) / re;
        CAPTURE(degree, found, closed_form);
        // Measured: the surface is located to between a millionth and three millionths of its own radius -- the
        // table's interpolation error and nothing else, since the weight is monotone along a ray. The tolerance is
        // a hundredth of a percent, thirty times the measurement, because it is a statement about the model being
        // reproduced rather than about where one machine rounds.
        REQUIRE(relative_to(found, closed_form) < 1.0e-4);
    }

    // **And the composition it exists for**: the dipole multiplied by the boundary, which is the smooth version of
    // what a mask does to it. Where the weight is essentially zero the field is essentially gone, and where it is
    // essentially one the field is untouched -- both counted, so neither statement can pass by being empty.
    const gfield::FieldKey dipole_key{63, FieldNodes::kPortField};
    const gfield::FieldKey shielded_key{64, FieldNodes::kPortMulOut};
    REQUIRE(bake_dipole(0.0, kDipoleMomentAm2, coarse, dipole_key, fields));
    REQUIRE(bake_scaled(fields.view(dipole_key), weight, shielded_key, fields));
    const gfield::FieldValue dipole = fields.view(dipole_key);
    const gfield::FieldValue shielded = fields.view(shielded_key);
    REQUIRE(gfield::is_readable(shielded));
    std::uint64_t cut = 0;
    std::uint64_t kept = 0;
    for (std::uint64_t point = 0; point < shielded.point_count(); ++point) {
        const double w = gfield::get_component(weight, point, 0);
        for (std::uint64_t component = 0; component < 3; ++component) {
            const double field_value = gfield::get_component(dipole, point, component);
            const double product = gfield::get_component(shielded, point, component);
            // The product is the field times the weight, component by component, which is what makes the boundary a
            // weight rather than a switch: nothing here rounds, clips or renormalises.
            REQUIRE(product == field_value * w);
        }
        if (w < 0.01) ++cut;
        if (w > 0.99) ++kept;
    }
    // Both regions exist in this box, so neither statement above can pass by being empty -- and the two counts are
    // the assertion that the boundary really does divide it.
    REQUIRE(cut > 0);
    REQUIRE(kept > 0);

    // Refusals: a standoff of zero is not a boundary, a negative flaring would shrink the surface away from the
    // nose, a zero width is a discontinuity, and a non-finite number is not a parameter. Each is refused rather
    // than clamped -- the internal singularities are the formula's own and are clamped, but these are the user's.
    FieldNodes::MagnetopauseSpec zero_standoff = default_spec;
    zero_standoff.standoff_m = 0.0;
    REQUIRE_FALSE(bake_magnetopause(zero_standoff, slab, fine_key, fields));
    FieldNodes::MagnetopauseSpec negative_standoff = default_spec;
    negative_standoff.standoff_m = -5.0 * re;
    REQUIRE_FALSE(bake_magnetopause(negative_standoff, slab, fine_key, fields));
    FieldNodes::MagnetopauseSpec negative_flaring = default_spec;
    negative_flaring.flaring = -0.1;
    REQUIRE_FALSE(bake_magnetopause(negative_flaring, slab, fine_key, fields));
    FieldNodes::MagnetopauseSpec zero_width = default_spec;
    zero_width.width_m = 0.0;
    REQUIRE_FALSE(bake_magnetopause(zero_width, slab, fine_key, fields));
    FieldNodes::MagnetopauseSpec not_a_number = default_spec;
    not_a_number.standoff_m = std::nan("");
    REQUIRE_FALSE(bake_magnetopause(not_a_number, slab, fine_key, fields));
    const GridSpec no_interior{Vec3{}, Vec3{re, re, re}, 1, 1, 1};
    REQUIRE_FALSE(bake_magnetopause(default_spec, no_interior, fine_key, fields));

    // The reader is the one the evaluator uses: a node carrying only the flaring exponent gets the defaults for the
    // other two, which is the state a freshly placed node is in.
    qp::graph::Node node;
    node.type_name = FieldNodes::kMagnetopauseType;
    node.set_param(FieldNodes::kPortMagnetopauseFlaring, qp::ports::Value{0.7});
    const FieldNodes::MagnetopauseSpec read = FieldNodes::read_magnetopause(node);
    REQUIRE(read.flaring == 0.7);
    REQUIRE(read.standoff_m == FieldNodes::kDefaultMagnetopauseStandoffRe * kEarthRadiusM);
    REQUIRE(read.width_m == FieldNodes::kDefaultMagnetopauseWidthM);

    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[12].type_name == FieldNodes::kMagnetopauseType);
    REQUIRE(types[12].has_compute);
    REQUIRE(types[12].allow_in_field_domain);
    REQUIRE_FALSE(types[12].allow_in_particle_domain);
    const graph::PortDesc* boundary = types[12].find_port(FieldNodes::kPortWeight, true);
    REQUIRE(boundary != nullptr);
    REQUIRE(boundary->type == qp::ports::kScalarField);
    REQUIRE(boundary->unit_symbol == std::string{"1"});
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const graph::PortDesc* port = types[12].find_port(FieldNodes::kPortMagnetopauseOrigin0 + offset, false);
        REQUIRE(port != nullptr);
        REQUIRE_FALSE(port->connectable);
    }
}

TEST_CASE("magnetosphere.field_nodes.a_mix_blends_the_potentials_not_the_fields", "[magnetosphere]") {
    // **The second blend, and the reason there are two.** `field.blend` interpolates along `x` with a sigmoid it
    // owns, so it owns the derivative too and its correction is exact. This node takes a weight **table** -- a
    // mask's, a magnetopause's, anything that publishes a scalar field -- and pays for that freedom with a
    // differentiated gradient. The construction is the same idea one layer up: blend the **vector potentials**
    // rather than the fields, and the result is a curl, hence exactly divergence-free whatever the weight is.
    //
    // The case is built so the first half has an exact answer. Two **uniform** fields whose difference is along the
    // weight's own gradient, and a weight that is **linear** in `x`: a linear function's central difference is
    // exact, and a uniform difference field's potential `A = (B x r) / 2` is genuine, so every number below is
    // closed-form -- including the divergence, which the correction must take from `dw/dx * dB_x` to nothing.
    const double re = kEarthRadiusM;
    const GridSpec grid{Vec3{-4.0 * re, -4.0 * re, -4.0 * re}, Vec3{0.25 * re, 0.25 * re, 0.25 * re}, 33, 33, 33};
    gfield::FieldSet fields;
    const double first_t = 2.0e-8;
    const double second_t = 6.0e-8;
    const double difference_t = second_t - first_t;
    const gfield::FieldKey first_key{71, FieldNodes::kPortMixA};
    const gfield::FieldKey second_key{72, FieldNodes::kPortMixB};
    REQUIRE(bake_uniform(Vec3{first_t, 0.0, 0.0}, grid, first_key, fields, tesla_dimension()));
    REQUIRE(bake_uniform(Vec3{second_t, 0.0, 0.0}, grid, second_key, fields, tesla_dimension()));

    const double weight_slope = 1.0 / (8.0 * re);
    const gfield::FieldKey linear_weight_key{73, FieldNodes::kPortMixWeight};
    std::vector<double> linear(static_cast<std::size_t>(grid.point_count()), 0.0);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        const double x = grid.origin_m.x + static_cast<double>(i) * grid.spacing_m.x;
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) linear[at++] = 0.5 + weight_slope * x;
        }
    }
    // A **scalar** table, built by the case rather than by a node: the weight's provenance is the experiment here,
    // and a linear shape is the one whose difference is exact.
    REQUIRE(fields.publish(linear_weight_key,
                           qp::abi::make_lattice(qp::abi::LatticeKind::volume, qp::abi::ComponentKind::scalar,
                                                 qp::abi::ElementType::f64, qp::abi::kDimensionless, grid.nx,
                                                 grid.ny, grid.nz),
                           std::move(linear)));
    const gfield::FieldValue weight = fields.view(linear_weight_key);
    REQUIRE(gfield::is_readable(weight));
    REQUIRE(weight.is_scalar());

    const gfield::FieldKey straight_key{74, FieldNodes::kPortMixOut};
    const gfield::FieldKey corrected_key{75, FieldNodes::kPortMixOut};
    REQUIRE(bake_mix(fields.view(first_key), fields.view(second_key), weight, grid, 0.0, straight_key, fields));
    REQUIRE(bake_mix(fields.view(first_key), fields.view(second_key), weight, grid, 1.0, corrected_key, fields));
    const gfield::FieldValue straight = fields.view(straight_key);
    const gfield::FieldValue corrected = fields.view(corrected_key);
    REQUIRE(gfield::is_readable(straight));
    REQUIRE(gfield::is_readable(corrected));
    // Tesla, described from the first socket, on the lattice the inputs share.
    REQUIRE(corrected.desc.dimension.M == 1);
    REQUIRE(corrected.desc.dimension.T == -2);
    REQUIRE(corrected.point_count() == grid.point_count());

    // Every node against the closed form. The `x` row is the convex combination and nothing else (the potential's
    // term has no `x` component here); the `y` row is exactly zero, because a poloidal potential has one component
    // and `grad w` has none in `y`; and the `z` row is the correction alone, `g dpsi` with `dpsi` the integral of
    // `-dB_x` from the anchor -- which is `-g dB_x (z - z_min)`, the anchor showing up as the gauge offset
    // `field.blend` documents.
    const double g = weight_slope * difference_t;
    const double anchor_z = grid.origin_m.z;
    for (std::uint32_t i = 0; i < grid.nx; ++i) {
        const double x = grid.origin_m.x + static_cast<double>(i) * grid.spacing_m.x;
        const double w = 0.5 + weight_slope * x;
        for (std::uint32_t j = 0; j < grid.ny; ++j) {
            for (std::uint32_t k = 0; k < grid.nz; ++k) {
                const double z = grid.origin_m.z + static_cast<double>(k) * grid.spacing_m.z;
                const std::uint64_t point = (static_cast<std::uint64_t>(i) * grid.ny + j) * grid.nz + k;
                REQUIRE(gfield::get_component(corrected, point, 0) == first_t * (1.0 - w) + second_t * w);
                REQUIRE(gfield::get_component(corrected, point, 1) == 0.0);
                REQUIRE(relative_to(gfield::get_component(corrected, point, 2), -g * (z - anchor_z)) < 1.0e-14);
                // And with the correction switched off the same table is the straight blend: the two differ by the
                // potential's term and by nothing else.
                REQUIRE(gfield::get_component(straight, point, 0) == first_t * (1.0 - w) + second_t * w);
                REQUIRE(gfield::get_component(straight, point, 1) == 0.0);
                REQUIRE(gfield::get_component(straight, point, 2) == 0.0);
            }
        }
    }

    // **The measurement the whole construction exists for.** The straight blend's divergence is the closed form
    // `dw/dx * dB_x`; the corrected one is the difference operator's own floor. Both are measured with the same
    // central differences the field's case uses.
    const double straight_divergence = worst_divergence(straight, grid.spacing_m);
    const double corrected_divergence = worst_divergence(corrected, grid.spacing_m);
    const double predicted = std::abs(g);
    CAPTURE(straight_divergence, corrected_divergence, predicted);
    // The straight blend's divergence is the closed form `dw/dx * dB_x` to six parts in a million -- the weight is
    // linear, so the difference that produced it is exact and what is left is the order the arithmetic ran in. The
    // corrected one is **fourteen orders of magnitude** smaller, which is not a tolerance but the difference
    // operator's own floor: this is the regime where the construction is not an approximation at all.
    REQUIRE(relative_to(straight_divergence, predicted) < 1.0e-6);
    REQUIRE(corrected_divergence * 1.0e10 < straight_divergence);

    // **The regime where the potential is only an approximation, measured rather than claimed.** A dipole on one
    // socket and a uniform field on the other, mixed by the magnetopause's own weight: `A = (B x r) / 2` is not a
    // potential of a dipole, so the correction reduces the divergence instead of removing it, and the factor is
    // reported here rather than asserted as if it were exact. The box **starts three earth radii out**: a box with
    // the dipole at its centre has a node on the singularity, and the first version of this measurement read a
    // divergence of 3e22 T/m because of it -- the same lesson the resample's convergence study learned, in a
    // different costume.
    const GridSpec wide{Vec3{3.0 * re, -12.0 * re, -12.0 * re}, Vec3{0.4 * re, 0.4 * re, 0.4 * re}, 48, 61, 61};
    const gfield::FieldKey dipole_key{76, FieldNodes::kPortMixA};
    const gfield::FieldKey sheath_key{77, FieldNodes::kPortMixB};
    const gfield::FieldKey boundary_key{78, FieldNodes::kPortMixWeight};
    REQUIRE(bake_dipole(0.0, kDipoleMomentAm2, wide, dipole_key, fields));
    // The outer field is along `x` on purpose: the source layer a straight blend leaves is `grad w . (b - a)`, so a
    // field whose difference is **orthogonal** to the weight's gradient everywhere measures nothing. The first
    // version of this half used an axial outer field, and the naive divergence came out at the difference
    // operator's own floor -- a measurement of the experiment rather than of the node.
    REQUIRE(bake_uniform(Vec3{5.0e-9, 0.0, 0.0}, wide, sheath_key, fields, tesla_dimension()));
    REQUIRE(bake_magnetopause(FieldNodes::MagnetopauseSpec{}, wide, boundary_key, fields));
    const gfield::FieldKey naive_mix_key{79, FieldNodes::kPortMixOut};
    const gfield::FieldKey corrected_mix_key{80, FieldNodes::kPortMixOut};
    REQUIRE(bake_mix(fields.view(dipole_key), fields.view(sheath_key), fields.view(boundary_key), wide, 0.0,
                     naive_mix_key, fields));
    REQUIRE(bake_mix(fields.view(dipole_key), fields.view(sheath_key), fields.view(boundary_key), wide, 1.0,
                     corrected_mix_key, fields));
    const double naive_divergence = worst_divergence(fields.view(naive_mix_key), wide.spacing_m);
    const double reduced_divergence = worst_divergence(fields.view(corrected_mix_key), wide.spacing_m);
    CAPTURE(naive_divergence, reduced_divergence);
    // Measured: 5.3 times smaller with the correction in force, against 0.74 -- that is, **worse** -- for the
    // reference's `(B x r) / 2` potential on the same pair. The factor is modest because a dipole has a `y`
    // structure off the plane this is measured on, so the poloidal potential is not exact here either; what the
    // number is for is to show which side of one the correction is on, and the case keeps ten percent of room
    // because it is a measurement rather than a model constant.
    REQUIRE(reduced_divergence < naive_divergence);
    REQUIRE(reduced_divergence * 3.0 < naive_divergence);

    // **And where the potential is genuine.** A Harris sheet against a uniform field: both vary in `x` and `z` only,
    // which is exactly the family a poloidal potential describes, so here the construction is not an approximation
    // and the correction takes the divergence down to the difference operator's floor. This pair is also **not** a
    // uniform difference, which is the other regime the reference's potential needs and this one does not.
    const gfield::FieldKey sheet_key{87, FieldNodes::kPortMixA};
    const gfield::FieldKey uniform_key{88, FieldNodes::kPortMixB};
    FieldNodes::SheetSpec sheet;
    REQUIRE(bake_current_sheet(sheet, wide, sheet_key, fields));
    REQUIRE(bake_uniform(Vec3{0.0, 0.0, 1.0e-8}, wide, uniform_key, fields, tesla_dimension()));
    const gfield::FieldKey poloidal_naive_key{89, FieldNodes::kPortMixOut};
    const gfield::FieldKey poloidal_corrected_key{90, FieldNodes::kPortMixOut};
    REQUIRE(bake_mix(fields.view(sheet_key), fields.view(uniform_key), fields.view(boundary_key), wide, 0.0,
                     poloidal_naive_key, fields));
    REQUIRE(bake_mix(fields.view(sheet_key), fields.view(uniform_key), fields.view(boundary_key), wide, 1.0,
                     poloidal_corrected_key, fields));
    const double poloidal_naive = worst_divergence(fields.view(poloidal_naive_key), wide.spacing_m);
    const double poloidal_corrected = worst_divergence(fields.view(poloidal_corrected_key), wide.spacing_m);
    // Measured: 38 times smaller. Not to the floor, and the reason is stated rather than tuned away -- the weight's
    // gradient is a **difference** of the logistic table (`O(h^2 w''')`, about three percent at this spacing), and
    // the correction can only cancel the source layer as well as that difference knows it. The family is what is
    // being asserted: on inputs the potential describes, the correction removes the layer instead of doubling it.
    REQUIRE(poloidal_corrected * 20.0 < poloidal_naive);

    // A weight that is **constant** switches the boundary off entirely, and then the mix is exactly one of its
    // inputs -- with no correction at all, because a constant's gradient is zero. Both ends are asserted exactly:
    // a blend that left a trace of the other field where its weight says it does not exist would be a blend whose
    // ends mean something else.
    const gfield::FieldKey zero_key{81, FieldNodes::kPortMixWeight};
    const gfield::FieldKey one_key{82, FieldNodes::kPortMixWeight};
    std::vector<double> zeros(static_cast<std::size_t>(wide.point_count()), 0.0);
    std::vector<double> ones(static_cast<std::size_t>(wide.point_count()), 1.0);
    const qp::abi::LatticeDesc scalar_desc =
        qp::abi::make_lattice(qp::abi::LatticeKind::volume, qp::abi::ComponentKind::scalar,
                              qp::abi::ElementType::f64, qp::abi::kDimensionless, wide.nx, wide.ny, wide.nz);
    REQUIRE(fields.publish(zero_key, scalar_desc, std::move(zeros)));
    REQUIRE(fields.publish(one_key, scalar_desc, std::move(ones)));
    const gfield::FieldKey only_first_key{83, FieldNodes::kPortMixOut};
    const gfield::FieldKey only_second_key{84, FieldNodes::kPortMixOut};
    REQUIRE(bake_mix(fields.view(dipole_key), fields.view(sheath_key), fields.view(zero_key), wide, 1.0,
                     only_first_key, fields));
    REQUIRE(bake_mix(fields.view(dipole_key), fields.view(sheath_key), fields.view(one_key), wide, 1.0,
                     only_second_key, fields));
    for (std::uint64_t point = 0; point < wide.point_count(); ++point) {
        for (std::uint64_t component = 0; component < 3; ++component) {
            REQUIRE(gfield::get_component(fields.view(only_first_key), point, component) ==
                    gfield::get_component(fields.view(dipole_key), point, component));
            REQUIRE(gfield::get_component(fields.view(only_second_key), point, component) ==
                    gfield::get_component(fields.view(sheath_key), point, component));
        }
    }

    // Refusals: a vector where the weight belongs and a scalar where a field belongs (the port types catch the
    // first of those at connection time and the bake catches it too), a third lattice, two dimensions, a grid that
    // disagrees with the tables, and a correction outside `[0, 1]`.
    REQUIRE_FALSE(bake_mix(fields.view(first_key), fields.view(second_key), fields.view(first_key), grid, 1.0,
                           corrected_key, fields));
    REQUIRE_FALSE(bake_mix(weight, fields.view(second_key), fields.view(linear_weight_key), grid, 1.0,
                           corrected_key, fields));
    const GridSpec other{Vec3{-4.0 * re, -4.0 * re, -4.0 * re}, Vec3{0.5 * re, 0.5 * re, 0.5 * re}, 17, 17, 17};
    const gfield::FieldKey coarse_key{85, FieldNodes::kPortMixB};
    REQUIRE(bake_uniform(Vec3{second_t, 0.0, 0.0}, other, coarse_key, fields, tesla_dimension()));
    REQUIRE_FALSE(bake_mix(fields.view(first_key), fields.view(coarse_key), weight, grid, 1.0, corrected_key,
                           fields));
    REQUIRE_FALSE(bake_mix(fields.view(first_key), fields.view(second_key), weight, other, 1.0, corrected_key,
                           fields));
    const gfield::FieldKey volts_key{86, FieldNodes::kPortMixB};
    REQUIRE(bake_uniform(Vec3{0.0, 1.0, 0.0}, grid, volts_key, fields, volt_per_metre_dimension()));
    REQUIRE_FALSE(bake_mix(fields.view(first_key), fields.view(volts_key), weight, grid, 1.0, corrected_key,
                           fields));
    REQUIRE_FALSE(bake_mix(fields.view(first_key), fields.view(second_key), weight, grid, 1.5, corrected_key,
                           fields));
    REQUIRE_FALSE(bake_mix(fields.view(first_key), fields.view(second_key), weight, grid, -0.1, corrected_key,
                           fields));
    REQUIRE_FALSE(bake_mix(gfield::FieldValue{}, fields.view(second_key), weight, grid, 1.0, corrected_key, fields));

    // The declaration: its own numbers, a scalar weight socket, and the correction typed in rather than wired.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[13].type_name == FieldNodes::kMixType);
    REQUIRE(types[13].has_compute);
    REQUIRE(types[13].allow_in_field_domain);
    REQUIRE_FALSE(types[13].allow_in_particle_domain);
    REQUIRE(types[13].find_port(FieldNodes::kPortMixA, false)->type == qp::ports::kVectorField);
    REQUIRE(types[13].find_port(FieldNodes::kPortMixB, false)->type == qp::ports::kVectorField);
    REQUIRE(types[13].find_port(FieldNodes::kPortMixWeight, false)->type == qp::ports::kScalarField);
    REQUIRE_FALSE(types[13].find_port(FieldNodes::kPortMixCorrection, false)->connectable);
    const graph::PortDesc* mixed_out = types[13].find_port(FieldNodes::kPortMixOut, true);
    REQUIRE(mixed_out != nullptr);
    REQUIRE(mixed_out->unit_symbol == std::string{"T"});
}

TEST_CASE("magnetosphere.source.the_date_sets_the_dipole_tilt", "[magnetosphere]") {
    // **The second driver, and the second consumer of the optional-socket shape.** The reference's `day_source`
    // publishes the magnetic tilt a date implies: the axis leans 23.44 degrees and the magnetic axis is offset from
    // it, so the angle the dipole makes with the Sun runs from minus twelve to plus thirty-four degrees over a year.
    // Three days are exact, and they are what the case asserts first.
    REQUIRE(relative_to(SourceNodes::tilt_degrees_for_day(172.0), 23.44 + 11.0) < 1.0e-15);
    REQUIRE(relative_to(SourceNodes::tilt_degrees_for_day(172.0 + 182.625), 11.0 - 23.44) < 1.0e-12);
    REQUIRE(relative_to(SourceNodes::tilt_degrees_for_day(172.0 - 91.3125), 11.0) < 1.0e-12);
    // The range, and its direction: the solstices are the extremes and everything between them is monotone.
    double previous = 1000.0;
    for (double day = 172.0; day <= 172.0 + 180.0; day += 5.0) {
        const double tilt = SourceNodes::tilt_degrees_for_day(day);
        REQUIRE(tilt < previous);
        previous = tilt;
    }
    REQUIRE(SourceNodes::tilt_degrees_for_day(0.0) <= 11.0 + 23.44 + 1.0e-12);
    REQUIRE(SourceNodes::tilt_degrees_for_day(365.0) >= 11.0 - 23.44 - 1.0e-12);

    // Out of range is clamped, not wrapped: a node showing day 400 keeps showing it, and the tilt stays finite.
    REQUIRE(SourceNodes::tilt_degrees_for_day(400.0) == SourceNodes::tilt_degrees_for_day(365.0));
    REQUIRE(SourceNodes::tilt_degrees_for_day(-10.0) == SourceNodes::tilt_degrees_for_day(0.0));
    qp::graph::Node fresh;
    fresh.type_name = SourceNodes::kDayType;
    REQUIRE(SourceNodes::read_day(fresh) == SourceNodes::kDefaultDay);
    fresh.set_param(SourceNodes::kPortDay, qp::ports::Value{200.0});
    REQUIRE(SourceNodes::read_day(fresh) == 200.0);
    fresh.set_param(SourceNodes::kPortDay, qp::ports::Value{400.0});
    REQUIRE(SourceNodes::read_day(fresh) == SourceNodes::kMaxDay);
    fresh.set_param(SourceNodes::kPortDay, qp::ports::Value{std::nan("")});
    REQUIRE(SourceNodes::read_day(fresh) == SourceNodes::kDefaultDay);

    // **The graph path, asserted as an equality rather than as a similarity.** A dipole whose tilt is driven by the
    // June solstice must bake the **same table** as a dipole whose tilt parameter says 34.44 degrees, bit for bit:
    // the wire and the parameter are two ways to say one number, and a driver that arrived with a rounding of its
    // own would be a second answer to "what does this day mean".
    const double re = kEarthRadiusM;
    const double solstice_tilt = SourceNodes::tilt_degrees_for_day(SourceNodes::kDefaultDay);
    const auto tile = [&](Scene& target, const graph::NodeId dipole) {
        for (graph::PortNumber axis = 0; axis < 3; ++axis) {
            target.set(dipole, FieldNodes::kPortOrigin0 + axis, -6.0 * re);
            target.set(dipole, FieldNodes::kPortSpacing0 + axis, 0.5 * re);
            target.set(dipole, FieldNodes::kPortCount0 + axis, 25.0);
        }
    };

    Scene driven;
    const graph::NodeId date = driven.add(SourceNodes::kDayType);
    driven.set(date, SourceNodes::kPortDay, SourceNodes::kDefaultDay);
    const graph::NodeId leaning = driven.add(FieldNodes::kDipoleType);
    driven.set(leaning, FieldNodes::kPortTiltDegrees, 0.0);
    driven.set(leaning, FieldNodes::kPortMomentAm2, kDipoleMomentAm2);
    tile(driven, leaning);
    driven.wire(date, SourceNodes::kPortDayOut, leaning, FieldNodes::kPortTiltDriver);
    REQUIRE(driven.bake().has_value());
    const gfield::FieldValue from_a_date =
        driven.fields.view(gfield::FieldKey{leaning.index, FieldNodes::kPortField});
    REQUIRE(gfield::is_readable(from_a_date));

    // The same dipole with the tilt typed in, and the two tables compared node by node.
    Scene typed;
    const graph::NodeId typed_dipole = typed.add(FieldNodes::kDipoleType);
    typed.set(typed_dipole, FieldNodes::kPortTiltDegrees, solstice_tilt);
    typed.set(typed_dipole, FieldNodes::kPortMomentAm2, kDipoleMomentAm2);
    tile(typed, typed_dipole);
    REQUIRE(typed.bake().has_value());
    const gfield::FieldValue from_a_parameter =
        typed.fields.view(gfield::FieldKey{typed_dipole.index, FieldNodes::kPortField});
    REQUIRE(gfield::is_readable(from_a_parameter));
    REQUIRE(from_a_date.point_count() == from_a_parameter.point_count());
    for (std::uint64_t point = 0; point < from_a_date.point_count(); ++point) {
        for (std::uint64_t component = 0; component < 3; ++component) {
            REQUIRE(gfield::get_component(from_a_date, point, component) ==
                    gfield::get_component(from_a_parameter, point, component));
        }
    }

    // And with the socket **empty** the parameter is the lean, so a graph written before this socket existed keeps
    // its field -- and the untilted dipole is *not* the solstice dipole, which is the measurement that says the wire
    // did something rather than nothing.
    Scene alone;
    const graph::NodeId upright = alone.add(FieldNodes::kDipoleType);
    alone.set(upright, FieldNodes::kPortTiltDegrees, 0.0);
    alone.set(upright, FieldNodes::kPortMomentAm2, kDipoleMomentAm2);
    tile(alone, upright);
    REQUIRE(alone.bake().has_value());
    const gfield::FieldValue no_driver = alone.fields.view(gfield::FieldKey{upright.index, FieldNodes::kPortField});
    bool differs = false;
    for (std::uint64_t point = 0; point < no_driver.point_count() && !differs; ++point) {
        for (std::uint64_t component = 0; component < 3; ++component) {
            if (gfield::get_component(no_driver, point, component) !=
                gfield::get_component(from_a_date, point, component)) {
                differs = true;
            }
        }
    }
    REQUIRE(differs);

    // The declaration: two drivers now, and the tilt arrives in **degrees** -- the unit this kit's dipole socket is
    // written in, not the radians the reference's engine works in.
    const std::vector<graph::NodeDesc> drivers = SourceNodes::node_types();
    REQUIRE(drivers.size() == 2);
    REQUIRE(drivers[1].type_name == SourceNodes::kDayType);
    const graph::PortDesc* published = drivers[1].find_port(SourceNodes::kPortDayOut, true);
    REQUIRE(published != nullptr);
    REQUIRE(published->type == qp::ports::kScalarF64);
    REQUIRE(published->unit_symbol == std::string{"deg"});
    REQUIRE_FALSE(drivers[1].find_port(SourceNodes::kPortDay, false)->connectable);
}

TEST_CASE("magnetosphere.field_nodes.the_shield_suppresses_convection_inside_its_radius", "[magnetosphere]") {
    // **The third of `efield.py`'s three, and the one that is a coefficient rather than a field.** The reference's
    // own header states the family's composition and then shows it: `E = add(corotation(B), mul(convection,
    // volland_shield))`. So this case checks the factor's shape, checks that the multiplier composes it exactly, and
    // then measures the one mathematical consequence that a *coefficient* has and a *potential* would not.
    const double re = kEarthRadiusM;
    const double r0 = 4.0 * re;
    const double amplitude = FieldNodes::kDefaultConvectionA;
    const GridSpec grid{Vec3{-8.0 * re, -8.0 * re, -8.0 * re}, Vec3{0.25 * re, 0.25 * re, 0.25 * re}, 65, 65, 65};
    gfield::FieldSet fields;
    const gfield::FieldKey shield_key{91, FieldNodes::kPortShieldOut};
    REQUIRE(bake_shield(r0, grid, shield_key, fields));
    const gfield::FieldValue shield = fields.view(shield_key);
    REQUIRE(gfield::is_readable(shield));
    // A **scalar** table, and the shape is asserted before anything is read out of it: a consumer that assumed
    // three components would read other nodes' coefficients and call them its own.
    REQUIRE(shield.is_scalar());
    REQUIRE_FALSE(shield.is_vector());
    REQUIRE(shield.desc.component == qp::abi::ComponentKind::scalar);
    REQUIRE(shield.point_count() == grid.point_count());

    // Every node against the closed form, and the two regimes counted so that neither can pass by being empty.
    std::uint64_t inside = 0;
    std::uint64_t outside = 0;
    for (std::uint64_t point = 0; point < shield.point_count(); ++point) {
        const std::uint64_t k = point % grid.nz;
        const std::uint64_t j = (point / grid.nz) % grid.ny;
        const std::uint64_t i = point / (grid.nz * grid.ny);
        const Vec3 at{grid.origin_m.x + static_cast<double>(i) * grid.spacing_m.x,
                      grid.origin_m.y + static_cast<double>(j) * grid.spacing_m.y,
                      grid.origin_m.z + static_cast<double>(k) * grid.spacing_m.z};
        const double ratio = norm(at) / r0;
        const double expected = ratio < 1.0 ? ratio * ratio : 1.0;
        REQUIRE(gfield::get_component(shield, point, 0) == expected);
        // In `[0, 1]`, and the bottom of that range is **reached**: the coefficient is zero at the centre, which is
        // the formula's own value there and the reason this node needs no floor (the reference's `r > 0.1` guard
        // turns the centre into *one* instead, which is the one place its coefficient is not the formula's).
        REQUIRE(gfield::get_component(shield, point, 0) >= 0.0);
        REQUIRE(gfield::get_component(shield, point, 0) <= 1.0);
        if (ratio < 1.0) {
            ++inside;
        } else {
            ++outside;
        }
    }
    REQUIRE(inside > 0);
    REQUIRE(outside > 0);

    // The shell itself: a node at exactly the shielding radius gets **one**, because the two expressions agree
    // there. That is what makes `r0` a radius with a visible meaning rather than a scale factor, and the grid is
    // whole earth radii in this test so the node exists.
    const auto at_node = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        return gfield::get_component(shield, (static_cast<std::uint64_t>(i) * grid.ny + j) * grid.nz + k, 0);
    };
    REQUIRE(at_node(48, 32, 32) == 1.0);   // (4.0, 0, 0) earth radii: exactly r0
    REQUIRE(at_node(47, 32, 32) < 1.0);    // (3.75, 0, 0): inside
    REQUIRE(at_node(32, 32, 32) == 0.0);   // the centre: the formula's own zero, and no floor in the way

    // **The composition the reference names**, exactly: outside the shell the product *is* the convection field, to
    // the last bit, and inside it is the coefficient times the field -- which `field.mul`'s case already asserts
    // for arbitrary inputs, and which this case asserts for the pair that exists to be used together.
    const gfield::FieldKey convection_key{92, FieldNodes::kPortField};
    const gfield::FieldKey shielded_key{93, FieldNodes::kPortMulOut};
    REQUIRE(bake_convection(amplitude, grid, convection_key, fields));
    const gfield::FieldValue convection = fields.view(convection_key);
    REQUIRE(bake_scaled(convection, shield, shielded_key, fields));
    const gfield::FieldValue shielded = fields.view(shielded_key);
    REQUIRE(gfield::is_readable(shielded));
    REQUIRE(shielded.point_count() == convection.point_count());

    std::uint64_t weakened = 0;
    std::uint64_t untouched = 0;
    for (std::uint64_t point = 0; point < shielded.point_count(); ++point) {
        const double w = gfield::get_component(shield, point, 0);
        for (std::uint64_t component = 0; component < 3; ++component) {
            const double field_value = gfield::get_component(convection, point, component);
            REQUIRE(gfield::get_component(shielded, point, component) == field_value * w);
        }
        if (w < 1.0) {
            ++weakened;
        } else {
            // Outside the shell the coefficient is one, so the product is the field **exactly** -- no rounding, no
            // tolerance, which is the property a graph relies on when it shields one region and not another.
            REQUIRE(gfield::get_component(shielded, point, 0) == gfield::get_component(convection, point, 0));
            ++untouched;
        }
    }
    REQUIRE(weakened > 0);
    REQUIRE(untouched > 0);

    // **What the coefficient construction does that a potential construction would not.** `E = w (-grad phi)` is not
    // `-grad(w phi)`: the difference is `phi grad w`, and the curl is `grad w x E`. With `phi = 2Axy` and
    // `w = r^2/r0^2` the curl has a closed form -- `-4A (x^2 - y^2) / r0^2` -- and the case measures it off the
    // baked table with a five-point stencil, which is exact for polynomials of the degree in play modulo its own
    // truncation. A node that had published `-grad(w phi)` instead would fail this by a factor of order one.
    const auto interior = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        return gfield::get_component(shielded, (static_cast<std::uint64_t>(i) * grid.ny + j) * grid.nz + k, 0);
    };
    const auto interior_y = [&](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        return gfield::get_component(shielded, (static_cast<std::uint64_t>(i) * grid.ny + j) * grid.nz + k, 1);
    };
    const double h = grid.spacing_m.x;
    double worst = 0.0;
    double strongest = 0.0;
    for (std::uint32_t i = 2; i + 2 < grid.nx; ++i) {
        for (std::uint32_t j = 2; j + 2 < grid.ny; ++j) {
            const std::uint32_t k = grid.nz / 2;
            const double x = grid.origin_m.x + static_cast<double>(i) * h;
            const double y = grid.origin_m.y + static_cast<double>(j) * h;
            const double z = grid.origin_m.z + static_cast<double>(k) * h;
            // **The whole stencil must fit inside the shell.** A five-point derivative is exact for the
            // cubic this field is inside `r0`, and it stops being exact the moment one of its nodes sits
            // beyond the radius where the coefficient saturates -- which is what the first version of this
            // measurement did, and why it disagreed with the closed form by thirty-nine percent at the node
            // nearest the boundary.
            if (norm(Vec3{x, y, z}) + 2.0 * h >= r0) continue;
            const double dEy_dx = (-interior_y(i + 2, j, k) + 8.0 * interior_y(i + 1, j, k) -
                                   8.0 * interior_y(i - 1, j, k) + interior_y(i - 2, j, k)) /
                                  (12.0 * h);
            const double dEx_dy = (-interior(i, j + 2, k) + 8.0 * interior(i, j + 1, k) -
                                   8.0 * interior(i, j - 1, k) + interior(i, j - 2, k)) /
                                  (12.0 * h);
            const double measured = dEy_dx - dEx_dy;
            const double predicted = -4.0 * amplitude * (x * x - y * y) / (r0 * r0);
            const double gap = std::abs(measured - predicted);
            if (gap > worst) worst = gap;
            if (std::abs(predicted) > strongest) strongest = std::abs(predicted);
        }
    }
    CAPTURE(worst, strongest);
    REQUIRE(strongest > 0.0);
    // **Measured: 1.4e-15 of the strongest closed-form value**, which is the rounding of the arithmetic -- the
    // five-point stencil is exact for the cubic this field is inside the shell, so what is left is not truncation
    // but the last bits. The tolerance is a million times that, because the statement worth making is "this is the
    // coefficient construction" rather than "this is where one machine rounds": a node that had published
    // `-grad(w phi)` would miss by a factor of order one, nine orders of magnitude above this bound.
    REQUIRE(worst < 1.0e-9 * strongest);

    // Refusals: a shielding radius of zero is not a smaller shield, and a grid that cannot be baked is refused
    // rather than approximated.
    REQUIRE_FALSE(bake_shield(0.0, grid, shield_key, fields));
    REQUIRE_FALSE(bake_shield(-3.0 * re, grid, shield_key, fields));
    REQUIRE_FALSE(bake_shield(std::nan(""), grid, shield_key, fields));
    const GridSpec no_interior{Vec3{}, Vec3{re, re, re}, 1, 1, 1};
    REQUIRE_FALSE(bake_shield(r0, no_interior, shield_key, fields));

    // The declaration: its own numbers, a scalar output, and nine grid ports that are typed rather than wired.
    const std::vector<graph::NodeDesc> types = FieldNodes::node_types();
    REQUIRE(types.size() == 15);
    REQUIRE(types[14].type_name == FieldNodes::kShieldType);
    REQUIRE(types[14].has_compute);
    REQUIRE(types[14].allow_in_field_domain);
    REQUIRE_FALSE(types[14].allow_in_particle_domain);
    const graph::PortDesc* coefficient = types[14].find_port(FieldNodes::kPortShieldOut, true);
    REQUIRE(coefficient != nullptr);
    REQUIRE(coefficient->type == qp::ports::kScalarField);
    REQUIRE(coefficient->unit_symbol == std::string{"1"});
    for (graph::PortNumber offset = 0; offset < 9; ++offset) {
        const graph::PortDesc* port = types[14].find_port(FieldNodes::kPortShieldOrigin0 + offset, false);
        REQUIRE(port != nullptr);
        REQUIRE_FALSE(port->connectable);
    }
}

TEST_CASE("magnetosphere.source.the_kp_index_moves_the_magnetopause", "[magnetosphere]") {
    // **The first driver, and the promise `field.magnetopause` made when it was written**: "the standoff distance is
    // a driver's job: when this kit has one, Kp becomes a socket and this parameter becomes the fallback". Three
    // things are checked here, in the order a reader would ask them: the model's two formulas, the declaration of
    // the node that publishes the index, and -- the one that matters -- that **the surface actually moves** when the
    // index is on a wire rather than typed into the boundary.
    REQUIRE(relative_to(SourceNodes::standoff_re_for_kp(2.0), 10.0 / std::cbrt(3.0)) < 1.0e-15);
    REQUIRE(relative_to(SourceNodes::flaring_for_kp(2.0), 0.59) < 1.0e-15);

    // The direction is the physics: **more activity, less cavity**. Over the whole index range the nose comes in
    // from 7.94 to 5.36 earth radii, and the flaring grows with it.
    REQUIRE(SourceNodes::standoff_re_for_kp(0.0) > SourceNodes::standoff_re_for_kp(4.0));
    REQUIRE(SourceNodes::standoff_re_for_kp(4.0) > SourceNodes::standoff_re_for_kp(9.0));
    // The ends of the range, against the **formula** rather than against a decimal typed here: a typed constant is
    // what this repository keeps finding stale, and the two ends are what turn "monotone" into a statement about a
    // range. The decimal I wrote first was wrong in the fifth digit, and the assertion said so.
    REQUIRE(relative_to(SourceNodes::standoff_re_for_kp(0.0), 10.0 / std::cbrt(2.0)) < 1.0e-15);
    REQUIRE(relative_to(SourceNodes::standoff_re_for_kp(9.0), 10.0 / std::cbrt(6.5)) < 1.0e-15);
    REQUIRE(SourceNodes::flaring_for_kp(9.0) > SourceNodes::flaring_for_kp(0.0));
    // And out of range is clamped rather than extrapolated: below zero is not a quieter day, it is a different scale.
    REQUIRE(SourceNodes::standoff_re_for_kp(-5.0) == SourceNodes::standoff_re_for_kp(0.0));
    REQUIRE(SourceNodes::flaring_for_kp(99.0) == SourceNodes::flaring_for_kp(9.0));

    // The reader: a node with no parameter gets the default, a node with a number gets it, and a node carrying a
    // value off the scale gets the scale's end -- clamping, not refusing, for the reason the parameter readers give
    // everywhere: a half-filled node is a graph being edited.
    qp::graph::Node fresh;
    fresh.type_name = SourceNodes::kKpType;
    REQUIRE(SourceNodes::read_kp(fresh) == SourceNodes::kDefaultKp);
    fresh.set_param(SourceNodes::kPortKp, qp::ports::Value{6.0});
    REQUIRE(SourceNodes::read_kp(fresh) == 6.0);
    fresh.set_param(SourceNodes::kPortKp, qp::ports::Value{12.0});
    REQUIRE(SourceNodes::read_kp(fresh) == SourceNodes::kMaxKp);
    fresh.set_param(SourceNodes::kPortKp, qp::ports::Value{std::nan("")});
    REQUIRE(SourceNodes::read_kp(fresh) == SourceNodes::kDefaultKp);

    // The declaration: one type, one numeric output, and the index itself typed in rather than wired. The table is
    // searched **by name** rather than indexed: how many drivers exist is the date case's business, and this case
    // must not fail merely because a second driver was added beside this one.
    const std::vector<graph::NodeDesc> drivers = SourceNodes::node_types();
    const auto kp_type = std::find_if(drivers.begin(), drivers.end(), [](const graph::NodeDesc& desc) {
        return desc.type_name == SourceNodes::kKpType;
    });
    REQUIRE(kp_type != drivers.end());
    REQUIRE(kp_type->has_compute);
    REQUIRE(kp_type->allow_in_field_domain);
    REQUIRE_FALSE(kp_type->allow_in_particle_domain);
    const graph::PortDesc* published = kp_type->find_port(SourceNodes::kPortKpOut, true);
    REQUIRE(published != nullptr);
    REQUIRE(published->type == qp::ports::kScalarF64);
    // A scalar **value**, not a scalar field: a table would need a lattice, and a number has none.
    REQUIRE(published->type != qp::ports::kScalarField);
    REQUIRE_FALSE(kp_type->find_port(SourceNodes::kPortKp, false)->connectable);

    // **The surface follows the wire.** A driver at Kp = 6 into a magnetopause whose own parameter says ten earth
    // radii: the index must win, and the nose must stand where the Kp model puts it. The weight is baked on a fine
    // slab and the half level is found by bisection along the sunward axis -- the same measurement the
    // magnetopause's own case makes, which is what makes the two comparable.
    const double re = kEarthRadiusM;
    const Vec3 slab_origin{-20.0 * re, -20.0 * re, -0.2 * re};
    const Vec3 slab_spacing{0.1 * re, 0.1 * re, 0.1 * re};

    Scene scene;
    const graph::NodeId driver = scene.add(SourceNodes::kKpType);
    scene.set(driver, SourceNodes::kPortKp, 6.0);
    const graph::NodeId boundary = scene.add(FieldNodes::kMagnetopauseType);
    scene.set(boundary, FieldNodes::kPortMagnetopauseStandoff, 10.0 * re);
    scene.set(boundary, FieldNodes::kPortMagnetopauseFlaring, 0.58);
    scene.set(boundary, FieldNodes::kPortMagnetopauseWidth, 1.0 * re);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        scene.set(boundary, FieldNodes::kPortMagnetopauseOrigin0 + axis,
                  axis == 0 ? slab_origin.x : (axis == 1 ? slab_origin.y : slab_origin.z));
        scene.set(boundary, FieldNodes::kPortMagnetopauseOrigin0 + 3 + axis,
                  axis == 0 ? slab_spacing.x : (axis == 1 ? slab_spacing.y : slab_spacing.z));
        scene.set(boundary, FieldNodes::kPortMagnetopauseOrigin0 + 6 + axis, axis == 2 ? 5.0 : 401.0);
    }
    scene.wire(driver, SourceNodes::kPortKpOut, boundary, FieldNodes::kPortMagnetopauseKp);
    REQUIRE(scene.bake().has_value());
    const gfield::FieldValue weight =
        scene.fields.view(gfield::FieldKey{boundary.index, FieldNodes::kPortWeight});
    REQUIRE(gfield::is_readable(weight));
    REQUIRE(weight.is_scalar());

    const auto nose = [&](const gfield::FieldValue& table) {
        double low = 0.5 * re;
        double high = 19.5 * re;
        for (int step = 0; step < 80; ++step) {
            const double middle = 0.5 * (low + high);
            if (sample_baked_scalar(table, slab_origin, slab_spacing, Vec3{middle, 0.0, 0.0}) > 0.5) {
                low = middle;
            } else {
                high = middle;
            }
        }
        return 0.5 * (low + high) / re;
    };
    const double driven = nose(weight);
    CAPTURE(driven, SourceNodes::standoff_re_for_kp(6.0));
    REQUIRE(relative_to(driven, SourceNodes::standoff_re_for_kp(6.0)) < 0.01);

    // And with the socket **empty** the parameter is the surface, unchanged: a graph written before this port
    // existed keeps the boundary it described.
    Scene without;
    const graph::NodeId boundary_alone = without.add(FieldNodes::kMagnetopauseType);
    without.set(boundary_alone, FieldNodes::kPortMagnetopauseStandoff, 10.0 * re);
    without.set(boundary_alone, FieldNodes::kPortMagnetopauseFlaring, 0.58);
    without.set(boundary_alone, FieldNodes::kPortMagnetopauseWidth, 1.0 * re);
    for (graph::PortNumber axis = 0; axis < 3; ++axis) {
        without.set(boundary_alone, FieldNodes::kPortMagnetopauseOrigin0 + axis,
                    axis == 0 ? slab_origin.x : (axis == 1 ? slab_origin.y : slab_origin.z));
        without.set(boundary_alone, FieldNodes::kPortMagnetopauseOrigin0 + 3 + axis,
                    axis == 0 ? slab_spacing.x : (axis == 1 ? slab_spacing.y : slab_spacing.z));
        without.set(boundary_alone, FieldNodes::kPortMagnetopauseOrigin0 + 6 + axis, axis == 2 ? 5.0 : 401.0);
    }
    REQUIRE(without.bake().has_value());
    const double alone = nose(without.fields.view(gfield::FieldKey{boundary_alone.index, FieldNodes::kPortWeight}));
    REQUIRE(relative_to(alone, 10.0) < 0.01);
    REQUIRE(driven < alone);
}

TEST_CASE("magnetosphere.field_nodes.the_dipole_is_baked_onto_the_grid_it_declares", "[magnetosphere]") {
    // The bake is the model evaluated at every node, and the two things that can be wrong about it are the
    // **positions** and the **layout**. The positions are checked against the dipole itself -- which is the only
    // way to catch a bake that filled the table with the right values at the wrong places -- and the layout
    // against `get_component`, which is the vocabulary a kernel reads through.
    const GridSpec grid{Vec3{-4.0 * kEarthRadiusM, -4.0 * kEarthRadiusM, -4.0 * kEarthRadiusM},
                        Vec3{0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM, 0.5 * kEarthRadiusM},
                        17, 17, 17};
    gfield::FieldSet fields;
    REQUIRE(bake_dipole(kMagneticTiltDegrees, kDipoleMomentAm2, grid, gfield::FieldKey{7, 1}, fields));

    const gfield::FieldValue view = fields.view(gfield::FieldKey{7, 1});
    REQUIRE(gfield::is_readable(view));
    REQUIRE(view.kind() == gfield::Kind::Volume);
    REQUIRE(view.is_vector());
    REQUIRE(view.desc.count[0] == 17);
    REQUIRE(view.desc.count[1] == 17);
    REQUIRE(view.desc.count[2] == 17);
    REQUIRE(view.point_count() == grid.point_count());

    const DipoleField dipole{kMagneticTiltDegrees};
    for (std::uint32_t i = 0; i < grid.nx; i += 4) {
        for (std::uint32_t j = 0; j < grid.ny; j += 4) {
            for (std::uint32_t k = 0; k < grid.nz; k += 4) {
                const Vec3 expected = dipole.at(grid.node_position(i, j, k));
                const std::uint64_t point =
                    (static_cast<std::uint64_t>(i) * grid.ny + j) * grid.nz + k;
                // **Exactly**, not nearly: a node sample is the model's own value, and rounding here would mean
                // the table is not the model at the only points where it can be checked without interpolation.
                REQUIRE(gfield::get_component(view, point, 0) == expected.x);
                REQUIRE(gfield::get_component(view, point, 1) == expected.y);
                REQUIRE(gfield::get_component(view, point, 2) == expected.z);
            }
        }
    }

    // The interior node that is not the origin: an off-by-one in the layout would leave the origin right and
    // everything else shifted, which is why the loop above steps by four rather than checking one point.
    const Vec3 off_axis = dipole.at(grid.node_position(8, 4, 12));
    const std::uint64_t point = (8ULL * 17 + 4) * 17 + 12;
    REQUIRE(gfield::get_component(view, point, 2) == off_axis.z);

    // The dimension survives the bake, because it is what makes the samples a magnetic field.
    REQUIRE(view.dimension().M == 1);
    REQUIRE(view.dimension().T == -2);
    REQUIRE(view.dimension().I == -1);

    // A grid that cannot be baked is refused **before** anything is allocated: a count one digit too large is a
    // request no machine can satisfy, and the vector's answer to that is to throw. Each refusal below is a shape
    // a user can produce with two keystrokes.
    gfield::FieldSet refused;
    REQUIRE_FALSE(bake_dipole(kMagneticTiltDegrees, kDipoleMomentAm2, GridSpec{grid.origin_m, grid.spacing_m, 1, 17, 17},
                              gfield::FieldKey{1, 1}, refused));
    REQUIRE_FALSE(bake_dipole(kMagneticTiltDegrees, kDipoleMomentAm2,
                              GridSpec{grid.origin_m, Vec3{0.0, 1.0, 1.0}, 4, 4, 4}, gfield::FieldKey{1, 1},
                              refused));
    REQUIRE_FALSE(bake_dipole(kMagneticTiltDegrees, kDipoleMomentAm2,
                              GridSpec{grid.origin_m, grid.spacing_m, 4096, 4096, 4096}, gfield::FieldKey{1, 1},
                              refused));
    REQUIRE_FALSE(bake_dipole(std::nan(""), kDipoleMomentAm2, grid, gfield::FieldKey{1, 1}, refused));
    REQUIRE(refused.size() == 0);
    // And the boundary of the cap is a real number rather than "large": a grid at the limit is accepted, which
    // is what makes the refusal above a decision instead of an accident.
    REQUIRE(FieldNodes::kMaxPoints == 4U * 1024U * 1024U);
}

TEST_CASE("magnetosphere.field_nodes.a_handle_names_the_lattice_in_the_store", "[magnetosphere]") {
    // **The round trip that closes the field_handle gap.** A port value carries a descriptor and no data, so the
    // only way a consumer finds the samples is through the store, keyed by the publisher. This case checks the
    // three facts that have to line up: the handle the evaluator returned, the descriptor the store holds, and
    // the samples themselves.
    Scene scene;
    const graph::NodeId dipole = add_dipole(scene, kMagneticTiltDegrees, 8.0, 1.0, 17);

    REQUIRE(scene.bake().has_value());
    // One store entry per baked field, and it is the node's own key: the node's slot index and the output port.
    REQUIRE(scene.fields.size() == 1);
    const gfield::FieldKey key{dipole.index, FieldNodes::kPortField};
    REQUIRE(scene.fields.contains(key));

    const qp::ports::Value handle = scene.result.get(dipole, FieldNodes::kPortField);
    REQUIRE(handle.valid());
    REQUIRE(handle.kind() == qp::ports::ValueKind::field_handle);
    REQUIRE(handle.kind_name() == std::string{"field_handle"});

    // The descriptor in the handle is the store's own, field for field. A second construction of the same
    // lattice would agree until somebody changed one of them, and then a consumer would read a 17-node grid out
    // of a 33-node buffer.
    const qp::abi::LatticeDesc described = handle.as_field();
    const gfield::FieldValue stored = scene.fields.view(key);
    REQUIRE(gfield::is_readable(stored));
    REQUIRE(described.count[0] == stored.desc.count[0]);
    REQUIRE(described.count[1] == stored.desc.count[1]);
    REQUIRE(described.count[2] == stored.desc.count[2]);
    REQUIRE(described.kind == stored.desc.kind);
    REQUIRE(described.component == stored.desc.component);
    REQUIRE(described.element == stored.desc.element);
    REQUIRE(described.spacing_bytes == stored.desc.spacing_bytes);

    // And the samples the handle names are the **field**, not a placeholder: node 14 on the equator is six earth
    // radii out, where the dipole is 137 nT and points south -- the sign the whole kit turns on. Compared
    // against the model rather than against a remembered number, because what this case is about is that the
    // handle and the arithmetic agree, not that the arithmetic is right (the bake's own case does that).
    const GridSpec grid = FieldNodes::read_from(*scene.g.find_node(dipole));
    const std::uint64_t point = (14ULL * 17 + 8) * 17 + 8;
    const Vec3 expected = DipoleField{kMagneticTiltDegrees}.at(grid.node_position(14, 8, 8));
    REQUIRE(gfield::get_component(stored, point, 0) == expected.x);
    REQUIRE(gfield::get_component(stored, point, 1) == expected.y);
    REQUIRE(gfield::get_component(stored, point, 2) == expected.z);
    REQUIRE(expected.z < 0.0);
    REQUIRE(std::abs(expected.z) > 1.3e-7);
    REQUIRE(std::abs(expected.z) < 1.4e-7);
}

TEST_CASE("magnetosphere.field_nodes.a_node_of_another_domain_produces_nothing", "[magnetosphere]") {
    // `evaluate_graph` walks the **whole** graph -- all three domains -- so the bake is called for particle and
    // render nodes too. Answering `unknown_node` for them, as the evaluator test fixtures do, would make a graph
    // containing a pusher impossible to bake at all. A graph whose node type nobody implements is a
    // **validation** finding, and this case is what keeps it from being reported as a bake failure.
    Scene scene;
    const graph::NodeId dipole = add_dipole(scene, 11.5, 8.0, 1.0, 17);
    const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(dipole, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);

    const auto stats = scene.bake();
    REQUIRE(stats.has_value());
    // **Both nodes are visited and both are computed**, and that is the measurement this case is built on:
    // `evaluate_graph` calls the evaluator for every node in the graph, whatever domain the node belongs to, so
    // the pusher really does reach this evaluator -- which produces nothing for it and returns successfully. A
    // bake that answered `unknown_node` for a type it does not own, as the evaluator fixtures in
    // `tests/unit/graph/test_eval.cpp` do, would make **every** graph containing a pusher unbakeable, and the
    // failure would be reported as "a node type nobody implements" about a type that is registered.
    REQUIRE(stats.value().nodes_visited == 2);
    REQUIRE(stats.value().nodes_computed == 2);
    REQUIRE(scene.fields.size() == 1);
    // The bake produced **no value** for the pusher, which is the point: an empty answer rather than a failure.
    REQUIRE(scene.result.get(pusher, 1).kind() == qp::ports::ValueKind::invalid);
    const graph::NodeDesc* desc = scene.host.node_types().find(PusherNodes::kBorisType);
    REQUIRE(desc != nullptr);
    REQUIRE(desc->allow_in_particle_domain);
    REQUIRE_FALSE(desc->allow_in_field_domain);
    // The output it declares is the **state channel**: a wire that carries topology rather than data, and the
    // reason `has_compute` is true. `graph/domain`'s `build_plan` puts a node in a domain's plan only when that
    // flag is set, so it means "this node has an implementation" -- the pusher's is the kernel -- and not "the
    // bake produces a port value for it".
    REQUIRE(desc->outputs.size() == 1);
    REQUIRE(desc->outputs.front().number == PusherNodes::kPortStateOut);
    REQUIRE(desc->outputs.front().type == qp::ports::kParticleBuffer);
    REQUIRE(desc->has_compute);
}

TEST_CASE("magnetosphere.pusher_nodes.the_type_declares_the_ports_the_builder_reads", "[magnetosphere]") {
    // The pusher's port layout is what gives a wire its meaning, so it is the thing the binding is checked
    // against: three field sockets with the types that stop a scalar field being wired into a vector one, and the
    // five settings a run needs but a user should not have to wire.
    //
    // **Two schemes and one layout.** The kit ships Boris and RK4 as separate node types with identical sockets and
    // identical parameters, which is what the reference implementation's four integrators are: one question with
    // several answers. That claim is checked here rather than assumed -- a family whose members had drifted apart
    // would be a graph that runs under one scheme and refuses under the other.
    const std::vector<graph::NodeDesc> types = PusherNodes::node_types();
    REQUIRE(types.size() == 2);
    const graph::NodeDesc& boris = types.front();
    REQUIRE(boris.type_name == PusherNodes::kBorisType);
    REQUIRE(boris.valid());
    const graph::NodeDesc& rk4 = types[1];
    REQUIRE(rk4.type_name == PusherNodes::kRk4Type);
    REQUIRE(rk4.valid());
    REQUIRE(rk4.inputs.size() == boris.inputs.size());
    REQUIRE(rk4.outputs.size() == boris.outputs.size());
    for (std::size_t index = 0; index < boris.inputs.size(); ++index) {
        REQUIRE(rk4.inputs[index].number == boris.inputs[index].number);
        REQUIRE(rk4.inputs[index].type == boris.inputs[index].type);
        REQUIRE(rk4.inputs[index].connectable == boris.inputs[index].connectable);
    }
    REQUIRE(rk4.outputs.front().number == boris.outputs.front().number);
    REQUIRE(rk4.outputs.front().type == boris.outputs.front().type);
    // And the family's own predicate agrees with the list, name by name: a scheme the builder did not recognize
    // would be a node a user can place, wire and configure that the plan silently skips.
    for (const graph::NodeDesc& described : types) REQUIRE(PusherNodes::is_pusher(described.type_name));
    REQUIRE_FALSE(PusherNodes::is_pusher(FieldNodes::kDipoleType));
    REQUIRE_FALSE(PusherNodes::is_pusher(std::string{"particle.rk45"}));

    const graph::PortDesc* magnetic = boris.find_port(PusherNodes::kPortMagnetic, false);
    const graph::PortDesc* electric = boris.find_port(PusherNodes::kPortElectric, false);
    const graph::PortDesc* drag = boris.find_port(PusherNodes::kPortDrag, false);
    REQUIRE(magnetic != nullptr);
    REQUIRE(electric != nullptr);
    REQUIRE(drag != nullptr);
    REQUIRE(magnetic->type == qp::ports::kVectorField);
    REQUIRE(electric->type == qp::ports::kVectorField);
    REQUIRE(drag->type == qp::ports::kScalarField);
    REQUIRE(magnetic->connectable);
    REQUIRE(electric->connectable);
    REQUIRE(drag->connectable);
    // Not marked required, and the reason is in the description: the requirement is enforced where it can be
    // **named**, by `BorisAdvancer::kRequiredFields` reaching `ParticleExecutor::prepare` as `slot_unbound`. A
    // second answer here would be the graph validator's, which reports "a socket nobody wired" -- a different
    // finding with a different fix.
    REQUIRE_FALSE(magnetic->required);

    for (const graph::PortNumber number :
         {PusherNodes::kPortMaxRangeRe, PusherNodes::kPortGravity, PusherNodes::kPortSubstepCap,
          PusherNodes::kPortSpeedLimit, PusherNodes::kPortUseDrag}) {
        const graph::PortDesc* port = boris.find_port(number, false);
        REQUIRE(port != nullptr);
        REQUIRE_FALSE(port->connectable);
    }
    REQUIRE(boris.find_port(PusherNodes::kPortUseDrag, false)->type == qp::ports::kBool);

    // The six socket and knob numbers are distinct, or a wire would land on a setting.
    const graph::PortNumber all[] = {PusherNodes::kPortMagnetic, PusherNodes::kPortElectric,
                                     PusherNodes::kPortDrag,     PusherNodes::kPortMaxRangeRe,
                                     PusherNodes::kPortGravity,  PusherNodes::kPortSubstepCap,
                                     PusherNodes::kPortSpeedLimit, PusherNodes::kPortUseDrag};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        for (std::size_t j = i + 1; j < std::size(all); ++j) REQUIRE(all[i] != all[j]);
    }

    // The parameter block a fresh node produces is one the kernel **accepts**: every default is inside the range
    // `BorisAdvancer::prepare` demands, so a node a user dropped on the canvas without touching anything runs.
    graph::Node fresh;
    fresh.type_name = PusherNodes::kBorisType;
    const pk::ParamBlock params = PusherNodes::param_block_of(fresh);
    BorisAdvancer kernel;
    // The grid slots are zero here by design -- the builder fills them from the field node -- so the block is
    // completed the way the builder completes it before the kernel is asked.
    pk::ParamBlock complete = params;
    complete.set_real(BorisAdvancer::kIndexGridOrigin0, 0.0);
    complete.set_real(BorisAdvancer::kIndexGridSpacing0, kEarthRadiusM);
    complete.set_real(BorisAdvancer::kIndexGridSpacing1, kEarthRadiusM);
    complete.set_real(BorisAdvancer::kIndexGridSpacing2, kEarthRadiusM);
    REQUIRE(kernel.prepare(complete).has_value());
    REQUIRE(params.real(BorisAdvancer::kIndexMaxRange) == PusherNodes::kDefaultMaxRangeRe);
    REQUIRE(params.real(BorisAdvancer::kIndexGravity) == 0.0);
    REQUIRE(params.real(BorisAdvancer::kIndexSubstepCap) == PusherNodes::kDefaultSubstepCap);
    REQUIRE(params.real(BorisAdvancer::kIndexSpeedLimit) == BorisAdvancer::kDefaultSpeedLimit);
    REQUIRE(params.integer(BorisAdvancer::kIndexUseDrag) == 0);
    // And a node carrying a number the kernel would refuse falls back to the default rather than reaching it.
    graph::Node broken;
    broken.set_param(PusherNodes::kPortMaxRangeRe, qp::ports::Value{-1.0});
    broken.set_param(PusherNodes::kPortSpeedLimit, qp::ports::Value{std::nan("")});
    REQUIRE(PusherNodes::param_block_of(broken).real(BorisAdvancer::kIndexMaxRange) ==
            PusherNodes::kDefaultMaxRangeRe);
    REQUIRE(PusherNodes::param_block_of(broken).real(BorisAdvancer::kIndexSpeedLimit) ==
            BorisAdvancer::kDefaultSpeedLimit);

    qp::host::PluginHost host{qp::plugin::Capability::particle_domain};
    REQUIRE(PusherNodes::mount(host) == 2);
    REQUIRE(host.node_types().find(PusherNodes::kBorisType) != nullptr);
}

TEST_CASE("magnetosphere.plan.a_field_node_binds_to_the_slot_the_pusher_reads", "[magnetosphere]") {
    // **The binding.** A field reaches a pusher through an edge, and the port the edge lands on *is* the slot.
    // The case checks the three things that make it true rather than plausible: the bound view is the field the
    // wire names, the six grid slots describe that field's own lattice, and the step declares the requirement
    // that turns an unwired pusher into a named refusal.
    Scene scene;
    const graph::NodeId dipole = add_dipole(scene, kMagneticTiltDegrees, 8.0, 0.5, 33);
    const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
    scene.set(pusher, PusherNodes::kPortMaxRangeRe, 12.0);
    scene.set(pusher, PusherNodes::kPortGravity, 1.0);
    scene.wire(dipole, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);
    REQUIRE(scene.bake().has_value());

    BuiltPlan plan;
    REQUIRE(build_particle_plan(scene.g, scene.order(), scene.fields, plan) == PlanBuildRefusal::ok);
    REQUIRE(plan.pushers == 1);
    REQUIRE(plan.steps.size() == 1);
    REQUIRE(plan.kernels.size() == 1);
    // The dipole node was walked over, not built: a count is how a caller tells "the plan built nothing" from
    // "the plan built one step out of nine nodes".
    REQUIRE(plan.skipped == 1);

    const pp::StepPlan& step = plan.steps.front();
    REQUIRE(step.kernel == plan.kernels.front().get());
    REQUIRE(step.kernel->name() == std::string_view{"boris"});
    REQUIRE(step.required_slots == BorisAdvancer::kRequiredFields);
    REQUIRE(step.required_slots == pp::slot_bit(pp::SlotName::magnetic));

    // The bound field is the one the wire names -- identified by publisher, so a second field of the same shape
    // could not be substituted.
    const gfield::FieldValue bound = step.field(pp::SlotName::magnetic);
    REQUIRE(gfield::is_readable(bound));
    REQUIRE(bound.data == scene.fields.view(gfield::FieldKey{dipole.index, FieldNodes::kPortField}).data);
    REQUIRE(bound.desc.count[0] == 33);
    // The sockets nobody wired are **absent**, not zero-valued: an unbound electric field and an electric field
    // of zero are the same force and different experiments.
    REQUIRE_FALSE(gfield::is_readable(step.field(pp::SlotName::electric)));
    REQUIRE_FALSE(gfield::is_readable(step.field(pp::SlotName::drag)));

    // The six grid slots are the field node's own lattice, read through the baker's reader. A megabyte of
    // samples and a kernel that samples a box somewhere else is the failure this pair of assertions exists for.
    const GridSpec grid = FieldNodes::read_from(*scene.g.find_node(dipole));
    REQUIRE(step.param.real(BorisAdvancer::kIndexGridOrigin0) == grid.origin_m.x);
    REQUIRE(step.param.real(BorisAdvancer::kIndexGridOrigin1) == grid.origin_m.y);
    REQUIRE(step.param.real(BorisAdvancer::kIndexGridOrigin2) == grid.origin_m.z);
    REQUIRE(step.param.real(BorisAdvancer::kIndexGridSpacing0) == grid.spacing_m.x);
    REQUIRE(step.param.real(BorisAdvancer::kIndexGridSpacing1) == grid.spacing_m.y);
    REQUIRE(step.param.real(BorisAdvancer::kIndexGridSpacing2) == grid.spacing_m.z);
    REQUIRE(grid.nx == 33);

    // And the node's own settings reached the block rather than its defaults.
    REQUIRE(step.param.real(BorisAdvancer::kIndexMaxRange) == 12.0);
    REQUIRE(step.param.real(BorisAdvancer::kIndexGravity) == 1.0);

    // The executor prepares it: the plan is not merely well formed, it is one a run can start from.
    pp::ParticleState state{2};
    pp::ParticleExecutor executor{state, std::vector<pp::StepPlan>{step}};
    REQUIRE(executor.prepare() == pp::PlanRefusal::ok);
    REQUIRE(executor.step_count() == 1);

    REQUIRE(std::string{to_string(PlanBuildRefusal::ok)} == "ok");
    REQUIRE(std::string{to_string(PlanBuildRefusal::field_not_baked)} == "field_not_baked");
}

TEST_CASE("magnetosphere.plan.an_unwired_pusher_is_refused_by_the_executor", "[magnetosphere]") {
    // **The payoff from the bridge's requirement mask.** A pusher whose magnetic socket is not wired has no
    // field: it is not an error the builder can report, because a graph under construction is allowed to have
    // sockets nobody has wired yet, and it is not something the kernel can report either -- it is handed a batch,
    // and an absent field and a zero field look identical from inside a step. What can report it is the step's
    // own declaration, checked once, before the first step.
    Scene scene;
    scene.add(PusherNodes::kBorisType);

    BuiltPlan plan;
    REQUIRE(build_particle_plan(scene.g, scene.order(), scene.fields, plan) == PlanBuildRefusal::ok);
    REQUIRE(plan.steps.size() == 1);
    REQUIRE_FALSE(gfield::is_readable(plan.steps.front().field(pp::SlotName::magnetic)));

    pp::ParticleState state{4};
    pp::ParticleExecutor executor{state, std::vector<pp::StepPlan>{plan.steps.front()}};
    REQUIRE(executor.prepare() == pp::PlanRefusal::slot_unbound);
    REQUIRE_FALSE(executor.prepared());
    // Nothing ran, and that is the point: a plan refused at load time is one the user can fix while they are
    // still looking at the graph, rather than a run that finishes with every particle in a straight line.
    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    REQUIRE_FALSE(executor.advance(ctx).has_value());
    REQUIRE(executor.report().steps == 0);

    // And the same graph with the wire present runs, so the refusal above is about the wire and not about the
    // plan being unbuildable.
    Scene wired;
    const graph::NodeId dipole = add_dipole(wired, kMagneticTiltDegrees, 8.0, 1.0, 17);
    const graph::NodeId wired_pusher = wired.add(PusherNodes::kBorisType);
    wired.wire(dipole, FieldNodes::kPortField, wired_pusher, PusherNodes::kPortMagnetic);
    REQUIRE(wired.bake().has_value());
    BuiltPlan wired_plan;
    REQUIRE(build_particle_plan(wired.g, wired.order(), wired.fields, wired_plan) == PlanBuildRefusal::ok);
    pp::ParticleExecutor wired_executor{state, std::vector<pp::StepPlan>{wired_plan.steps.front()}};
    REQUIRE(wired_executor.prepare() == pp::PlanRefusal::ok);
}

TEST_CASE("magnetosphere.plan.a_grid_it_cannot_ask_about_is_refused", "[magnetosphere]") {
    // A field node type this build does not know how to ask for a grid must be **refused**, never defaulted. The
    // default is a box in a different place from the samples, so defaulting would produce a plausible field from
    // the wrong place -- the one failure that no test of either half would catch, because both halves would be
    // right about their own half.
    //
    // The stand-in is a second dipole type name: the shape of a field node without the one this build recognises.
    Scene scene;
    const graph::NodeId field_node = scene.add(FieldNodes::kDipoleType);
    scene.set(field_node, FieldNodes::kPortCount0, 8.0);
    scene.set(field_node, FieldNodes::kPortCount1, 8.0);
    scene.set(field_node, FieldNodes::kPortCount2, 8.0);
    const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(field_node, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);
    REQUIRE(scene.bake().has_value());

    // Rename the node's type to something this build's reader does not answer for, and re-bake is unnecessary:
    // the store still holds the field, so the refusal below is about the **grid**, not about the bake.
    scene.g.find_node_mutable(field_node)->type_name = "field.something_else";
    scene.g.bump_version();

    BuiltPlan plan;
    REQUIRE(build_particle_plan(scene.g, scene.order(), scene.fields, plan) == PlanBuildRefusal::grid_unknown);
    // Refused means empty, not half built: a plan with one step and no grid would be a run that starts and
    // integrates a field from the origin of the coordinate system.
    REQUIRE(plan.steps.empty());
    REQUIRE(plan.kernels.empty());
    REQUIRE(plan.pushers == 0);

    // A socket wired to a node whose field was never baked is the other refusal, and it is a different finding:
    // the bake did not run, or ran for a different node.
    Scene unbaked;
    const graph::NodeId other = add_dipole(unbaked, 0.0, 4.0, 1.0, 9);
    const graph::NodeId other_pusher = unbaked.add(PusherNodes::kBorisType);
    unbaked.wire(other, FieldNodes::kPortField, other_pusher, PusherNodes::kPortMagnetic);
    REQUIRE(unbaked.fields.size() == 0);   // deliberately not baked
    BuiltPlan unbaked_plan;
    REQUIRE(build_particle_plan(unbaked.g, unbaked.order(), unbaked.fields, unbaked_plan) ==
            PlanBuildRefusal::field_not_baked);
    REQUIRE(unbaked_plan.steps.empty());

    REQUIRE(std::string{to_string(PlanBuildRefusal::grid_unknown)} == "grid_unknown");
}

TEST_CASE("magnetosphere.plan.a_stale_order_names_a_node_that_is_not_there", "[magnetosphere]") {
    // An order that names a node the graph does not hold is a refusal rather than a skip. A plan that silently
    // dropped a node would run a graph one node smaller than the one the user drew, and the difference would be
    // invisible: fewer particles moving is a plausible picture.
    Scene scene;
    const graph::NodeId dipole = add_dipole(scene, 0.0, 4.0, 1.0, 9);
    const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
    scene.wire(dipole, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);
    REQUIRE(scene.bake().has_value());

    // A deleted node's id, in an order that was computed before the delete.
    graph::NodeId ghost{};
    ghost.index = static_cast<std::uint32_t>(scene.g.slots().size() + 4);
    std::vector<graph::NodeId> stale = scene.order();
    stale.push_back(ghost);

    BuiltPlan plan;
    REQUIRE(build_particle_plan(scene.g, stale, scene.fields, plan) == PlanBuildRefusal::stale_order);
    REQUIRE(plan.steps.empty());

    // And the plan that was built before the ghost was appended is still what it was, so the refusal above is
    // not a builder that fails on every input.
    BuiltPlan good;
    REQUIRE(build_particle_plan(scene.g, scene.order(), scene.fields, good) == PlanBuildRefusal::ok);
    REQUIRE(good.steps.size() == 1);

    // A rebuild clears what was there: `clear` is called first, so a second build is reported as its own rather
    // than as the union of two.
    REQUIRE(build_particle_plan(scene.g, {}, scene.fields, good) == PlanBuildRefusal::ok);
    REQUIRE(good.steps.empty());
    REQUIRE(good.pushers == 0);
    REQUIRE(good.kernels.empty());
}

TEST_CASE("magnetosphere.plan.a_graph_run_gyrates_a_proton_the_way_the_field_points", "[magnetosphere]") {
    // **The end-to-end case**: a graph, a bake, a plan, an executor, and a proton that orbits the way the field
    // it was wired to says it should.
    //
    // The prediction is a **sign**, and it is the reason this case exists. The dipole's field points south at the
    // equator (a compass feels it), which is `B_z < 0` here; a positive charge gyrates about `B`, so its velocity
    // turns **counter-clockwise** seen from `+z` -- the opposite sense from the uniform `+z` field the kernel's
    // own tests use. Neither half of this kit can make that prediction alone: the field's sign convention and
    // the pusher's rotation direction have to agree, and if either one is flipped the particle orbits the wrong
    // way around a field that still points south.
    Scene scene;
    const double radius_re = 6.6;   // geostationary orbit, where the dipole is 103 nT and the belt lives
    const graph::NodeId dipole = add_dipole(scene, /*tilt=*/0.0, /*half_extent=*/8.0, /*spacing=*/0.25,
                                            /*nodes=*/65);
    const graph::NodeId pusher = scene.add(PusherNodes::kBorisType);
    scene.set(pusher, PusherNodes::kPortMaxRangeRe, 20.0);
    scene.wire(dipole, FieldNodes::kPortField, pusher, PusherNodes::kPortMagnetic);
    REQUIRE(scene.bake().has_value());

    BuiltPlan plan;
    REQUIRE(build_particle_plan(scene.g, scene.order(), scene.fields, plan) == PlanBuildRefusal::ok);
    REQUIRE(plan.steps.size() == 1);

    // A proton at rest in the rotating frame is a proton with a small velocity: 1% of c, which is 3000 km/s and
    // is the order of a ring-current ion.
    pp::ParticleState state{1};
    const double speed = 0.01;
    state.set(0, 0, pp::ParticleState::Slot::position, radius_re * kEarthRadiusM);
    state.set(0, 1, pp::ParticleState::Slot::velocity, speed * kSpeedOfLightSI);
    state.set(0, 0, pp::ParticleState::Slot::charge_mass, kProtonChargeMassSI);

    pp::ParticleExecutor executor{state, std::vector<pp::StepPlan>{plan.steps.front()}};
    REQUIRE(executor.prepare() == pp::PlanRefusal::ok);

    // The field the particle starts in, from the **model** rather than from the table, so the comparison below
    // is against physics and not against the same interpolation the kernel uses.
    const Vec3 start{radius_re * kEarthRadiusM, 0.0, 0.0};
    const DipoleField model{0.0};
    const Vec3 b_start = model.at(start);
    REQUIRE(b_start.z < 0.0);   // points south, which is what a compass feels
    REQUIRE(b_start.x == 0.0);

    const double gamma0 = 1.0 / std::sqrt(1.0 - speed * speed);
    // **The guiding centre, not the starting point.** A gyrating particle's frequency is set by the field it
    // orbits *around*, and the centre is one gyroradius away from where it happens to start -- 0.047 earth radii
    // here, which on a `1/r^3` field is a 2.2% difference. A prediction evaluated at the starting point is
    // therefore wrong by more than the effect being measured, and the first version of this case was, by 2.7%.
    // The first-order variation of the field across the orbit averages out over a gyration, so what is left is
    // second order in `rho / R`: about 2e-4.
    const Vec3 velocity_norm{0.0, speed, 0.0};
    const Vec3 b_norm = b_start * kNormalizedPerTesla;
    const Vec3 start_re{radius_re, 0.0, 0.0};
    const Vec3 centre_re =
        start_re + cross(velocity_norm, b_norm) * (1.0 / (kProtonNormalizedChargeMass * norm2(b_norm)));
    const double gyroradius = norm(centre_re - start_re);
    const Vec3 b_centre = model.at(Vec3{centre_re.x * kEarthRadiusM, centre_re.y * kEarthRadiusM,
                                        centre_re.z * kEarthRadiusM});
    const double omega = kProtonNormalizedChargeMass * (norm(b_centre) * kNormalizedPerTesla) / gamma0;

    const std::size_t steps = 2000;
    pk::AdvanceContext ctx;
    ctx.dt = 0.01;
    const double swept = swept_xy(state, executor, ctx, steps);
    const double elapsed = 0.01 * static_cast<double>(steps);

    REQUIRE(executor.report().steps == steps);
    REQUIRE(executor.report().clamped == 0);
    // The sense: counter-clockwise, because `B` is along `-z` and the charge is positive.
    REQUIRE(swept > 0.0);
    // And the magnitude, from the dipole's own value at the guiding centre. The tolerance is 1% and the
    // arithmetic above says it can be: what is left after the first-order variation cancels is `(rho/R)^2`,
    // about 2e-4, plus 2e-3 from the trilinear bake. A case that demanded 1e-6 here would be demanding that a
    // dipole behave like a uniform field.
    REQUIRE(relative_to(gyroradius, 0.0475) < 0.05);
    REQUIRE(relative_to(swept, omega * elapsed) < 0.01);

    // The speed is unchanged, which is the property a magnetic field has and the one a wrong scheme loses first.
    const double final_speed = norm(Vec3{state.at(0, 0, pp::ParticleState::Slot::velocity),
                                         state.at(0, 1, pp::ParticleState::Slot::velocity),
                                         state.at(0, 2, pp::ParticleState::Slot::velocity)}) *
                               kNormalizedPerMetrePerSecond;
    REQUIRE(relative_to(final_speed, speed) < 1.0e-10);
    REQUIRE(state.status_of(0) == pp::Status::live);

    // The particle stayed inside the box it was given, and the run says so: a field sampled outside the bake
    // grid is clamped to the boundary, which is a finding rather than a detail.
    const double final_radius = norm(Vec3{state.at(0, 0, pp::ParticleState::Slot::position),
                                          state.at(0, 1, pp::ParticleState::Slot::position),
                                          state.at(0, 2, pp::ParticleState::Slot::position)}) *
                                kNormalizedPerMetre;
    REQUIRE(relative_to(final_radius, radius_re) < 0.01);
}
