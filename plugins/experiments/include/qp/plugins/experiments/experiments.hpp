/**
 * @file experiments.hpp
 * @brief A teacher's experiment: a graph, a name, and the inputs a student is meant to turn.
 *
 * ## What this adds, and what it deliberately reuses
 *
 * The platform ships one document format, `plugins/formats/qpjson`, and one question has been open since
 * `docs/plan-tree.md` section 9.9 wrote it down: what is the *second* format consumer, and what does adding one
 * actually cost? The measurement said the cost was not the text syntax -- a JSON parser is three hundred lines --
 * but the **marshalling** of nodes, edges, parameters and layouts, which `qpjson_format.cpp` performs in both
 * directions and which a second format would otherwise copy.
 *
 * This plugin is the answer, and the answer is that the marshalling does **not** need to be extracted to be
 * reused: an experiment file carries a whole `.qpd` document as one nested member, and `QpJsonFormat` parses it.
 * So this file owns an envelope and nothing else -- a marker, a version, a name, and a delegation -- and the
 * ~450 lines of graph marshalling exist once, in the format that already had them.
 *
 * That is a different conclusion from the one section 9.9 recorded, and it is a better one: the reopening
 * condition said "extract the marshalling first, then a second format is days of work". Composing the formats
 * instead means the second format was hours of work, **and** the two cannot drift, because there is only one
 * implementation of the part that could.
 *
 * ## Why this is `plugins/experiments` and not `plugins/formats/qpx`
 *
 * A `.qpd` document and a `.qpx` experiment are different things, and the difference is not the syntax. A
 * document is **the graph a user built**; an experiment is **the graph a teacher handed out, and the list of
 * what a student may change in it**. The second has a meaning the first does not -- `Experiment::parameters` is
 * the assignment -- and a format directory that held both would be a directory named after syntax holding a
 * concept.
 *
 * ## What an experiment's parameters are, and why they are derived rather than listed
 *
 * `Experiment::parameters` reports every input port the **descriptor calls a setting** that no edge in this
 * document feeds. Two conditions rather than one, and each rules out a different mistake: `connectable == false`
 * is how a `PortDesc` says "this is a knob, not a signal", and the edge check is how the document says what it
 * actually did with that knob. A teacher may wire a setting port to an upstream value -- that is a legitimate
 * experiment, and it means the assignment is the wire.
 *
 * So the list cannot disagree with the graph it came from: a file cannot claim a parameter the draw cannot
 * receive, and adding a setting port to a plugin's node type makes it appear here with no edit to this file.
 *
 * The alternative was a `"parameters"` member in the envelope naming each one, and it was rejected: it would be a
 * second statement of something the descriptor already says, and the failure mode of two statements is that a
 * teacher ships an assignment whose knob list does not match its graph.
 *
 * ## What is refused, and why each refusal is its own code
 *
 * `not_a_document` when the first member is not this format's marker: the file is something else, and telling
 * its owner it is malformed would send them looking for a syntax error that is not there. `unsupported_version`
 * when the marker names a version this build does not read -- a newer teacher file, which a newer build opens.
 * `malformed` for everything else the envelope can be: an unknown member, a missing document, bytes after the
 * closing brace. And anything wrong with the **document** comes back as the document format's own refusal,
 * unchanged, because "your graph has a dangling edge" is a better message than "your experiment is malformed".
 *
 * @ownership   observes (the format borrows the document format it delegates to)
 * @thread      main
 * @pre         The delegated format outlives this object
 * @post        none
 * @invariant   `to_bytes` and `from_bytes` agree about the envelope's shape
 * @errors      Reports through `DocumentRefusal` rather than throwing
 * @frozen      no
 * @tests       experiments.a_teacher_file_loads_and_names_its_inputs,
 *              experiments.a_refusal_is_the_documents_own,
 *              experiments.the_bundle_round_trips,
 *              experiments.a_loaded_experiment_runs,
 *              experiments.a_second_reading_uses_the_first_files_graph,
 *              experiments.the_bundle_is_what_the_file_menu_offers
 */
#pragma once

#include <qp/authoring/persist/persist.hpp>
#include <qp/graph/ir/node_type_registry.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qp::plugins::experiments {

/**
 * @brief One input a student may set: where it is, what it is called, and what it may be.
 *
 * A flat record rather than a path like `"pendulum.length"`, because a node's **user name** is optional and a
 * graph may legitimately hold two nodes of one type -- so a name-based path would be ambiguous exactly when a
 * student's file is most interesting. `node` and `port` are the handles the graph itself addresses by.
 *
 * @ownership   owns
 * @thread      main
 * @pre         `port` is an input port of the node `node` names
 * @post        none
 * @invariant   `min_value <= max_value` when `bounded`
 * @errors      noexcept
 * @frozen      no
 * @tests       experiments.a_teacher_file_loads_and_names_its_inputs
 */
struct ExperimentParameter final {
    /// The node whose input this is. `index` and `generation` together, as the graph addresses it.
    std::uint32_t node_index = 0;
    std::uint32_t node_generation = 0;
    /// The node's type, so a report can say "the pendulum's length" without resolving the graph again.
    std::string type_name{};
    /// The node's user name, or empty when it has none.
    std::string node_name{};
    /// Which input port.
    std::uint32_t port = 0;
    /// The port's stable name, e.g. `length`. What a script and a log line use.
    std::string name{};
    /// The port's user-facing label, or empty when the descriptor has none.
    std::string label{};
    /// The unit the user types in, as the descriptor spells it. Empty means SI.
    std::string unit{};
    /// The value the file holds for this input, in SI units.
    double value = 0.0;
    /// Whether `min_value`/`max_value` apply.
    bool bounded = false;
    /// The smallest value the descriptor allows, in the same units as `value`.
    double min_value = 0.0;
    /// The largest value the descriptor allows, in the same units as `value`.
    double max_value = 0.0;
    /// How the port presents the value to the user. False for a text, integer or boolean input.
    bool numeric = true;
};

/**
 * @brief A teacher's experiment: the document, plus what it is and what it is for.
 *
 * @ownership   owns
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `parameters` describes exactly the unwired inputs of `document.graph`
 * @errors      noexcept
 * @frozen      no
 * @tests       experiments.a_teacher_file_loads_and_names_its_inputs
 */
struct Experiment final {
    /// Stable identifier for the experiment, e.g. "simple-pendulum". May be empty for a file that names none.
    std::string name{};
    /// What the student is meant to do, in the teacher's words. May be empty.
    std::string description{};
    /// The graph, its layouts and its title: the same thing a `.qpd` holds, borrowed whole from the document
    /// format. Move-only, because a graph is.
    qp::authoring::DocumentSnapshot document{};
    /// The inputs the file did not wire, in graph order. Derived from `document`, never declared beside it.
    std::vector<ExperimentParameter> parameters{};
};

/**
 * @brief The experiment bundle: `qp.experiment.json`, extension `.qpx`.
 *
 * @ownership   observes the delegated format
 * @thread      main
 * @pre         `inner` outlives this object
 * @post        none
 * @invariant   `format().name` is distinct from the delegated format's
 * @errors      Reports through `DocumentRefusal` rather than throwing
 * @frozen      no
 * @tests       experiments.the_bundle_round_trips
 */
class ExperimentFormat final : public qp::authoring::IDocumentFormat {
public:
    /// @brief The version this build writes and the highest it reads.
    static constexpr std::int64_t kVersion = 1;
    /// @brief The envelope's first member, which is what "this is an experiment" means.
    static constexpr const char* kMarkerKey = "experiment";

    /**
     * @brief Builds the format over the document format it carries documents in.
     *
     * **Borrowed, and that is the whole design.** This class owns no marshalling: it writes an envelope around
     * whatever `inner` writes and hands the nested member back to `inner` to read. A second implementation of
     * node, edge, parameter and layout marshalling is the thing this composition exists to avoid, and an object
     * that owned its own copy of the document format would be one.
     *
     * @param inner  The format an experiment's `document` member is written and read by.
     * @param catalog Where node types are resolved, so a parameter can carry its label, unit and bounds.
     *
     * @ownership   observes both
     * @thread      main
     * @pre         `inner` and `catalog` outlive this object
     * @post        `format().name == "qp.experiment.json"`
     * @invariant   Nothing is copied from `inner` or `catalog`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       experiments.the_bundle_round_trips
     */
    ExperimentFormat(const qp::authoring::IDocumentFormat& inner,
                     const qp::graph::INodeCatalog& catalog) noexcept;

    /// @brief This format's description. Stable for the object's lifetime.
    [[nodiscard]] const qp::authoring::DocumentFormatDesc& format() const noexcept override;

    /**
     * @brief Writes the envelope, with the document nested inside it.
     *
     * The three refusals a text format owes -- a live field handle, a non-UTF-8 string, a non-finite number -- are
     * **not** checked here. They are checked by the delegated writer, which must refuse them anyway, and checking
     * them twice would be a second place for the rule to live. So a document this format cannot carry comes back
     * with the document format's own code, which is also the more useful message.
     *
     * The envelope's `name` is the document's **title**, and its `description` is not written, because
     * `DocumentSource` is a graph and a title and nothing else. That is the interface's shape rather than this
     * format's choice: an assignment is not part of a document, so it cannot come back out of one. A caller that
     * owns one composes its own envelope -- which is what this format does to the document format, one level
     * down.
     *
     * @param source The document to write, borrowed.
     * @param out    Receives the bytes. Cleared at entry.
     *
     * @ownership   observes `source`, owns `out`'s content
     * @thread      main
     * @pre         none
     * @post        On `ok`, `out` holds one JSON object whose `document` member is a valid `.qpd`
     * @invariant   Writing the same source twice produces the same bytes
     * @errors      noexcept; whatever the delegated format refuses, plus `malformed` for a source the envelope
     *              cannot describe -- which is nothing today, because the envelope adds no requirement of its own
     * @complexity  O(nodes + edges + layouts)
     * @nondet      none
     * @frozen      no
     * @tests       experiments.the_bundle_round_trips
     */
    [[nodiscard]] qp::authoring::DocumentRefusal to_bytes(const qp::authoring::DocumentSource& source,
                                                          std::string& out) const noexcept override;

    /**
     * @brief Reads the envelope, then hands the nested document to the delegated format.
     *
     * @param bytes The bytes to parse. May be anything at all.
     * @param out   Receives the document. Unchanged on failure.
     *
     * @ownership   observes `bytes`, owns `out`'s content
     * @thread      main
     * @pre         none
     * @post        On `ok`, `out` holds the parsed graph and layouts
     * @invariant   Returns a code rather than crashing for any input, including truncated and arbitrary bytes
     * @errors      noexcept; `not_a_document`, `unsupported_version`, `malformed`, or whatever the delegated
     *              format returns for the nested document
     * @complexity  O(bytes)
     * @nondet      none
     * @frozen      no
     * @tests       experiments.a_teacher_file_loads_and_names_its_inputs,
     *              experiments.a_refusal_is_the_documents_own
     */
    [[nodiscard]] qp::authoring::DocumentRefusal from_bytes(std::string_view bytes,
                                                            qp::authoring::DocumentSnapshot& out) const
        noexcept override;

    /**
     * @brief Reads an envelope and describes the inputs it leaves open.
     *
     * The whole reason a `.qpx` is not a `.qpd`: the same bytes load through either, and only this one answers
     * "what may the student change". A caller that wanted only the graph calls `from_bytes` and pays nothing.
     *
     * @param bytes The bytes to parse. May be anything at all.
     * @param out   Receives the experiment. Unchanged on failure.
     *
     * @ownership   observes `bytes`, owns `out`'s content
     * @thread      main
     * @pre         none
     * @post        On `ok`, `out.parameters` describes every unwired input of `out.document.graph`
     * @invariant   Returns exactly what `from_bytes` returns for the same bytes, and fills more
     * @errors      noexcept; the same codes as `from_bytes`
     * @complexity  O(bytes + nodes)
     * @nondet      none
     * @frozen      no
     * @tests       experiments.a_teacher_file_loads_and_names_its_inputs
     */
    [[nodiscard]] qp::authoring::DocumentRefusal read(std::string_view bytes, Experiment& out) const noexcept;

private:
    const qp::authoring::IDocumentFormat* inner_;
    const qp::graph::INodeCatalog* catalog_;
};

}  // namespace qp::plugins::experiments
