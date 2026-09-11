/**
 * @file test_ir.cpp
 * @brief Unit and property tests for core/graph/ir.
 *
 * The case ids correspond word for word to the @tests field of core/graph/ir/include/qp/graph/ir.hpp.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/ir.hpp>

#include <string>
#include <type_traits>
#include <vector>

using namespace qp::graph;
using qp::ports::Value;

// ===========================================================================
// ids.hpp
// ===========================================================================

TEST_CASE("graph.ids.node_default_is_invalid", "[graph][ir]") {
    const NodeId n;
    REQUIRE_FALSE(n.valid());
    REQUIRE(n.index == kNoSlot);
    REQUIRE(n.generation == kNoGeneration);

    // Half-valid is invalid too: neither a 0 index nor a 0 generation is a legal handle
    REQUIRE_FALSE((NodeId{0, 5}.valid()));
    REQUIRE_FALSE((NodeId{5, 0}.valid()));
    REQUIRE((NodeId{5, 1}.valid()));
}

TEST_CASE("graph.ids.node_equality", "[graph][ir]") {
    STATIC_REQUIRE(sizeof(NodeId) == 8);
    STATIC_REQUIRE(std::is_trivially_copyable_v<NodeId>);
    STATIC_REQUIRE(NodeId{} == NodeId{});
    STATIC_REQUIRE((NodeId{1, 1} == NodeId{1, 1}));
    STATIC_REQUIRE((NodeId{1, 1} != NodeId{1, 2}));
    STATIC_REQUIRE((NodeId{1, 1} != NodeId{2, 1}));
}

TEST_CASE("graph.ids.node_generation_matters", "[graph][ir]") {
    // The generation is the **only reason** this ID scheme exists:
    // once a slot is reused, an old handle must be distinguishable from a new one,
    // or the undo stack / cache key / UI selection would silently point at another node.
    const NodeId old_handle{3, 1};
    const NodeId new_handle{3, 2};   // the same slot, reused
    REQUIRE(old_handle != new_handle);
    REQUIRE(old_handle.index == new_handle.index);
    REQUIRE(old_handle.generation != new_handle.generation);
}

TEST_CASE("graph.ids.port_ref_default_is_invalid", "[graph][ir]") {
    const PortRef r;
    REQUIRE_FALSE(r.valid());
    REQUIRE(r.port == kNoPort);
    REQUIRE(r.direction == PortDirection::input);

    REQUIRE_FALSE((PortRef{NodeId{}, 1, PortDirection::output}.valid()));   // no node
    REQUIRE_FALSE((PortRef{NodeId{1, 1}, 0, PortDirection::output}.valid())); // no port
    REQUIRE((PortRef{NodeId{1, 1}, 1, PortDirection::output}.valid()));
}

TEST_CASE("graph.ids.port_ref_equality", "[graph][ir]") {
    const PortRef a{NodeId{1, 1}, 2, PortDirection::input};
    const PortRef b{NodeId{1, 1}, 2, PortDirection::input};
    const PortRef c{NodeId{1, 1}, 2, PortDirection::output};
    REQUIRE(a == b);
    REQUIRE(a != c);   // a different direction is a different ref
    REQUIRE(a != PortRef{NodeId{1, 2}, 2, PortDirection::input});   // a different generation
}

// ===========================================================================
// descriptor.hpp
// ===========================================================================

namespace {

PortDesc make_port(PortNumber n, const char* name, qp::ports::PortTypeId t) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = t;
    return p;
}

NodeDesc make_node() {
    NodeDesc d{};
    d.type_name = "dipole";
    d.label = "偶极场";
    d.category = "磁场";
    d.inputs.push_back(make_port(1, "moment", qp::ports::kScalarF64));
    d.inputs.push_back(make_port(2, "tilt", qp::ports::kScalarF64));
    d.outputs.push_back(make_port(1, "field", qp::ports::kVectorField));
    d.has_compute = true;
    return d;
}

}  // namespace

TEST_CASE("graph.desc.port_basic", "[graph][ir]") {
    PortDesc p{};
    REQUIRE_FALSE(p.valid());
    p.number = 1;
    p.name = "mass";
    REQUIRE(p.valid());
    REQUIRE(p.label.empty());
    REQUIRE(p.connectable);      // connectable by default
    REQUIRE_FALSE(p.required);   // not required by default
    REQUIRE(p.unit_factor == 1.0);   // default 1.0: the user types SI values
}

TEST_CASE("graph.desc.port_connectable_flag", "[graph][ir]") {
    // Port and Param unified in one place: the same struct, one flag to tell them apart.
    // This removes the duplicate work of validating / drawing / serializing each of them twice.
    PortDesc param{};
    param.number = 1;
    param.name = "density";
    param.connectable = false;   // value only
    REQUIRE(param.valid());

    PortDesc socket{};
    socket.number = 2;
    socket.name = "signal";
    socket.connectable = true;
    REQUIRE(socket.valid());

    // Both have exactly the same layout -- only the flag differs
    REQUIRE(sizeof(param) == sizeof(socket));
}

TEST_CASE("graph.desc.port_numeric_bounds", "[graph][ir]") {
    PortDesc p{};
    p.number = 1;
    p.name = "angle";
    p.type = qp::ports::kScalarF64;
    p.has_range = true;
    p.min_value = -180.0;
    p.max_value = 180.0;
    p.step = 1.0;
    p.unit_factor = 1.0;
    p.unit_symbol = "deg";   // display only; the dimension still comes from the port type

    REQUIRE(p.has_range);
    REQUIRE(p.min_value == -180.0);
    REQUIRE(p.max_value == 180.0);
    REQUIRE(p.unit_symbol == "deg");
}

TEST_CASE("graph.desc.port_choices", "[graph][ir]") {
    PortDesc p{};
    p.number = 1;
    p.name = "axis";
    p.type = qp::ports::kEnum;
    p.choice_names = {"x", "y", "z"};
    p.choice_labels = {"X 轴", "Y 轴", "Z 轴"};

    REQUIRE(p.choice_names.size() == 3);
    REQUIRE(p.choice_names.size() == p.choice_labels.size());
    REQUIRE(p.choice_names[0] == "x");
}

TEST_CASE("graph.desc.input_view_lookup", "[graph][ir]") {
    const PortValues values{{1, Value{2.5}}, {2, Value{std::int64_t{7}}}};
    const InputView in{values};

    REQUIRE(in.f64(1) == 2.5);
    REQUIRE(in.i64(2) == 7);
    // A missing port returns a fallback value instead of crashing
    REQUIRE(in.f64(99) == 0.0);
    REQUIRE(in.i64(99) == 0);
    REQUIRE_FALSE(in.boolean(99));
    REQUIRE(in.text(99).empty());
    // A wrong type also returns the fallback value
    REQUIRE(in.i64(1) == 0);
}

TEST_CASE("graph.desc.node_basic", "[graph][ir]") {
    const NodeDesc d = make_node();
    REQUIRE(d.valid());
    REQUIRE(d.type_name == "dipole");
    REQUIRE(d.version == 1);
    REQUIRE(d.inputs.size() == 2);
    REQUIRE(d.outputs.size() == 1);
    REQUIRE(d.has_compute);
    // Default domain policy: allowed in the baked domain, **denied** in the real-time one
    REQUIRE(d.allow_in_field_domain);
    REQUIRE_FALSE(d.allow_in_particle_domain);
}

TEST_CASE("graph.desc.node_port_lookup", "[graph][ir]") {
    const NodeDesc d = make_node();

    const PortDesc* m = d.find_port(1, /*is_output=*/false);
    REQUIRE(m != nullptr);
    REQUIRE(m->name == "moment");

    REQUIRE(d.find_port(2, false) != nullptr);
    REQUIRE(d.find_port(3, false) == nullptr);
    REQUIRE(d.find_port(1, true) != nullptr);          // output port 1
    REQUIRE(d.find_port(2, true) == nullptr);
    REQUIRE(d.find_port(0, false) == nullptr);         // port 0 does not exist

    const PortDesc* by_name = d.find_by_name("tilt", false);
    REQUIRE(by_name != nullptr);
    REQUIRE(by_name->number == 2);
    REQUIRE(d.find_by_name("nope", false) == nullptr);
    REQUIRE(d.find_by_name("field", true) != nullptr);
    REQUIRE(d.find_by_name("field", false) == nullptr);   // an output name is not in the inputs
    REQUIRE(d.find_by_name("", false) == nullptr);
}

TEST_CASE("graph.desc.node_output_count", "[graph][ir]") {
    NodeDesc d = make_node();
    REQUIRE(d.output_count() == 1);
    d.outputs.clear();
    REQUIRE(d.output_count() == 0);   // no outputs -> nothing to evaluate
}

TEST_CASE("graph.desc.node_has_hooks", "[graph][ir]") {
    // A node has only five field-level "capability declarations" and no lifecycle hooks.
    // This is a **ceiling**: a sixth requires an ADR first.
    const NodeDesc d = make_node();
    REQUIRE(d.has_compute);
    // The three optional hooks are expressed by has_compute + the two domain flags, no state
    REQUIRE(d.allow_in_field_domain);
    REQUIRE_FALSE(d.allow_in_particle_domain);
}

// ===========================================================================
// node.hpp
// ===========================================================================

TEST_CASE("graph.node.construction", "[graph][ir]") {
    Node n{};
    REQUIRE_FALSE(n.id.valid());
    REQUIRE(n.type_name.empty());
    REQUIRE(n.params.empty());
    REQUIRE_FALSE(n.bypassed);
    REQUIRE(n.order_hint == 0);
}

TEST_CASE("graph.node.param_lookup", "[graph][ir]") {
    Node n{};
    n.id = NodeId{1, 1};
    n.type_name = "dipole";
    n.set_param(1, Value{3.5});
    n.set_param(2, Value{std::int64_t{4}});

    REQUIRE(n.param(1).as_f64() == 3.5);
    REQUIRE(n.param(2).as_i64() == 4);
    // A parameter that was never set returns an invalid value
    REQUIRE_FALSE(n.param(99).valid());
    REQUIRE_FALSE(n.param(0).valid());
}

TEST_CASE("graph.node.set_param_replaces", "[graph][ir]") {
    Node n{};
    n.set_param(1, Value{1.0});
    n.set_param(1, Value{2.0});      // replace, not append
    REQUIRE(n.params.size() == 1);
    REQUIRE(n.param(1).as_f64() == 2.0);

    // Replacing may change the type
    n.set_param(1, Value{std::string{"text"}});
    REQUIRE(n.params.size() == 1);
    REQUIRE(n.param(1).as_text() == "text");

    // Erase
    REQUIRE(n.erase_param(1));
    REQUIRE_FALSE(n.erase_param(1));   // no longer present
    REQUIRE(n.params.empty());
    REQUIRE_FALSE(n.param(1).valid());
}

TEST_CASE("graph.node.bypass_flag", "[graph][ir]") {
    Node n{};
    REQUIRE_FALSE(n.bypassed);
    n.bypassed = true;
    REQUIRE(n.bypassed);
    // Bypass is **run-time semantics** and changes no structure: node and edges remain
    REQUIRE(n.params.empty());
}

TEST_CASE("graph.node.user_name_is_separate_from_id", "[graph][ir]") {
    // id is the internal addressing handle (with generation; invalid after deletion);
    // name is the human-facing label (YAML key, report reference, error message).
    // The two must stay separate: renaming must not affect any internal reference.
    Node n{};
    n.id = NodeId{7, 3};
    n.name = "spring_1";
    n.type_name = "spring";

    REQUIRE(n.id.valid());
    REQUIRE(n.name == "spring_1");

    const NodeId id_before = n.id;
    n.name = "春天的弹簧";           // rename
    REQUIRE(n.id == id_before);      // the internal handle is unchanged
}

// ===========================================================================
// edge.hpp
// ===========================================================================

TEST_CASE("graph.edge.construction", "[graph][ir]") {
    const Edge e{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                 PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    REQUIRE(e.valid());
    REQUIRE(e.from.node == NodeId{1, 1});
    REQUIRE(e.to.node == NodeId{2, 1});
}

TEST_CASE("graph.edge_equality", "[graph][ir]") {
    const Edge a{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                 PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    Edge b = a;
    REQUIRE(a == b);
    b.to.port = 2;
    REQUIRE(a != b);
    b = a;
    b.from.node.generation = 2;   // the source generation changed -> a different edge
    REQUIRE(a != b);
    b = a;
    b.to.direction = PortDirection::output;   // the direction changed -> a different edge
    REQUIRE(a != b);
}

TEST_CASE("graph.edge_direction_invariant", "[graph][ir]") {
    // Invariant: from must be an output and to must be an input.
    // A reversed direction is not "another legal edge", it is an error.
    const Edge reversed{PortRef{NodeId{1, 1}, 1, PortDirection::input},
                        PortRef{NodeId{2, 1}, 1, PortDirection::output}};
    REQUIRE_FALSE(reversed.valid());

    const Edge out_to_out{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                          PortRef{NodeId{2, 1}, 1, PortDirection::output}};
    REQUIRE_FALSE(out_to_out.valid());

    const Edge in_to_in{PortRef{NodeId{1, 1}, 1, PortDirection::input},
                        PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    REQUIRE_FALSE(in_to_in.valid());

    const Edge missing_node{PortRef{}, PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    REQUIRE_FALSE(missing_node.valid());
}
