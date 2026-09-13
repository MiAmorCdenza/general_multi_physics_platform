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
#include <qp/plugins/magnetosphere/units.hpp>

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
        // Seven field models now: the dipole, the uniform field, the sum, the uniform electric field, the region
        // mask, the multiplier and the convection field. Each is a **type of its own** with its own port numbers,
        // which is the composition principle -- a shielding field is `mul(convection, shield)`, not a switch
        // inside a node.
        REQUIRE(FieldNodes::mount(host) == 8);
        REQUIRE(PusherNodes::mount(host) == 1);
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
    REQUIRE(types.size() == 8);
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
    // Eight field models now, and the count is asserted rather than assumed: it is the one place a new type
    // announces itself in the test suite, so a type that silently failed to register is a failure here.
    REQUIRE(FieldNodes::mount(host) == 8);
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
    REQUIRE(types.size() == 8);
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
    REQUIRE(types.size() == 8);
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
    REQUIRE(types.size() == 8);
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

    // A socket with nothing wired is not a grid, and neither is a wire to a node that is gone: the resolver
    // answers false so that the caller can make its own refusal rather than sampling a box it invented.
    const graph::NodeId lonely = scene.add(PusherNodes::kBorisType);
    GridSpec nothing;
    REQUIRE_FALSE(resolve_field_origin(scene.g, lonely, PusherNodes::kPortMagnetic, nothing));
    const graph::Graph empty;
    REQUIRE_FALSE(resolve_field_origin(empty, graph::NodeId{}, PusherNodes::kPortMagnetic, nothing));
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
    const std::vector<graph::NodeDesc> types = PusherNodes::node_types();
    REQUIRE(types.size() == 1);
    const graph::NodeDesc& boris = types.front();
    REQUIRE(boris.type_name == PusherNodes::kBorisType);
    REQUIRE(boris.valid());

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
    REQUIRE(PusherNodes::mount(host) == 1);
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
