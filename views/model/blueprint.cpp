/**
 * @file blueprint.cpp
 * @brief Checking and applying a demo's graph, in the two halves the header argues for.
 *
 * The check is here rather than inside the applier because the two answer different questions at different times: a
 * build asks "can I offer this?" at startup, for every demo it ships, and only later does a user ask for one. Both
 * walk the same lists, and neither writes anything before it knows it can.
 */
#include <qp/views/model/blueprint.hpp>

#include <qp/ports/check.hpp>

#include <string>

namespace qp::views::model {

namespace {

/// @brief One node's type description, or null when this build does not have the type.
[[nodiscard]] const qp::graph::NodeDesc* find_desc(const qp::graph::NodeTypeRegistry& catalog,
                                                   const std::string& type_name) noexcept {
    if (type_name.empty()) return nullptr;
    return catalog.find(type_name);
}

/// @brief A refusal sentence for a blueprint whose type this build does not have.
[[nodiscard]] std::string describe_missing_type(const std::string& type_name) {
    return "no such node type: " + type_name;
}

}  // namespace

BlueprintCheck check_blueprint(const qp::graph::NodeTypeRegistry& catalog,
                               const qp::ports::PortTypeRegistry& registry,
                               const GraphBlueprint& blueprint) noexcept {
    try {
        for (const BlueprintNode& node : blueprint.nodes) {
            const qp::graph::NodeDesc* desc = find_desc(catalog, node.type_name);
            if (desc == nullptr) return BlueprintCheck{false, describe_missing_type(node.type_name)};
            for (const auto& [port, value] : node.params) {
                (void)value;
                if (desc->find_port(port, /*is_output=*/false) == nullptr) {
                    return BlueprintCheck{false, "port " + std::to_string(port) + " is not an input of " +
                                                     node.type_name};
                }
            }
        }

        for (const BlueprintWire& wire : blueprint.wires) {
            if (wire.from >= blueprint.nodes.size() || wire.to >= blueprint.nodes.size()) {
                return BlueprintCheck{false, "a wire names a node the blueprint does not have"};
            }
            const qp::graph::NodeDesc* from = find_desc(catalog, blueprint.nodes[wire.from].type_name);
            const qp::graph::NodeDesc* to = find_desc(catalog, blueprint.nodes[wire.to].type_name);
            if (from == nullptr || to == nullptr) return BlueprintCheck{false, "a wire names a missing type"};
            const qp::graph::PortDesc* out = from->find_port(wire.from_port, /*is_output=*/true);
            const qp::graph::PortDesc* in = to->find_port(wire.to_port, /*is_output=*/false);
            if (out == nullptr || in == nullptr) {
                return BlueprintCheck{false, "a wire names a port that is not declared"};
            }
            // The same verdict the canvas gives a user who drags this wire: one rule, asked in one place, so a
            // blueprint cannot describe a wire the editor would refuse to draw.
            const qp::ports::PortTypeDesc* from_type = registry.find(out->type);
            const qp::ports::PortTypeDesc* to_type = registry.find(in->type);
            if (from_type == nullptr || to_type == nullptr) {
                return BlueprintCheck{false, "a wire names a port type this build does not have"};
            }
            const qp::ports::ConnectionCheck verdict = qp::ports::check_connection(
                *from_type, qp::ports::PortDirection::output, *to_type, qp::ports::PortDirection::input);
            if (!verdict.acceptable()) {
                return BlueprintCheck{false, std::string{"a wire is refused: "} + qp::ports::to_string(verdict.verdict)};
            }
        }
    } catch (...) {
        // `noexcept` is a promise, and a registry that throws is a broken registry rather than a broken demo:
        // answering "this build cannot offer it" is the honest form of that.
        return BlueprintCheck{false, "the catalog refused to answer"};
    }
    return BlueprintCheck{true, {}};
}

BlueprintReport apply_blueprint(qp::authoring::Session& session, const qp::graph::NodeTypeRegistry& catalog,
                                const GraphBlueprint& blueprint) {
    BlueprintReport report;
    // The same gate a build asks at startup, asked again by the applier: a caller that skipped the check gets the
    // refusal here rather than half a demo on the canvas.
    const BlueprintCheck check = check_blueprint(catalog, qp::ports::builtin_registry(), blueprint);
    if (!check.ok) {
        report.refusal = check.refusal;
        return report;
    }
    report.nodes.reserve(blueprint.nodes.size());

    for (const BlueprintNode& node : blueprint.nodes) {
        const auto reserved = session.reserve_node();
        if (!reserved.has_value()) {
            report.refusal = "the session refused to reserve a node";
            return report;
        }
        qp::graph::AddNode command;
        command.id = reserved.value();
        command.type_name = node.type_name;
        command.name = node.name;
        const auto applied = session.apply(command);
        if (!applied.has_value()) {
            report.refusal = "the session refused node " + node.name;
            return report;
        }
        for (const auto& [port, value] : node.params) {
            qp::graph::SetParam set;
            set.id = reserved.value();
            set.port = port;
            set.value = value;
            const auto set_applied = session.apply(set);
            if (!set_applied.has_value()) {
                report.refusal = "the session refused a parameter of " + node.name;
                return report;
            }
        }
        report.nodes.push_back(reserved.value());
    }

    for (const BlueprintWire& wire : blueprint.wires) {
        if (wire.from >= report.nodes.size() || wire.to >= report.nodes.size()) {
            report.refusal = "a wire names a node the blueprint does not have";
            return report;
        }
        qp::graph::Connect command;
        command.from = qp::graph::PortRef{report.nodes[wire.from], wire.from_port, qp::graph::PortDirection::output};
        command.to = qp::graph::PortRef{report.nodes[wire.to], wire.to_port, qp::graph::PortDirection::input};
        const auto applied = session.apply(command);
        if (!applied.has_value()) {
            report.refusal = "the session refused a wire";
            return report;
        }
        ++report.wires_connected;
    }

    report.ok = true;
    return report;
}

}  // namespace qp::views::model
