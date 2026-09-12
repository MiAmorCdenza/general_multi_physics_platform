/**
 * @file test_qpjson_format.cpp
 * @brief Tests for the JSON document format.
 *
 * ## What is worth testing here
 *
 * A format has exactly one obligation that matters: **what comes back is what went in**. Everything
 * else -- size, speed, prettiness -- is negotiable, and a format that quietly loses a parameter's kind,
 * a node's bypass flag or a view's layout is worse than no format at all, because the loss surfaces
 * weeks later in a file nobody can reconstruct.
 *
 * So the cases below are the round trip in full (every value kind the format claims, every node field,
 * the edges, the layouts, the title), and then the refusals, because the refusals are what keep the
 * round trip honest: `find_unwritable` exists so that a document holding something JSON cannot express
 * is refused rather than silently trimmed.
 *
 * ## Why there is a mutation sweep
 *
 * `from_bytes` is handed whatever the user picked in a file dialog. The taxonomy in
 * `standards/test-taxonomy.md` names "arbitrary bytes must not crash" for a graph loader, and the
 * honest way to assert that is to actually feed it arbitrary bytes: every truncation of a valid
 * document, every single-byte substitution at a few positions, every possible first byte, and a few
 * degenerate inputs. A parser that indexes past the end, loops forever or recurses without bound is
 * found by that, not by a case that feeds it one wrong character.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/plugins/qpjson/qpjson_format.hpp>

#include <qp/authoring/persist.hpp>

#include <qp/diag/logging.hpp>
#include <qp/graph/ir.hpp>
#include <qp/graph/structure/graph.hpp>
#include <qp/ports/value.hpp>
#include <qp/units/dim.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace qp::authoring;

namespace {

namespace json = qp::plugins::qpjson;

/// @brief A document with a node of every value kind the format claims to carry.
///
/// Built through the public graph API rather than assembled by hand, so the fixture cannot produce a
/// shape the real code never creates -- a document that only a test can build proves nothing about the
/// ones a user has.
struct Fixture final {
    qp::graph::Graph graph{};
    ViewLayouts layouts{};
    std::string title = "Spring-damper, 200 N/m";

    qp::graph::NodeId first{};
    qp::graph::NodeId second{};
    qp::graph::NodeId pending{};

    Fixture() {
        const auto a = graph.add_node_named("demo.spring_damper", "n1");
        REQUIRE(a.has_value());
        first = a.value();

        qp::graph::Node* node = graph.find_node_mutable(first);
        REQUIRE(node != nullptr);
        node->set_param(2, qp::ports::Value{200.0});
        node->set_param(3, qp::ports::Value{std::int64_t{1}});
        node->set_param(4, qp::ports::Value{0.5f});
        node->set_param(5, qp::ports::Value{true});
        node->set_param(6, qp::ports::Value{std::string{"a text parameter"}});
        node->set_param(7, qp::ports::Value{qp::units::Dim{1, -1, 0, 0, 0, 0, 0}});
        node->bypassed = true;
        node->order_hint = -3;

        const auto b = graph.add_node("demo.incline");
        REQUIRE(b.has_value());
        second = b.value();
        qp::graph::Node* other = graph.find_node_mutable(second);
        REQUIRE(other != nullptr);
        other->set_param(1, qp::ports::Value{0.25});

        // A node that was reserved and not yet given a type. It is a real state -- the command bus
        // passes through it -- and a save during an edit has to survive.
        const auto c = graph.reserve_node();
        REQUIRE(c.has_value());
        pending = c.value();

        REQUIRE(graph.connect(qp::graph::PortRef{first, 1, qp::graph::PortDirection::output},
                             qp::graph::PortRef{second, 1, qp::graph::PortDirection::input})
                    .has_value());
        REQUIRE(graph.connect(qp::graph::PortRef{second, 1, qp::graph::PortDirection::output},
                             qp::graph::PortRef{pending, 1, qp::graph::PortDirection::input})
                    .has_value());

        layouts.set("graph", "{\"nodes\": {\"n1\": [120, 80]}}");
        layouts.set("timeseries", "");
    }
};

/// @brief Asserts two parameter values are the same value **and the same kind**.
///
/// The kind is the part that is easy to lose: writing `"value": 1` and reading it back as a double
/// would turn an integrator choice into `1.0`, and the binder's "is this an integer" test would then
/// answer differently from the run the user configured.
void require_same_value(const qp::ports::Value& lhs, const qp::ports::Value& rhs) {
    REQUIRE(lhs.kind() == rhs.kind());
    switch (lhs.kind()) {
        case qp::ports::ValueKind::invalid:
            break;
        case qp::ports::ValueKind::f64:
            REQUIRE(lhs.as_f64() == rhs.as_f64());
            break;
        case qp::ports::ValueKind::f32:
            REQUIRE(lhs.as_f32() == rhs.as_f32());
            break;
        case qp::ports::ValueKind::i64:
            REQUIRE(lhs.as_i64() == rhs.as_i64());
            break;
        case qp::ports::ValueKind::boolean:
            REQUIRE(lhs.as_bool() == rhs.as_bool());
            break;
        case qp::ports::ValueKind::text:
            REQUIRE(lhs.as_text() == rhs.as_text());
            break;
        case qp::ports::ValueKind::dimension: {
            const qp::units::Dim a = lhs.as_dimension();
            const qp::units::Dim b = rhs.as_dimension();
            REQUIRE(a.L == b.L);
            REQUIRE(a.M == b.M);
            REQUIRE(a.T == b.T);
            REQUIRE(a.I == b.I);
            REQUIRE(a.Th == b.Th);
            REQUIRE(a.N == b.N);
            REQUIRE(a.J == b.J);
            break;
        }
        case qp::ports::ValueKind::field_handle:
            FAIL("a field handle is not a value this format carries");
            break;
    }
}

/// @brief Asserts two graphs hold the same nodes (with the same handles), parameters and edges.
void require_same_graph(const qp::graph::Graph& lhs, const qp::graph::Graph& rhs) {
    REQUIRE(lhs.node_count() == rhs.node_count());
    REQUIRE(lhs.edge_count() == rhs.edge_count());
    REQUIRE(lhs.slot_count() == rhs.slot_count());
    REQUIRE(lhs.pending_count() == rhs.pending_count());

    for (const qp::graph::NodeSlot& slot : lhs.slots()) {
        if (!slot.occupied) continue;
        // Looked up by handle, which is the assertion that handles survived: an edge or a view's layout
        // refers to a node by `(index, generation)`, so a load that renumbered them would leave every
        // reference pointing at a different node.
        const qp::graph::Node* other = rhs.find_node(slot.node.id);
        REQUIRE(other != nullptr);
        REQUIRE(other->type_name == slot.node.type_name);
        REQUIRE(other->name == slot.node.name);
        REQUIRE(other->bypassed == slot.node.bypassed);
        REQUIRE(other->order_hint == slot.node.order_hint);
        REQUIRE(other->params.size() == slot.node.params.size());
        for (std::size_t i = 0; i < slot.node.params.size(); ++i) {
            REQUIRE(other->params[i].number == slot.node.params[i].number);
            require_same_value(slot.node.params[i].value, other->params[i].value);
        }
    }
    REQUIRE(lhs.edges() == rhs.edges());
}

/// @brief Writes `fixture` and asserts the write succeeded.
[[nodiscard]] std::string written(const Fixture& fixture, const json::QpJsonFormat& format) {
    std::string bytes;
    const DocumentRefusal refused =
        format.to_bytes(DocumentSource{fixture.graph, fixture.layouts, fixture.title}, bytes);
    REQUIRE(refused == DocumentRefusal::ok);
    return bytes;
}

/// @brief Whether `r` is one of the codes this format defines.
///
/// A refusal that is not a named code would mean a value came from somewhere other than the enum --
/// an uninitialised field, or a length-driven walk off the end of the parser. It is the only thing a
/// mutation sweep can assert besides "no crash", and it is worth asserting.
[[nodiscard]] bool is_a_named_refusal(DocumentRefusal r) {
    return std::string{to_string(r)} != "unknown";
}

/// @brief Wraps members in a document object with the version marker first.
[[nodiscard]] std::string document_with(std::string_view members) {
    return "{\n  \"qp_document\": 1,\n  " + std::string{members} + "\n}\n";
}

/// @brief One node with the given handle and nothing else.
[[nodiscard]] std::string node_json(std::uint32_t index, std::uint32_t generation,
                                   std::string_view type = "demo.spring_damper") {
    return "{\"index\": " + std::to_string(index) + ", \"generation\": " +
           std::to_string(generation) + ", \"type\": \"" + std::string{type} + "\"}";
}

/// @brief A graph member holding `nodes` and `edges`.
[[nodiscard]] std::string graph_json(std::string_view nodes, std::string_view edges) {
    return "\"graph\": {\"nodes\": [" + std::string{nodes} + "], \"edges\": [" +
           std::string{edges} + "]}";
}

/// @brief An edge from one output to one input, both given as index/generation/port triples.
[[nodiscard]] std::string edge_json(std::uint32_t fn, std::uint32_t fg, std::uint32_t fp,
                                   std::uint32_t tn, std::uint32_t tg, std::uint32_t tp) {
    const auto endpoint = [](std::uint32_t n, std::uint32_t g, std::uint32_t p) {
        return "{\"node\": " + std::to_string(n) + ", \"generation\": " + std::to_string(g) +
               ", \"port\": " + std::to_string(p) + "}";
    };
    return "{\"from\": " + endpoint(fn, fg, fp) + ", \"to\": " + endpoint(tn, tg, tp) + "}";
}

}  // namespace

TEST_CASE("persist.qpjson.describes_itself", "[persist][qpjson]") {
    // The description is what a "save as" list shows, what a log line records, and what a caller uses to
    // decide whether a file is worth offering. Each field is asserted because each has a consumer:
    // `name` for a record, `label` for a dialog, `extensions` for matching a picked file, `is_text` for
    // "can this go in a diff".
    const json::QpJsonFormat format;
    const DocumentFormatDesc& desc = format.format();

    REQUIRE(desc.name == "qp.document.json");
    REQUIRE_FALSE(desc.label.empty());
    REQUIRE(desc.extensions.size() == 1);
    REQUIRE(desc.extensions.front() == "qpd");
    REQUIRE(desc.is_text);

    // The same object every call: a registry holds this reference.
    REQUIRE(&format.format() == &desc);
    // And the marker the parser requires first is the one the writer emits.
    REQUIRE(std::string{json::QpJsonFormat::kMarkerKey} == "qp_document");
    REQUIRE(json::QpJsonFormat::kVersion >= 1);
}

TEST_CASE("persist.qpjson.round_trip_preserves_everything_that_matters", "[persist][qpjson]") {
    const json::QpJsonFormat format;
    const Fixture fixture;
    const std::string bytes = written(fixture, format);

    // The file is what it claims to be: text, valid UTF-8, and carrying the marker and a node type a
    // person can read. A format whose `is_text` is true and whose bytes are not valid UTF-8 would break
    // every tool that opens it.
    REQUIRE(bytes.find("\"qp_document\": 1") != std::string::npos);
    REQUIRE(bytes.find("demo.spring_damper") != std::string::npos);
    REQUIRE(bytes.find("a text parameter") != std::string::npos);
    REQUIRE(qp::diag::is_valid_utf8(bytes));
    REQUIRE(bytes.front() == '{');

    DocumentSnapshot loaded;
    REQUIRE(format.from_bytes(bytes, loaded) == DocumentRefusal::ok);

    require_same_graph(fixture.graph, loaded.graph);
    REQUIRE(loaded.title == fixture.title);
    REQUIRE(loaded.layouts.size() == fixture.layouts.size());
    REQUIRE(loaded.layouts.view_ids() == fixture.layouts.view_ids());
    REQUIRE(loaded.layouts.get("graph") == fixture.layouts.get("graph"));
    // An empty layout payload is a layout that exists and says nothing, which is different from a view
    // that has no layout at all -- the distinction the document module is built around.
    REQUIRE(loaded.layouts.has("timeseries"));
    REQUIRE(loaded.layouts.get("timeseries").empty());
    REQUIRE_FALSE(loaded.layouts.has("formula"));

    // A pending node comes back pending. Writing its empty type as an error would make a save during an
    // edit impossible; writing it as a named type would invent a node the user had not finished.
    REQUIRE(loaded.graph.is_pending(fixture.pending));
    REQUIRE(loaded.graph.pending_count() == 1);
    REQUIRE(loaded.graph.is_stable() == fixture.graph.is_stable());
}

TEST_CASE("persist.qpjson.a_save_is_byte_stable", "[persist][qpjson]") {
    // Writing the same document twice produces the same bytes. That is what lets a diff mean "this
    // changed", and it is asserted at the byte level rather than by parsing, because two documents that
    // parse equal can still churn a file that a version control system then reports as modified.
    //
    // The order is what makes it true: nodes in slot order, parameters in the order the node holds
    // them, edges in insertion order, layouts in first-set order.
    const json::QpJsonFormat format;
    const Fixture fixture;

    const std::string first = written(fixture, format);
    const std::string second = written(fixture, format);
    REQUIRE(first == second);

    // And a second write into a buffer that already held something replaces it rather than appending.
    std::string reused = "stale bytes from a previous save";
    REQUIRE(format.to_bytes(DocumentSource{fixture.graph, fixture.layouts, fixture.title}, reused) ==
            DocumentRefusal::ok);
    REQUIRE(reused == first);

    // A round trip through the bytes produces the same bytes again: nothing is lost and re-invented.
    DocumentSnapshot loaded;
    REQUIRE(format.from_bytes(first, loaded) == DocumentRefusal::ok);
    std::string again;
    REQUIRE(format.to_bytes(DocumentSource{loaded.graph, loaded.layouts, loaded.title}, again) ==
            DocumentRefusal::ok);
    REQUIRE(again == first);
}

TEST_CASE("persist.qpjson.refuses_what_json_cannot_carry", "[persist][qpjson]") {
    // Three things JSON cannot hold, and the writer refuses all three **before** producing a byte. The
    // alternative -- writing what fits and dropping the rest -- turns a save into silent data loss, and
    // the user finds out when they reopen the file and a parameter is missing.
    const json::QpJsonFormat format;

    SECTION("a field handle names data that is not in the document") {
        Fixture fixture;
        // A lattice descriptor is a real `Value`: a plugin reads a field through one. It points at a
        // buffer in this process, so a document holding it would be a document that cannot be reopened.
        qp::graph::Node* node = fixture.graph.find_node_mutable(fixture.second);
        REQUIRE(node != nullptr);
        node->set_param(9, qp::ports::Value{qp::abi::LatticeDesc{}});

        std::string out = "not empty";
        REQUIRE(format.to_bytes(DocumentSource{fixture.graph, fixture.layouts, fixture.title}, out) ==
                DocumentRefusal::value_kind_not_supported);
        REQUIRE(out.empty());
    }

    SECTION("a string that is not valid UTF-8 would be edited by escaping") {
        Fixture fixture;
        fixture.layouts.set("graph", std::string{"x\x80" "y"});   // a lone continuation byte
        std::string out;
        REQUIRE(format.to_bytes(DocumentSource{fixture.graph, fixture.layouts, fixture.title}, out) ==
                DocumentRefusal::text_not_utf8);
        REQUIRE(out.empty());

        // The same rule for a text parameter and for the title: one rule, applied to every string, which
        // is the only way a caller can rely on it.
        Fixture other;
        qp::graph::Node* node = other.graph.find_node_mutable(other.first);
        REQUIRE(node != nullptr);
        node->set_param(8, qp::ports::Value{std::string{"a\xff" "b"}});
        REQUIRE(format.to_bytes(DocumentSource{other.graph, other.layouts, other.title}, out) ==
                DocumentRefusal::text_not_utf8);

        Fixture titled;
        const std::string bad_title = "spring\xc3";
        REQUIRE(format.to_bytes(DocumentSource{titled.graph, titled.layouts, bad_title}, out) ==
                DocumentRefusal::text_not_utf8);
    }

    SECTION("an infinity or a NaN has no JSON literal") {
        // `to_chars` would write "inf" or "nan", neither of which is JSON, and substituting `null` would
        // load as a value nobody computed. A diverged run can put either into a parameter, so this is a
        // state the tool has to answer for rather than a hypothetical.
        Fixture fixture;
        qp::graph::Node* node = fixture.graph.find_node_mutable(fixture.first);
        REQUIRE(node != nullptr);
        node->set_param(20, qp::ports::Value{std::numeric_limits<double>::infinity()});

        std::string out;
        REQUIRE(format.to_bytes(DocumentSource{fixture.graph, fixture.layouts, fixture.title}, out) ==
                DocumentRefusal::non_finite_number);
        REQUIRE(out.empty());

        node->set_param(20, qp::ports::Value{std::numeric_limits<double>::quiet_NaN()});
        REQUIRE(format.to_bytes(DocumentSource{fixture.graph, fixture.layouts, fixture.title}, out) ==
                DocumentRefusal::non_finite_number);
    }

    SECTION("an unset parameter is written, not refused") {
        // The distinction this whole platform exists to keep: "never measured" is not "measured zero".
        // A parameter that was never set has no `value` member in the file, and it comes back unset --
        // refusing to save a half-filled node would make work in progress unsaveable.
        Fixture fixture;
        qp::graph::Node* node = fixture.graph.find_node_mutable(fixture.second);
        REQUIRE(node != nullptr);
        node->set_param(11, qp::ports::Value{});

        const std::string bytes = written(fixture, format);
        REQUIRE(bytes.find("\"kind\": \"invalid\"") != std::string::npos);

        DocumentSnapshot loaded;
        REQUIRE(format.from_bytes(bytes, loaded) == DocumentRefusal::ok);
        const qp::graph::Node* restored = loaded.graph.find_node(fixture.second);
        REQUIRE(restored != nullptr);
        REQUIRE(restored->param(11).valid() == false);
    }
}

TEST_CASE("persist.qpjson.refuses_a_foreign_file_and_a_broken_one", "[persist][qpjson]") {
    // "This is not a document" and "this document is broken" are different answers with different fixes,
    // which is why the marker exists and why it is required first. The remaining cases pin the boundary
    // between the two: everything after a valid marker is the document's problem, everything before it
    // is the file's.
    const json::QpJsonFormat format;
    DocumentSnapshot snapshot;

    SECTION("not a document at all") {
        const std::string_view foreign[] = {
            "",                       // empty
            "   \n\t ",               // whitespace only
            "[]",                     // a JSON array
            "hello",                  // not JSON
            "{}",                     // a JSON object, but not this one
            "{\"title\": \"x\"}",     // a document-shaped object without the marker
            "{\"qp_document_extra\": 1}",   // a prefix of the marker is not the marker
        };
        for (const std::string_view text : foreign) {
            REQUIRE(format.from_bytes(text, snapshot) != DocumentRefusal::ok);
        }
        REQUIRE(format.from_bytes("{}", snapshot) == DocumentRefusal::not_a_document);
        REQUIRE(format.from_bytes("[]", snapshot) == DocumentRefusal::not_a_document);
        REQUIRE(format.from_bytes("garbage", snapshot) == DocumentRefusal::not_a_document);
        REQUIRE(format.from_bytes("", snapshot) == DocumentRefusal::truncated);
        // A snapshot is untouched by any of it.
        REQUIRE(snapshot.graph.node_count() == 0);
        REQUIRE(snapshot.title.empty());
    }

    SECTION("a version this build does not read") {
        // Distinct from malformed on purpose: the file is fine and this build is older than it, and the
        // fix is to open it with a newer build rather than to look for a syntax error.
        REQUIRE(format.from_bytes("{\"qp_document\": 2, \"graph\": {\"nodes\": [], \"edges\": []}}",
                                  snapshot) == DocumentRefusal::unsupported_version);
        REQUIRE(format.from_bytes("{\"qp_document\": 99}", snapshot) ==
                DocumentRefusal::unsupported_version);
        // Version 0 and negative versions are not "older", they are not versions.
        REQUIRE(format.from_bytes("{\"qp_document\": 0}", snapshot) == DocumentRefusal::malformed);
        REQUIRE(format.from_bytes("{\"qp_document\": -1}", snapshot) == DocumentRefusal::malformed);
    }

    SECTION("a well-formed document this version does not define") {
        // Unknown members are refused rather than skipped, and that is the decision the version marker
        // pays for: skipping would load the part the two agree on, which is a document that is not the
        // one that was saved.
        const std::string cases[] = {
            "{\"qp_document\": 1, \"colour\": \"blue\"}",
            document_with("\"graph\": {\"nodes\": [], \"edges\": []}, \"colour\": \"blue\""),
            document_with("\"graph\": {\"nodes\": [], \"edges\": [], \"weights\": []}"),
            document_with("\"graph\": {\"nodes\": [{\"index\": 1, \"generation\": 1, "
                          "\"type\": \"x\", \"colour\": \"blue\"}], \"edges\": []}"),
            document_with("\"graph\": \"not an object\""),
            document_with("\"title\": 5, \"graph\": {\"nodes\": [], \"edges\": []}"),
            // No graph at all: not an empty document, a file this format does not define.
            document_with("\"title\": \"only a title\""),
            // Bytes after the document ends.
            document_with("\"graph\": {\"nodes\": [], \"edges\": []}") + "trailing",
        };
        for (const std::string& text : cases) {
            REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::malformed);
        }
    }

    SECTION("a document whose parts break the model's own invariants") {
        // Each of these would otherwise be resolved by *replacing* something silently on load.
        const std::string cases[] = {
            // Two parameters for one port: `set_param` would keep the last and drop the first.
            document_with(graph_json("{\"index\": 1, \"generation\": 1, \"type\": \"x\", "
                                     "\"params\": [{\"port\": 2, \"kind\": \"f64\", \"value\": 1}, "
                                     "{\"port\": 2, \"kind\": \"f64\", \"value\": 2}]}",
                                     "")),
            // Two layouts for one view: `set` would keep the last.
            document_with("\"graph\": {\"nodes\": [], \"edges\": []}, \"layouts\": ["
                          "{\"view\": \"graph\", \"text\": \"a\"}, "
                          "{\"view\": \"graph\", \"text\": \"b\"}]"),
            // A value member before the kind that says how to read it.
            document_with(graph_json("{\"index\": 1, \"generation\": 1, \"type\": \"x\", "
                                     "\"params\": [{\"port\": 2, \"value\": 1, "
                                     "\"kind\": \"f64\"}]}",
                                     "")),
            // A kind with no payload, and a payload for a kind that has none.
            document_with(graph_json("{\"index\": 1, \"generation\": 1, \"type\": \"x\", "
                                     "\"params\": [{\"port\": 2, \"kind\": \"f64\"}]}",
                                     "")),
            document_with(graph_json("{\"index\": 1, \"generation\": 1, \"type\": \"x\", "
                                     "\"params\": [{\"port\": 2, \"kind\": \"invalid\", "
                                     "\"value\": 1}]}",
                                     "")),
            // A node with no identity, and a handle of zero.
            document_with(graph_json("{\"type\": \"x\"}", "")),
            document_with(graph_json("{\"index\": 0, \"generation\": 1, \"type\": \"x\"}", "")),
            document_with(graph_json(node_json(1, 1) + ", {\"index\": 2, \"type\": \"x\"}", "")),
            // A kind this format does not define.
            document_with(graph_json("{\"index\": 1, \"generation\": 1, \"type\": \"x\", "
                                     "\"params\": [{\"port\": 2, \"kind\": \"complex\", "
                                     "\"value\": 1}]}",
                                     "")),
        };
        for (const std::string& text : cases) {
            REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::malformed);
        }
    }

    SECTION("a file that was cut short") {
        // Reported as truncated rather than malformed, because the user's problem is a copy that ran out
        // of disk or a download that stopped, and the fix is to find the original.
        //
        // Every proper prefix is either incomplete or does not yet hold the whole marker; the one
        // exception is a prefix that drops only trailing whitespace, which is the document itself.
        const Fixture fixture;
        const std::string bytes = written(fixture, format);
        const std::size_t last = bytes.find_last_not_of(" \t\n\r");
        for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
            const std::string prefix = bytes.substr(0, cut);
            const DocumentRefusal refused = format.from_bytes(prefix, snapshot);
            if (prefix.find_last_not_of(" \t\n\r") == last) continue;
            REQUIRE(refused != DocumentRefusal::ok);
            REQUIRE((refused == DocumentRefusal::truncated || refused == DocumentRefusal::malformed ||
                     refused == DocumentRefusal::not_a_document));
        }
    }

    SECTION("a document that holds the wrong kind of bytes") {
        // A `field` parameter is named in the file so the refusal can be a sentence rather than "unknown
        // kind": a build that could carry a handle wrote it, and this one cannot.
        const std::string text =
            document_with(graph_json("{\"index\": 1, \"generation\": 1, \"type\": \"x\", "
                                     "\"params\": [{\"port\": 2, \"kind\": \"field\"}]}",
                                     ""));
        REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::value_kind_not_supported);
    }
}

TEST_CASE("persist.qpjson.rejects_a_graph_with_a_cycle", "[persist][qpjson]") {
    // The graph the document describes is put back through `restore_node` and `restore_edge`, which are
    // the same functions undo uses, so the document cannot introduce a shape the live graph refuses.
    // Each refusal has its own name because each sends the user to a different place in the file.
    const json::QpJsonFormat format;
    DocumentSnapshot snapshot;

    SECTION("a cycle") {
        // Two nodes, and edges in both directions. The evaluator cannot order a cycle, so loading one
        // would mean evaluating nodes in an arbitrary order and believing the result.
        const std::string text = document_with(graph_json(
            node_json(1, 1) + ", " + node_json(2, 1),
            edge_json(1, 1, 1, 2, 1, 1) + ", " + edge_json(2, 1, 1, 1, 1, 1)));
        REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::cyclic_graph);
    }

    SECTION("a self loop") {
        const std::string text =
            document_with(graph_json(node_json(1, 1), edge_json(1, 1, 1, 1, 1, 1)));
        REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::cyclic_graph);
    }

    SECTION("an edge to a node the document does not hold") {
        const std::string text =
            document_with(graph_json(node_json(1, 1), edge_json(1, 1, 1, 9, 1, 1)));
        REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::dangling_edge);
    }

    SECTION("two edges into one input port") {
        const std::string text = document_with(graph_json(
            node_json(1, 1) + ", " + node_json(2, 1) + ", " + node_json(3, 1),
            edge_json(1, 1, 1, 3, 1, 1) + ", " + edge_json(2, 1, 1, 3, 1, 1)));
        REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::duplicate_edge);
    }

    SECTION("two nodes claiming one handle") {
        // The same index **and** generation: a second node under a handle that is already taken.
        const std::string text =
            document_with(graph_json(node_json(1, 1) + ", " + node_json(1, 1), ""));
        REQUIRE(format.from_bytes(text, snapshot) == DocumentRefusal::duplicate_node);
    }

    SECTION("a snapshot that already held a document is left alone") {
        // A load that failed must not leave the caller holding half of the file: it is parsed into a
        // local snapshot and moved into place only on success.
        const Fixture fixture;
        const json::QpJsonFormat writer;
        DocumentSnapshot existing;
        REQUIRE(writer.from_bytes(written(fixture, writer), existing) == DocumentRefusal::ok);
        const std::size_t before = existing.graph.node_count();

        const std::string cycle = document_with(graph_json(
            node_json(1, 1) + ", " + node_json(2, 1),
            edge_json(1, 1, 1, 2, 1, 1) + ", " + edge_json(2, 1, 1, 1, 1, 1)));
        REQUIRE(format.from_bytes(cycle, existing) == DocumentRefusal::cyclic_graph);
        REQUIRE(existing.graph.node_count() == before);
        REQUIRE(existing.graph.edge_count() == fixture.graph.edge_count());
    }
}

TEST_CASE("persist.qpjson.arbitrary_bytes_never_crash", "[persist][qpjson]") {
    // `from_bytes` is handed the contents of a file the user picked, and "that file is not a document"
    // is an ordinary answer. What must never happen is a crash, a hang, or a claim of success -- the
    // contract says a code comes back for **any** input, and this is the case that holds it to that.
    const json::QpJsonFormat format;
    const Fixture fixture;
    const std::string valid = written(fixture, format);

    SECTION("every truncation of a valid document") {
        // The one case a hand-written test would miss and a disk-full copy produces.
        //
        // A prefix that still holds every non-whitespace byte is the whole document with its trailing
        // newline dropped, and it has to load. Every other prefix is a file that was cut short, and none
        // of them may be accepted: a parser that stopped at the first complete-looking member would hand
        // back a graph missing whatever came after the cut, and the user would have no way to tell that
        // from a graph that is genuinely small.
        const std::size_t last = valid.find_last_not_of(" \t\n\r");
        for (std::size_t cut = 0; cut < valid.size(); ++cut) {
            const std::string prefix = valid.substr(0, cut);
            DocumentSnapshot snapshot;
            const DocumentRefusal refused = format.from_bytes(prefix, snapshot);
            if (prefix.find_last_not_of(" \t\n\r") == last) {
                REQUIRE(refused == DocumentRefusal::ok);
            } else {
                REQUIRE(refused != DocumentRefusal::ok);
            }
        }
    }

    SECTION("every possible first byte") {
        // 256 inputs, each one byte long, plus each of them followed by a fragment of a real document: a
        // parser that switches on the first byte rather than comparing it finds this.
        for (int b = 0; b < 256; ++b) {
            const std::string one(1, static_cast<char>(b));
            DocumentSnapshot snapshot;
            REQUIRE(format.from_bytes(one, snapshot) != DocumentRefusal::ok);

            const std::string tail = one + valid.substr(1);
            if (b == '{') {
                // The one byte that leaves the document untouched: `{` replaced by `{`.
                REQUIRE(format.from_bytes(tail, snapshot) == DocumentRefusal::ok);
            } else {
                REQUIRE(format.from_bytes(tail, snapshot) != DocumentRefusal::ok);
            }
        }
    }

    SECTION("a single byte changed anywhere in the document") {
        // Substitutions rather than deletions, so the length stays the same and a length-driven parser
        // still walks off the end. The substituted bytes are the ones that end a token, open a
        // container, or begin an escape -- each is a place a scanner can go wrong.
        //
        // The assertion is weaker here than elsewhere, and deliberately: a substitution inside a number
        // or a string can leave a document that still parses (`200.0` with one digit replaced is another
        // number), so "everything is refused" would be false. What is asserted is the contract's other
        // half -- a *named* code always comes back -- which is what catches a parser that returns an
        // uninitialised verdict or falls off the end of its own logic.
        const char hostile[] = {'"', '\\', '{', '}', '[', ']', ':', ',', '0', 'e', 'n', '\x00', '\xff'};
        for (std::size_t i = 0; i < valid.size(); i += 7) {
            for (const char c : hostile) {
                std::string mutated = valid;
                mutated[i] = c;
                DocumentSnapshot snapshot;
                REQUIRE(is_a_named_refusal(format.from_bytes(mutated, snapshot)));
            }
        }
    }

    SECTION("degenerate inputs") {
        const std::string braces(10000, '{');
        const std::string brackets(10000, '[');
        const std::string nested = "{\"qp_document\": 1, \"graph\": " + std::string(5000, '[') + "}";
        const std::string deep = "{\"qp_document\": 1, \"graph\": {\"nodes\": [" +
                                 std::string(2000, '{') + "]}}";
        const std::string long_string = "{\"qp_document\": 1, \"title\": \"" + std::string(100000, 'a') +
                                        "\", \"graph\": {\"nodes\": [], \"edges\": []}}";
        const std::string not_utf8 = std::string("{\"qp_document\": 1, \"title\": \"") +
                                     std::string("\x80\x80\x80", 3) + "\"}";

        DocumentSnapshot snapshot;
        REQUIRE(format.from_bytes(braces, snapshot) != DocumentRefusal::ok);
        REQUIRE(format.from_bytes(brackets, snapshot) != DocumentRefusal::ok);
        REQUIRE(format.from_bytes(nested, snapshot) != DocumentRefusal::ok);
        REQUIRE(format.from_bytes(deep, snapshot) != DocumentRefusal::ok);
        REQUIRE(format.from_bytes(not_utf8, snapshot) != DocumentRefusal::ok);
        // A long string is only long. It parses, and the title comes back whole.
        REQUIRE(format.from_bytes(long_string, snapshot) == DocumentRefusal::ok);
        REQUIRE(snapshot.title.size() == 100000);
    }

    SECTION("an empty document is a document") {
        // The other side of the same coin: refusing everything unusual would be easy and useless. An
        // empty graph with no title and no layouts is what a new document is.
        DocumentSnapshot snapshot;
        REQUIRE(format.from_bytes("{\"qp_document\": 1, \"graph\": {\"nodes\": [], \"edges\": []}}",
                                  snapshot) == DocumentRefusal::ok);
        REQUIRE(snapshot.graph.node_count() == 0);
        REQUIRE(snapshot.layouts.empty());
        REQUIRE(snapshot.title.empty());
    }
}
