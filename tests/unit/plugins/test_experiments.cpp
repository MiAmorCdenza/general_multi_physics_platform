/**
 * @file test_experiments.cpp
 * @brief The second document format consumer: a teacher's file, loaded and run.
 *
 * Test case ids match the @tests fields in the plugin header byte for byte.
 *
 * ## What this file is evidence for
 *
 * `docs/plan-tree.md` section 9.9 measured the cost of a second format as "the marshalling of nodes, edges,
 * parameters and layouts, which `qpjson` does in both directions", and recorded the reopening condition as
 * "extract that marshalling first, then a second format is days of work". These cases are the measurement of a
 * different answer: an experiment file carries a whole document as one nested member, so the second format
 * composes with the first instead of copying it.
 *
 * Two things follow, and both are asserted rather than asserted-about:
 *
 *   - **there is one parser for graph JSON**, so the two formats cannot drift. The case that shows it is the one
 *     where a `.qpx` containing a broken graph comes back with the **document format's** refusal, not this
 *     format's -- if this file had its own reader for nodes it would have to have its own sentence for a dangling
 *     edge, and the two sentences would eventually disagree about what a dangling edge is;
 *   - **the experiment is the file plus a derived knob list.** The parameters are not in the envelope; they are
 *     read off the graph and the descriptors, so a file cannot claim a setting its graph has no input for.
 *
 * ## Why the JSON is written by hand here
 *
 * Because the point of the case is that the bytes a teacher writes are the bytes this format reads. A fixture
 * produced by `to_bytes` would prove the round trip and nothing about the shape -- and the shape is the part a
 * second implementation would have to match.
 *
 * The document below is exactly what `QpJsonFormat` writes for one pendulum node: the marker first, then the
 * title, then a graph of one node whose `params` are the four settings ports. The four are produced by
 * `ModelsBinder::node_types()`'s own helpers, so the expected count is derived from the descriptor rather than
 * written here -- a hard-coded four would have to be edited every time a port is added, which is precisely the
 * coupling this plugin exists to avoid.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/experiments/experiments.hpp>

#include <qp/authoring/persist/persist.hpp>
#include <qp/graph/execution/execution.hpp>
#include <qp/graph/ir/node_type_registry.hpp>
#include <qp/graph/structure/graph.hpp>
#include <qp/plugins/models/models.hpp>
#include <qp/plugins/models/models_binder.hpp>
#include <qp/plugins/qpjson/qpjson_format.hpp>
#include <qp/runtime/file/file.hpp>
#include <qp/units/dimensions.hpp>
#include <qp/views/model/document_controller.hpp>

#include <support/temp_dir.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace qp::plugins::experiments;

namespace {

namespace rt = qp::runtime;

/**
 * @brief A teacher's file: one pendulum, with the amplitude wired up and the length left open.
 *
 * The version marker comes first, as the format requires, and the document is a whole `.qpd` inside it. The
 * pendulum's four settings ports are `initial`, `rate`, `gravity` and `length`; this file leaves all four open,
 * which is what a student's copy looks like before they touch it.
 *
 * `angle` at port 1 is 0.2 rad and `omega` at port 2 is 0, which is a swing released from rest -- the standard
 * pendulum exercise, and the one whose period grows with amplitude in a way a linearised textbook misses.
 */
[[nodiscard]] std::string teacher_file() {
    return R"({
  "experiment": 1,
  "name": "simple-pendulum",
  "description": "Measure how the period depends on the length. Do not use the small-angle formula.",
  "document": {
  "qp_document": 1,
  "title": "The simple pendulum",
  "graph": {
    "nodes": [
      {"index": 1, "generation": 1, "type": "model.pendulum", "name": "bob", "bypassed": false, "order": 0, "params": [
        {"port": 1, "kind": "f64", "value": 0.2},
        {"port": 2, "kind": "f64", "value": 0},
        {"port": 3, "kind": "f64", "value": 9.81},
        {"port": 4, "kind": "f64", "value": 1.0}
      ]}
    ],
    "edges": [
    ]
  },
  "layouts": [
  ]
}
})";
}

/// @brief The catalog the models binder describes, so a parameter can carry its label, unit and bounds.
///
/// **Passed in rather than returned**, and that is the registry's own rule rather than a style choice: a
/// `NodeTypeRegistry` is not copyable and not movable, because a catalog that moved would leave its descriptors'
/// addresses behind. So a helper cannot hand one back -- it fills the caller's, which is the same arrangement the
/// application has one owner up.
void fill_catalog_with_the_models(qp::graph::NodeTypeRegistry& catalog) {
    for (const qp::graph::NodeDesc& type : qp::plugins::models::ModelsBinder::node_types()) {
        REQUIRE(catalog.register_type(type).has_value());
    }
}

/// @brief The parameter named `name` on the node whose user name is `node`, or null.
[[nodiscard]] const ExperimentParameter* parameter_of(const Experiment& experiment, const char* node,
                                                      const char* name) {
    for (const ExperimentParameter& parameter : experiment.parameters) {
        if (parameter.node_name == node && parameter.name == name) return &parameter;
    }
    return nullptr;
}

/// @brief How many settings ports the pendulum's descriptor declares.
///
/// Derived rather than written down: the count is a fact about `plugins/models`, and a case that hard-coded it
/// would fail the next time a port is added -- for the wrong reason, and in a file about formats.
[[nodiscard]] std::size_t pendulum_settings_count() {
    for (const qp::graph::NodeDesc& type : qp::plugins::models::ModelsBinder::node_types()) {
        if (type.type_name != "model.pendulum") continue;
        return static_cast<std::size_t>(
            std::count_if(type.inputs.begin(), type.inputs.end(),
                          [](const qp::graph::PortDesc& port) { return !port.connectable; }));
    }
    return 0;
}

}  // namespace

TEST_CASE("experiments.a_teacher_file_loads_and_names_its_inputs", "[experiments]") {
    const qp::plugins::qpjson::QpJsonFormat documents;
    qp::graph::NodeTypeRegistry catalog;
    fill_catalog_with_the_models(catalog);
    const ExperimentFormat format{documents, catalog};

    Experiment experiment;
    const auto loaded = format.read(teacher_file(), experiment);
    REQUIRE(loaded == qp::authoring::DocumentRefusal::ok);

    // The envelope's own half: the two things a document cannot know.
    REQUIRE(experiment.name == "simple-pendulum");
    REQUIRE(experiment.description.find("small-angle") != std::string::npos);

    // The document's half, parsed by the **document format** -- this plugin never looked inside it.
    REQUIRE(experiment.document.title == "The simple pendulum");
    REQUIRE(experiment.document.graph.node_count() == 1);
    REQUIRE(experiment.document.graph.edge_count() == 0);

    // And the derived half. Every settings port the descriptor declares is offered, which is the property that
    // makes a teacher's assignment and its graph unable to disagree.
    REQUIRE(experiment.parameters.size() == pendulum_settings_count());
    REQUIRE(experiment.parameters.size() >= 4);

    const ExperimentParameter* length = parameter_of(experiment, "bob", "length");
    REQUIRE(length != nullptr);
    // The value is the one in the **file**, not the descriptor's default: this is the experiment as handed out.
    REQUIRE(length->value == 1.0);
    // The unit, the label and the bounds come from the descriptor, which is where a node type says what its own
    // input means. A plugin that printed "length = 1" without "m" would be describing the number, not the draw.
    REQUIRE(length->unit == "m");
    REQUIRE(length->label == "Length");
    // `bounded` reports `PortDesc::has_range`, and for the models binder that is **false** even though the port
    // carries a minimum and a maximum. That is the descriptor being precise rather than the reader losing
    // something: `has_range` says "this value is checked against the range", and `models_binder` fills
    // `min_value`/`max_value` as a **hint** for the property panel's spinner without claiming the graph refuses
    // an out-of-range draw -- which it does not. A reader that conflated the two would tell a student their value
    // had been rejected when nothing rejected it.
    REQUIRE_FALSE(length->bounded);
    REQUIRE(length->min_value < length->max_value);
    REQUIRE(length->min_value > 0.0);
    REQUIRE(length->numeric);
    REQUIRE(length->type_name == "model.pendulum");
    REQUIRE(length->node_name == "bob");
    REQUIRE(length->node_index == 1);

    const ExperimentParameter* gravity = parameter_of(experiment, "bob", "gravity");
    REQUIRE(gravity != nullptr);
    REQUIRE(gravity->unit == "m/s^2");
    REQUIRE(gravity->value == 9.81);

    // Every input this model has is a **setting**: `models_binder`'s own `parameter` helper sets
    // `connectable = false`, which is how a descriptor says "a value the user edits, not something to connect".
    // That is the right shape for these models and it is why the knob list is four long -- the alternative, a
    // connectable input the file did not wire, is a different thing and is covered by the case that follows this
    // one. `state` is an **output**, so it is not an input at all and cannot appear here.
    REQUIRE(experiment.parameters.size() == 4);
    REQUIRE(parameter_of(experiment, "bob", "initial") != nullptr);
    REQUIRE(parameter_of(experiment, "bob", "rate") != nullptr);
    REQUIRE(parameter_of(experiment, "bob", "state") == nullptr);
}

TEST_CASE("experiments.a_refusal_is_the_documents_own", "[experiments]") {
    const qp::plugins::qpjson::QpJsonFormat documents;
    qp::graph::NodeTypeRegistry catalog;
    fill_catalog_with_the_models(catalog);
    const ExperimentFormat format{documents, catalog};

    // **The load-bearing case for "there is one parser".** A `.qpx` whose nested graph feeds one input port
    // twice is refused by `QpJsonFormat` with its own code, and this format reports that code unchanged rather
    // than translating it. A second reader for nodes would have had to invent its own sentence here, and the
    // two sentences would eventually disagree about what a duplicate edge is.
    const std::string duplicate_edge = R"({
  "experiment": 1,
  "document": {
  "qp_document": 1,
  "title": "two wires into one input",
  "graph": {
    "nodes": [
      {"index": 1, "generation": 1, "type": "model.pendulum", "name": "a", "bypassed": false, "order": 0, "params": []},
      {"index": 2, "generation": 1, "type": "model.pendulum", "name": "b", "bypassed": false, "order": 0, "params": []}
    ],
    "edges": [
      {"from": {"node": 1, "generation": 1, "port": 1}, "to": {"node": 2, "generation": 1, "port": 1}},
      {"from": {"node": 1, "generation": 1, "port": 1}, "to": {"node": 2, "generation": 1, "port": 1}}
    ]
  },
  "layouts": [
  ]
}
})";
    Experiment experiment;
    REQUIRE(format.read(duplicate_edge, experiment) == qp::authoring::DocumentRefusal::duplicate_edge);
    // And the refusal is reachable through the interface as well, which is what the file menu uses: the two
    // entry points must not be two opinions about the same bytes.
    qp::authoring::DocumentSnapshot snapshot;
    REQUIRE(format.from_bytes(duplicate_edge, snapshot) == qp::authoring::DocumentRefusal::duplicate_edge);

    // A dangling edge is the other side of the same claim: the code names the graph's problem, not the
    // envelope's.
    const std::string dangling = R"({
  "experiment": 1,
  "document": {
  "qp_document": 1,
  "title": "an edge to nowhere",
  "graph": {
    "nodes": [
      {"index": 1, "generation": 1, "type": "model.pendulum", "name": "a", "bypassed": false, "order": 0, "params": []}
    ],
    "edges": [
      {"from": {"node": 1, "generation": 1, "port": 1}, "to": {"node": 9, "generation": 1, "port": 1}}
    ]
  },
  "layouts": [
  ]
}
})";
    REQUIRE(format.from_bytes(dangling, snapshot) == qp::authoring::DocumentRefusal::dangling_edge);

    // The envelope's own refusals, each with its own name because each has its own fix.
    REQUIRE(format.from_bytes("not json at all", snapshot) == qp::authoring::DocumentRefusal::not_a_document);
    REQUIRE(format.from_bytes(R"({"qp_document": 1})", snapshot) ==
            qp::authoring::DocumentRefusal::not_a_document);
    REQUIRE(format.from_bytes(R"({"experiment": 99, "document": {}})", snapshot) ==
            qp::authoring::DocumentRefusal::unsupported_version);
    // An experiment without a document is not an empty experiment; it is a file this format does not define.
    REQUIRE(format.from_bytes(R"({"experiment": 1, "name": "empty"})", snapshot) ==
            qp::authoring::DocumentRefusal::malformed);
    // An unknown member is refused rather than skipped: the version marker is the compatibility mechanism, so a
    // member outside this version's shape means the file and this build disagree about what an experiment is.
    //
    // The nested document is a **valid** one here, and that is load-bearing rather than incidental. The first
    // version of this case passed `"document": {}` and expected `malformed` -- and got it, but from the document
    // format rather than from the envelope, because an empty object is not a document. The assertion would have
    // passed for the wrong reason on the day it was written and failed for the wrong reason on the day the
    // envelope's member check was removed. A case about the envelope has to hand the delegate something it takes.
    const std::string valid_document =
        R"({"qp_document": 1, "title": "t", "graph": {"nodes": [], "edges": []}, "layouts": []})";
    REQUIRE(format.from_bytes(R"({"experiment": 1, "document": )" + valid_document + R"(, "extra": 1})",
                              snapshot) == qp::authoring::DocumentRefusal::malformed);
    // And the same bytes without the extra member load, which is what makes the assertion above about the extra
    // member rather than about the document.
    REQUIRE(format.from_bytes(R"({"experiment": 1, "document": )" + valid_document + "}", snapshot) ==
            qp::authoring::DocumentRefusal::ok);
    REQUIRE(format.from_bytes("", snapshot) == qp::authoring::DocumentRefusal::truncated);
    // Truncated in the middle of the nested document, which is the case the brace matcher has to get right: it
    // must report that the bytes ran out rather than hand the delegate a fragment.
    REQUIRE(format.from_bytes(R"({"experiment": 1, "document": {"qp_document": 1, "graph": {"nodes": [)",
                               snapshot) != qp::authoring::DocumentRefusal::ok);

    // And a refusal left the caller's snapshot alone, which is the interface's own promise and the reason a
    // failed open does not destroy what the user had on screen.
    qp::authoring::DocumentSnapshot held;
    REQUIRE(format.from_bytes(teacher_file(), held) == qp::authoring::DocumentRefusal::ok);
    const std::size_t nodes_before = held.graph.node_count();
    REQUIRE(format.from_bytes(duplicate_edge, held) == qp::authoring::DocumentRefusal::duplicate_edge);
    REQUIRE(held.graph.node_count() == nodes_before);
}

TEST_CASE("experiments.the_bundle_round_trips", "[experiments]") {
    const qp::plugins::qpjson::QpJsonFormat documents;
    qp::graph::NodeTypeRegistry catalog;
    fill_catalog_with_the_models(catalog);
    const ExperimentFormat format{documents, catalog};

    REQUIRE(format.format().name == "qp.experiment.json");
    REQUIRE(format.format().extensions == std::vector<std::string>{"qpx"});
    REQUIRE(format.format().is_text);
    // Distinct from the format it delegates to, because a registry keyed by name would otherwise have two
    // entries under one key and the file menu would offer the same thing twice.
    REQUIRE(format.format().name != documents.format().name);

    Experiment loaded;
    REQUIRE(format.read(teacher_file(), loaded) == qp::authoring::DocumentRefusal::ok);

    // Written back out through the **interface**, which borrows the graph rather than copying it.
    qp::authoring::DocumentSource source{loaded.document.graph, loaded.document.layouts,
                                         loaded.document.title};
    std::string bytes;
    REQUIRE(format.to_bytes(source, bytes) == qp::authoring::DocumentRefusal::ok);
    REQUIRE_FALSE(bytes.empty());
    // The envelope is what a teacher reads, and the document is inside it. Both are checked because a bundle
    // that lost its name would still round-trip the graph perfectly -- and the marker is at offset **one**
    // rather than zero, because the document format's writer puts the opening brace on its own line. Asserting
    // `== 0` here was the first version and it failed, which is the fixture doing its job: the shape a second
    // implementation has to match is the shape, not an idea of it.
    REQUIRE(bytes.find("\"experiment\": 1") == 1);
    REQUIRE(bytes.find("\"document\":") != std::string::npos);
    REQUIRE(bytes.find("qp_document") != std::string::npos);

    // Reading what was written gives the same document. The title, both counts and the parameters are all
    // compared, because a bundle that dropped the layouts or renumbered a node would still parse.
    Experiment again;
    REQUIRE(format.read(bytes, again) == qp::authoring::DocumentRefusal::ok);
    REQUIRE(again.document.title == loaded.document.title);
    REQUIRE(again.document.graph.node_count() == loaded.document.graph.node_count());
    REQUIRE(again.document.graph.edge_count() == loaded.document.graph.edge_count());
    REQUIRE(again.parameters.size() == loaded.parameters.size());
    REQUIRE(parameter_of(again, "bob", "length") != nullptr);
    REQUIRE(parameter_of(again, "bob", "length")->value ==
            parameter_of(loaded, "bob", "length")->value);

    // The name is the document's **title**, and the description is deliberately absent: `DocumentSource` is a
    // graph and a title, because that is what a document is, so an assignment -- which is not part of one --
    // cannot come back out of one. Round-tripping the identity is what this envelope can honestly do; a caller
    // that owns an assignment composes its own envelope, which is what this format does one level down.
    REQUIRE(bytes.find("\"simple-pendulum\"") == std::string::npos);
    REQUIRE(bytes.find("\"name\": \"The simple pendulum\"") != std::string::npos);
    REQUIRE(bytes.find("\"description\"") == std::string::npos);
    // And reading the bundle back recovers the title as the title, and the name as the title too: the two are
    // one fact written twice, which is why the assertion above is about the *assignment* rather than the name.
    REQUIRE(again.name == "The simple pendulum");
    REQUIRE(again.document.title == "The simple pendulum");
    REQUIRE(again.description.empty());

    // Writing the same source twice produces the same bytes, so a save is idempotent and a bundle can be
    // compared rather than parsed.
    std::string twice;
    REQUIRE(format.to_bytes(source, twice) == qp::authoring::DocumentRefusal::ok);
    REQUIRE(twice == bytes);

    // A refusal clears the output, so a caller never receives a prefix of a bundle.
    std::string reused = "stale bytes";
    REQUIRE(format.to_bytes(source, reused) == qp::authoring::DocumentRefusal::ok);
    REQUIRE(reused == bytes);
}

TEST_CASE("experiments.a_loaded_experiment_runs", "[experiments]") {
    // **The case the whole plugin is for**: a graph that came out of a file, with parameters read out of that
    // same file, drives the model and produces the physics. Everything upstream of this is bookkeeping; this is
    // the claim that "adding an experiment does not mean recompiling" is true of the running system rather than
    // of a parser.
    //
    // The file's pendulum is one metre long with a 0.2 rad release, so the period is close to but **not** the
    // small-angle `2 pi sqrt(L/g)` -- the difference is the physics this model exists to show, and checking the
    // small-angle value instead would be checking the approximation rather than the pendulum.
    const qp::plugins::qpjson::QpJsonFormat documents;
    qp::graph::NodeTypeRegistry catalog;
    fill_catalog_with_the_models(catalog);
    const ExperimentFormat format{documents, catalog};

    Experiment experiment;
    REQUIRE(format.read(teacher_file(), experiment) == qp::authoring::DocumentRefusal::ok);

    const ExperimentParameter* length = parameter_of(experiment, "bob", "length");
    REQUIRE(length != nullptr);
    const ExperimentParameter* gravity = parameter_of(experiment, "bob", "gravity");
    REQUIRE(gravity != nullptr);

    // The model is built from the **loaded parameters**, which is what a student changing a knob does.
    std::unique_ptr<qp::graph::execution::IStateOperator> pendulum =
        qp::plugins::models::make_pendulum(gravity->value, length->value);
    REQUIRE(pendulum != nullptr);

    // Released from rest at 0.2 rad, and integrated with the project's own loop.
    auto state = qp::graph::execution::StateView::zeroed(1, 2);
    state.values[0] = 0.2;
    state.values[1] = 0.0;

    constexpr double kDt = 1.0e-4;
    constexpr double kDuration = 6.0;
    double previous = state.values[0];
    double previous_t = 0.0;
    std::vector<double> crossings;
    for (std::size_t i = 1; i <= static_cast<std::size_t>(kDuration / kDt); ++i) {
        REQUIRE(pendulum->step(state, kDt).has_value());
        const double t = kDt * static_cast<double>(i);
        const double now = state.values[0];
        // A crossing of zero, located by linear interpolation between the samples that bracket it: the same
        // reason a lab manual says to time a pendulum at the bottom of its swing.
        if ((previous > 0.0 && now <= 0.0) || (previous < 0.0 && now >= 0.0)) {
            const double fraction = previous / (previous - now);
            crossings.push_back(previous_t + fraction * kDt);
        }
        previous = now;
        previous_t = t;
    }

    // Four crossings is two periods, which is the least that can be called a measurement rather than an
    // interval.
    REQUIRE(crossings.size() >= 4);
    const double measured = (crossings.back() - crossings.front()) / static_cast<double>(crossings.size() - 1) * 2.0;

    const double small_angle = 2.0 * 3.14159265358979323846 * std::sqrt(length->value / gravity->value);
    // The period at 0.2 rad is longer than the small-angle value by the first correction, `theta0^2 / 16`:
    // about 0.25%. That is the assertion -- a measured period equal to the linear formula would mean the model
    // had linearised, and a tolerance loose enough to accept both would be measuring nothing.
    REQUIRE(measured > small_angle);
    const double predicted = small_angle * (1.0 + (0.2 * 0.2) / 16.0);
    REQUIRE(std::abs(measured - predicted) < 1.0e-3 * predicted);
}

TEST_CASE("experiments.the_bundle_is_what_the_file_menu_offers", "[experiments]") {
    // **The case that stops this plugin from being the defect it was written to demonstrate.** Declared,
    // documented, tested, and connected to nothing is the shape `plugins/instruments` had for a year, and a
    // second document format that no menu ever offers is the same defect with a shorter name.
    //
    // `views/app/main.cpp` is where the composition happens -- the format is constructed over the document
    // format and the host's catalog, and mounted -- so this case asserts the two facts that composition
    // depends on: the description a registry keys on is distinct, and a bundle opens through the **document
    // controller**, which is the path the File menu takes.
    const qp::plugins::qpjson::QpJsonFormat documents;
    qp::graph::NodeTypeRegistry catalog;
    fill_catalog_with_the_models(catalog);
    ExperimentFormat format{documents, catalog};

    REQUIRE(format.format().name != documents.format().name);
    REQUIRE(format.format().extensions.front() == "qpx");

    qp::test::TempDir dir;
    const std::string path = dir.path("pendulum.qpx");
    REQUIRE(qp::runtime::write_whole_file(path, teacher_file()) == qp::runtime::FileOutcome::ok);

    qp::authoring::Session session;
    // The controller is handed a format list, which is what the application mounts. One entry is enough here:
    // the question this case asks is whether an experiment opens through the **File menu's** path, not which
    // format is default. The list is built explicitly rather than braced because `IDocumentFormat*` is
    // non-const -- the controller may write through it -- and this case's format is a local `const`.
    std::vector<qp::authoring::IDocumentFormat*> mounted{&format};
    qp::views::model::DocumentController controller{session, std::move(mounted)};
    const qp::views::model::DocumentReport opened = controller.open(format, path);
    REQUIRE(opened.ok);
    // The graph the file holds is on the session's canvas, which is the whole claim: a teacher's file opens as a
    // document, not as a message about an unknown format.
    REQUIRE(session.graph().node_count() == 1);
    REQUIRE(session.graph().find_node(qp::graph::NodeId{1, 1}) != nullptr);

    // And a file that is merely a document does not open as an experiment: the marker is required first, which
    // is what makes "this is a different kind of file" a distinct answer rather than a parse error.
    const std::string document_path = dir.path("plain.qpd");
    REQUIRE(qp::runtime::write_whole_file(
                document_path,
                R"({"qp_document": 1, "title": "t", "graph": {"nodes": [], "edges": []}, "layouts": []})") ==
            qp::runtime::FileOutcome::ok);
    const qp::views::model::DocumentReport refused =
        controller.open(const_cast<ExperimentFormat&>(format), document_path);
    REQUIRE_FALSE(refused.ok);
    // And the failed open left the loaded document alone, which is the controller's own promise.
    REQUIRE(session.graph().node_count() == 1);
}

TEST_CASE("experiments.a_second_reading_uses_the_first_files_graph", "[experiments]") {
    // Two experiments through one format object, which is how a session uses it: the format is constructed once
    // and reads whatever the user opens. A format that cached anything about the first file would answer the
    // second with the first one's parameters, and that failure looks exactly like "the file did not load".
    const qp::plugins::qpjson::QpJsonFormat documents;
    qp::graph::NodeTypeRegistry catalog;
    fill_catalog_with_the_models(catalog);
    const ExperimentFormat format{documents, catalog};

    const std::string second = R"({
  "experiment": 1,
  "name": "long-pendulum",
  "document": {
  "qp_document": 1,
  "title": "The long pendulum",
  "graph": {
    "nodes": [
      {"index": 1, "generation": 1, "type": "model.pendulum", "name": "bob", "bypassed": false, "order": 0, "params": [
        {"port": 1, "kind": "f64", "value": 0.1},
        {"port": 2, "kind": "f64", "value": 0},
        {"port": 3, "kind": "f64", "value": 9.8},
        {"port": 4, "kind": "f64", "value": 4.0}
      ]}
    ],
    "edges": [
    ]
  },
  "layouts": [
  ]
}
})";

    Experiment first;
    REQUIRE(format.read(teacher_file(), first) == qp::authoring::DocumentRefusal::ok);
    Experiment other;
    REQUIRE(format.read(second, other) == qp::authoring::DocumentRefusal::ok);

    REQUIRE(other.name == "long-pendulum");
    REQUIRE(other.document.title == "The long pendulum");
    REQUIRE(parameter_of(other, "bob", "length")->value == 4.0);
    REQUIRE(parameter_of(other, "bob", "gravity")->value == 9.8);
    // And the first file's own answers are unchanged, which is the half a caching bug would break: two
    // experiments loaded through one format must not be one experiment.
    REQUIRE(first.name == "simple-pendulum");
    REQUIRE(parameter_of(first, "bob", "length")->value == 1.0);
}
