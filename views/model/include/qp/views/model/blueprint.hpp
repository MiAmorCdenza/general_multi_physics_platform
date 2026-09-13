/**
 * @file blueprint.hpp
 * @brief A graph a demo asks for, described in node types rather than in canvas commands.
 *
 * ## Why this exists, and what it fixes
 *
 * The window used to seed its own demonstrator graph, which meant `views/qt` named `demo.signal`,
 * `demo.spring_damper` and `demo.instrument` -- **type names that belong to a plugin**. That worked while there was
 * one demo and one plugin, and it is the wrong shape for the platform the charter describes: an editor that knows
 * a plugin's vocabulary is an editor that has to be edited every time a course ships a new kit, and the seam the
 * whole tree is built on ("content is a plugin, the editor is a view") is exactly what it erodes.
 *
 * A blueprint is the missing value type. It says *what* a demo is -- node types, their starting parameters, the
 * wires between them -- and nothing about how a canvas, a session or a command carries it out. The **application**
 * is what pairs a blueprint with a kit, because the application is the composition root and the only place in the
 * build that is allowed to know both. The window keeps the mechanism and loses the vocabulary.
 *
 * ## Blueprints are checked before they are offered
 *
 * A demo that refuses halfway leaves a half-built graph on the canvas, which reads as a broken editor rather than a
 * broken demo. `check_blueprint` therefore answers **before** anything is applied: every type must be in the
 * catalog, every port must exist, and every wire must be one `ports::check_connection` accepts. It is the same
 * division the rest of this kit uses -- ask first, then do -- and it is what lets an application validate all of
 * its demos at startup and report the ones this build cannot offer.
 *
 * ## What carries them out
 *
 * `apply_blueprint` goes through `authoring::Session` for every node, parameter and wire, so a seeded demo is
 * **undoable** and every panel is told about it, exactly as if a user had drawn it. A blueprint that wrote into
 * the graph directly would produce a demo that cannot be undone and a canvas that does not know it changed.
 *
 * @ownership   pure (the value types) / observes the session (the applier)
 * @thread      main (a window's thread)
 * @pre         none
 * @post        none
 * @invariant   `apply_blueprint` adds nodes in order, so a wire's indices are the same before and after
 * @errors      See each declaration
 * @frozen      no
 * @tests       blueprint.a_blueprint_becomes_nodes_parameters_and_wires,
 *              blueprint.a_blueprint_this_build_cannot_offer_is_refused_before_anything_is_added
 */
#pragma once

#include <qp/authoring/commands/session.hpp>
#include <qp/graph/ir/node_type_registry.hpp>
#include <qp/graph/structure.hpp>
#include <qp/ports/registry.hpp>
#include <qp/ports/value.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace qp::views::model {

/**
 * @brief One node a blueprint asks for: its type, the name a user will see, and its starting parameters.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   An empty `type_name` is a malformed blueprint and `check_blueprint` refuses it
 * @errors      noexcept
 * @frozen      no
 * @tests       blueprint.a_blueprint_becomes_nodes_parameters_and_wires
 */
struct BlueprintNode final {
    /// The node type, as the catalog names it. The one field a plugin's vocabulary appears in.
    std::string type_name;
    /// The instance's name, which is what the canvas and the property panel show.
    std::string name;
    /// The parameters to set, by port number, in the order they are applied.
    std::vector<std::pair<qp::graph::PortNumber, qp::ports::Value>> params;
};

/**
 * @brief One wire: an output port of one node in the list to an input port of another.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Both indices are into the blueprint's node list, not into the graph
 * @errors      noexcept
 * @frozen      no
 * @tests       blueprint.a_blueprint_becomes_nodes_parameters_and_wires
 */
struct BlueprintWire final {
    /// The index of the node the wire leaves.
    std::size_t from = 0;
    /// The output port it leaves from.
    qp::graph::PortNumber from_port = 1;
    /// The index of the node the wire enters.
    std::size_t to = 0;
    /// The input port it enters.
    qp::graph::PortNumber to_port = 1;
};

/**
 * @brief A demo's graph, in the vocabulary of node types.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Wires refer to nodes by index, so a blueprint is independent of the ids a session allocates
 * @errors      noexcept
 * @frozen      no
 * @tests       blueprint.a_blueprint_becomes_nodes_parameters_and_wires
 */
struct GraphBlueprint final {
    /// What a menu entry says. Empty for a blueprint nothing offers.
    std::string label;
    /// The nodes, in the order they are added.
    std::vector<BlueprintNode> nodes;
    /// The wires, made after every node exists.
    std::vector<BlueprintWire> wires;
};

/**
 * @brief What checking a blueprint found.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `ok` is true exactly when `refusal` is empty
 * @errors      noexcept
 * @frozen      no
 * @tests       blueprint.a_blueprint_this_build_cannot_offer_is_refused_before_anything_is_added
 */
struct BlueprintCheck final {
    /// Whether this build can offer the blueprint at all.
    bool ok = false;
    /// The first thing that would go wrong, in words -- for a status line or a log.
    std::string refusal;
};

/**
 * @brief What applying a blueprint produced.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   `ok` is true exactly when every node, parameter and wire was applied
 * @errors      noexcept
 * @frozen      no
 * @tests       blueprint.a_blueprint_becomes_nodes_parameters_and_wires
 */
struct BlueprintReport final {
    /// Whether the whole demo was applied.
    bool ok = false;
    /// The ids of the nodes added, in blueprint order.
    std::vector<qp::graph::NodeId> nodes;
    /// How many wires were made.
    std::size_t wires_connected = 0;
    /// The first thing that failed, in words, or empty on success.
    std::string refusal;
};

/**
 * @brief Whether `catalog` can offer this blueprint, asked before anything is added to a graph.
 *
 * Three questions, in the order a reader would ask them: is every type in the catalog, does every parameter and
 * every wire name a port that type declares, and does `ports::check_connection` accept every wire. A numeric
 * parameter on a port that does not exist is refused here rather than silently dropped later, which is the
 * difference between a demo that works and a demo that is missing a number nobody notices.
 *
 * @param catalog  The node-type registry. Borrowed; must outlive the call.
 * @param registry The port-type registry, for the connection verdicts. Borrowed.
 * @param blueprint The blueprint to check.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        On false, `refusal` names the first problem in words
 * @invariant   Adds nothing to any graph and touches no session
 * @errors      noexcept
 * @complexity  O(nodes + wires)
 * @nondet      none
 * @frozen      no
 * @tests       blueprint.a_blueprint_this_build_cannot_offer_is_refused_before_anything_is_added
 */
[[nodiscard]] BlueprintCheck check_blueprint(const qp::graph::NodeTypeRegistry& catalog,
                                             const qp::ports::PortTypeRegistry& registry,
                                             const GraphBlueprint& blueprint) noexcept;

/**
 * @brief Adds every node, sets every parameter and makes every wire, through the session.
 *
 * **It checks first**, with the same `check_blueprint` a build asks at startup, so a caller cannot half-apply a demo
 * this build cannot offer: the refusal comes back before the graph is touched. That is a parameter more than the
 * signature strictly needs, and it is there because the alternative -- "the caller has checked" -- is a rule two
 * callers remember differently, and the failure mode is a canvas with half a demo on it.
 *
 * In blueprint order, and stopping at the first refusal a session itself makes. Nodes are added with
 * `session.reserve_node()` and `AddNode`, parameters with `SetParam` and wires with `Connect`, so every part of the
 * demo is an ordinary undoable edit.
 *
 * **A demo is as many undo steps as it has edits**, and that is stated rather than hidden: `Session` has one undo
 * stack and no transaction primitive, so seeding five edits takes five undos to reverse. Inventing a grouping here
 * would be a second undo model beside the session's, which is exactly what this kit refuses to grow. The reopening
 * condition is the session gaining a transaction, at which point a demo becomes one step and this paragraph
 * changes.
 *
 * @param session   The session to edit. Borrowed; must outlive the call.
 * @param catalog   The node-type registry, asked before anything is added. Borrowed.
 * @param blueprint The blueprint to apply.
 *
 * @ownership   observes `session`
 * @thread      main
 * @pre         none
 * @post        On true, the session's graph holds every node and wire, with every parameter set
 * @invariant   Every edit goes through the session, so the demo is undoable like any other
 * @errors      noexcept
 * @complexity  O(nodes + wires)
 * @nondet      none
 * @frozen      no
 * @tests       blueprint.a_blueprint_becomes_nodes_parameters_and_wires,
 *              blueprint.a_blueprint_this_build_cannot_offer_is_refused_before_anything_is_added
 */
[[nodiscard]] BlueprintReport apply_blueprint(qp::authoring::Session& session,
                                              const qp::graph::NodeTypeRegistry& catalog,
                                              const GraphBlueprint& blueprint);

}  // namespace qp::views::model
