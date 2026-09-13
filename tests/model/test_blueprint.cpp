/**
 * @file test_blueprint.cpp
 * @brief A demo's graph as a value: checked before it is offered, and applied as ordinary edits.
 *
 * Test case ids match the @tests fields in `views/model/include/qp/views/model/blueprint.hpp`.
 *
 * ## Why this has its own catalog
 *
 * The point of a blueprint is that the **window does not know a plugin's vocabulary**, so a case that seeded one
 * through the real demonstrator library would test the mechanism with the very dependency the mechanism exists to
 * remove. The catalog here is three descriptors built by hand -- a source, a sink and a scalar knob -- which is all
 * `check_blueprint` is allowed to look at: names, ports, port types.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/blueprint.hpp>

#include <qp/authoring/commands/session.hpp>
#include <qp/graph/ir/node_type_registry.hpp>
#include <qp/graph/structure.hpp>
#include <qp/ports/registry.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace qp::views::model;

namespace {

namespace graph = qp::graph;

/// @brief A node type with one output of `out_type` and one input of `in_type`.
[[nodiscard]] graph::NodeDesc node_type(const char* name, qp::ports::PortTypeId out_type,
                                        qp::ports::PortTypeId in_type, bool has_params, bool has_compute = false) {
    graph::NodeDesc desc;
    desc.type_name = name;
    desc.label = name;
    desc.category = "test";
    desc.version = 1;
    desc.has_compute = has_compute;
    if (in_type != qp::ports::kInvalidType) {
        graph::PortDesc in;
        in.number = 1;
        in.name = "in";
        in.label = "In";
        in.type = in_type;
        in.connectable = true;
        in.required = false;
        desc.inputs.push_back(in);
    }
    if (has_params) {
        graph::PortDesc knob;
        knob.number = 2;
        knob.name = "knob";
        knob.label = "Knob";
        knob.type = qp::ports::kScalarF64;
        knob.connectable = false;
        knob.required = true;
        desc.inputs.push_back(knob);
    }
    graph::PortDesc out;
    out.number = 1;
    out.name = "out";
    out.label = "Out";
    out.type = out_type;
    out.connectable = true;
    desc.outputs.push_back(out);
    return desc;
}

/// @brief Fills a catalog for the cases below: two vector ports that connect, and one scalar that does not.
///
/// A reference rather than a return value because the registry is deliberately non-copyable: it is the catalog a
/// running host owns, and a copy of it would be a second answer to "what types does this build have".
void fill_catalog(graph::NodeTypeRegistry& registry) {
    REQUIRE(registry.register_type(node_type("test.source", qp::ports::kVectorField, qp::ports::kInvalidType, false)) .has_value());
    REQUIRE(registry.register_type(node_type("test.sink", qp::ports::kInvalidType, qp::ports::kVectorField, false)) .has_value());
    REQUIRE(registry.register_type(node_type("test.knob_source", qp::ports::kScalarField, qp::ports::kInvalidType, false)) .has_value());
    REQUIRE(registry.register_type(node_type("test.tuner", qp::ports::kVectorField, qp::ports::kInvalidType, true))
                .has_value());
}

/// @brief A blueprint with one wire, which every case below starts from.
[[nodiscard]] GraphBlueprint two_node_blueprint() {
    GraphBlueprint blueprint;
    blueprint.label = "two nodes";
    BlueprintNode source;
    source.type_name = "test.source";
    source.name = "src";
    BlueprintNode sink;
    sink.type_name = "test.sink";
    sink.name = "dst";
    blueprint.nodes.push_back(source);
    blueprint.nodes.push_back(sink);
    BlueprintWire wire;
    wire.from = 0;
    wire.from_port = 1;
    wire.to = 1;
    wire.to_port = 1;
    blueprint.wires.push_back(wire);
    return blueprint;
}

}  // namespace

TEST_CASE("blueprint.a_blueprint_becomes_nodes_parameters_and_wires", "[views]") {
    // **The mechanism the window now uses and no longer names a plugin for.** A blueprint is applied through the
    // session, so every node, every parameter and every wire is an ordinary undoable edit -- and the case asserts
    // that, because a demo that wrote into the graph directly would be a demo nobody can undo.
    graph::NodeTypeRegistry types;
    fill_catalog(types);
    const qp::ports::PortTypeRegistry& ports = qp::ports::builtin_registry();

    GraphBlueprint blueprint = two_node_blueprint();
    BlueprintNode tuner;
    tuner.type_name = "test.tuner";
    tuner.name = "tuner";
    tuner.params.emplace_back(2, qp::ports::Value{12.5});
    blueprint.nodes.push_back(tuner);

    REQUIRE(check_blueprint(types, ports, blueprint).ok);

    qp::authoring::Session session;
    const BlueprintReport report = apply_blueprint(session, types, blueprint);
    REQUIRE(report.ok);
    REQUIRE(report.nodes.size() == 3);
    REQUIRE(report.wires_connected == 1);
    REQUIRE(report.refusal.empty());
    REQUIRE(session.graph().node_count() == 3);
    REQUIRE(session.graph().edge_count() == 1);

    // The names and the parameters arrived, read back through the graph rather than from the report: a blueprint
    // that reported success while the session dropped a parameter would look identical otherwise.
    const graph::Node* source = session.graph().find_node(report.nodes[0]);
    const graph::Node* tuner_node = session.graph().find_node(report.nodes[2]);
    REQUIRE(source != nullptr);
    REQUIRE(tuner_node != nullptr);
    REQUIRE(source->type_name == "test.source");
    REQUIRE(tuner_node->name == "tuner");
    REQUIRE(tuner_node->param(2).as_f64() == 12.5);

    // **The wire is where the blueprint said**, by index: the first node's output into the second node's input.
    const graph::Edge* edge =
        session.graph().incoming(graph::PortRef{report.nodes[1], 1, graph::PortDirection::input});
    REQUIRE(edge != nullptr);
    REQUIRE(edge->from.node == report.nodes[0]);
    REQUIRE(edge->from.port == 1);

    // And the demo is undone the way any other edit is. **Counted rather than assumed**: `Session` has one undo
    // stack and no transaction primitive, so the three nodes, the parameter and the wire are five steps -- and a
    // case that asserted "one undo clears the canvas" would be asserting a grouping this kit does not have.
    std::size_t undone = 0;
    while (session.can_undo()) {
        REQUIRE(session.undo().has_value());
        ++undone;
    }
    REQUIRE(undone == 5);
    REQUIRE(session.graph().node_count() == 0);
    REQUIRE(session.graph().edge_count() == 0);
}

TEST_CASE("blueprint.a_blueprint_this_build_cannot_offer_is_refused_before_anything_is_added", "[views]") {
    // A demo whose type is not in this build must be refused **before** the graph is touched: a half-applied demo
    // leaves nodes on the canvas and reads as a broken editor rather than as a kit that is not mounted. Each
    // refusal below says which of the four questions failed -- the type, the parameter port, the wire's ports, or
    // the connection verdict -- because "refused" with no reason is what sends a reader to the wrong file.
    graph::NodeTypeRegistry types;
    fill_catalog(types);
    const qp::ports::PortTypeRegistry& ports = qp::ports::builtin_registry();

    GraphBlueprint missing = two_node_blueprint();
    missing.nodes[1].type_name = "test.absent";
    const BlueprintCheck missing_check = check_blueprint(types, ports, missing);
    REQUIRE_FALSE(missing_check.ok);
    REQUIRE(missing_check.refusal.find("test.absent") != std::string::npos);

    GraphBlueprint bad_parameter = two_node_blueprint();
    bad_parameter.nodes[0].params.emplace_back(7, qp::ports::Value{1.0});
    const BlueprintCheck parameter_check = check_blueprint(types, ports, bad_parameter);
    REQUIRE_FALSE(parameter_check.ok);
    REQUIRE(parameter_check.refusal.find("port 7") != std::string::npos);

    GraphBlueprint bad_port = two_node_blueprint();
    bad_port.wires[0].to_port = 9;
    REQUIRE_FALSE(check_blueprint(types, ports, bad_port).ok);

    // A scalar field into a vector socket: the same verdict the canvas gives a user who drags that wire.
    GraphBlueprint wrong_type = two_node_blueprint();
    wrong_type.nodes[0].type_name = "test.knob_source";
    const BlueprintCheck type_check = check_blueprint(types, ports, wrong_type);
    REQUIRE_FALSE(type_check.ok);
    REQUIRE(type_check.refusal.find("refused") != std::string::npos);

    // And nothing above touched a session: the check takes no session at all, which is the strongest form of that
    // statement -- there is no argument through which it could have written anything.
    qp::authoring::Session session;
    REQUIRE(session.graph().node_count() == 0);
    // The applier asks the same question, so a caller that skipped the check gets a refusal **and an untouched
    // graph** -- which is the whole reason the catalog is a parameter of it rather than a rule about its callers.
    const BlueprintReport refused = apply_blueprint(session, types, missing);
    REQUIRE_FALSE(refused.ok);
    REQUIRE(session.graph().node_count() == 0);
    REQUIRE(refused.nodes.empty());
    REQUIRE(refused.refusal == missing_check.refusal);
}
