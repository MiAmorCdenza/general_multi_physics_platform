/**
 * @file test_persist.cpp
 * @brief Tests for the document persistence contract.
 *
 * ## What is worth testing here
 *
 * The module is an interface and two value types, so the properties that matter are the ones a format
 * implementation could get wrong and the platform would have no way to notice:
 *
 *   - a refusal code round-trips through `to_string` and stays distinct, because a log line that maps
 *     two problems to one name is a log line that sends the reader to the wrong fix;
 *   - `DocumentSnapshot` is move-only and stays usable after a move, because `Graph` is move-only and a
 *     snapshot that copied would give two graphs whose handles both claim to be valid;
 *   - `DocumentSource` cannot be constructed without both parts, which is the whole reason it holds
 *     references rather than pointers.
 *
 * The format itself is tested where it lives -- `tests/unit/plugins/test_qpjson_format.cpp` -- because a
 * stub here would only test the stub. What this file uses is the smallest format that can answer the
 * interface at all, so the interface's own guarantees are exercised rather than described.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/authoring/persist.hpp>

#include <qp/authoring/document.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace qp::authoring;

namespace {

/// @brief A format that stores nothing and succeeds, for the interface's own guarantees.
///
/// Deliberately not a real format: it exists so `IDocumentFormat` can be called through a reference,
/// which is how every caller will hold one -- a plugin's object, reached through the interface. A test
/// that only ever named the concrete format would not notice if the interface stopped being usable.
class NullFormat final : public IDocumentFormat {
public:
    [[nodiscard]] const DocumentFormatDesc& format() const noexcept override {
        static const DocumentFormatDesc desc = [] {
            DocumentFormatDesc d;
            d.name = "test.null";
            d.label = "Nothing at all";
            d.extensions = {"null"};
            d.is_text = true;
            return d;
        }();
        return desc;
    }

    [[nodiscard]] DocumentRefusal to_bytes(const DocumentSource&, std::string& out) const noexcept override {
        out = "null";
        return DocumentRefusal::ok;
    }

    [[nodiscard]] DocumentRefusal from_bytes(std::string_view bytes,
                                            DocumentSnapshot& out) const noexcept override {
        if (bytes != "null") return DocumentRefusal::not_a_document;
        out.title = "loaded";
        return DocumentRefusal::ok;
    }
};

}  // namespace

TEST_CASE("persist.refusal.codes_are_named", "[persist]") {
    // Every code has a name, the names are distinct, and `ok` is the zero value. The last one matters
    // for a different reason than the first two: a zero-initialised refusal meaning "failed" would make
    // every default-constructed result a failure, and a zero-initialised *success* is what lets a
    // struct of results be value-initialised and then filled in.
    const DocumentRefusal all[] = {
        DocumentRefusal::ok,
        DocumentRefusal::not_a_document,
        DocumentRefusal::unsupported_version,
        DocumentRefusal::malformed,
        DocumentRefusal::truncated,
        DocumentRefusal::cyclic_graph,
        DocumentRefusal::duplicate_node,
        DocumentRefusal::duplicate_edge,
        DocumentRefusal::dangling_edge,
        DocumentRefusal::text_not_utf8,
        DocumentRefusal::value_kind_not_supported,
        DocumentRefusal::non_finite_number,
    };

    REQUIRE(static_cast<int>(DocumentRefusal::ok) == 0);
    for (const DocumentRefusal r : all) {
        const std::string name = to_string(r);
        REQUIRE_FALSE(name.empty());
        REQUIRE(name != "unknown");
        for (const DocumentRefusal other : all) {
            if (other == r) continue;
            REQUIRE(name != to_string(other));
        }
    }

    // An unnamed value does not pretend to be one of the named ones. A cast from an integer is how a
    // value from a file or a log would arrive, so the answer has to exist.
    REQUIRE(std::string{to_string(static_cast<DocumentRefusal>(200))} == "unknown");
}

TEST_CASE("persist.format.describes_itself", "[persist]") {
    // The description is what a "save as" list shows and what a log line records, so it has to be
    // complete enough to be used for either. Empty extensions would put a format in a list that cannot
    // be matched to a file, which is the same as not being registered at all.
    const NullFormat format;
    const IDocumentFormat& as_interface = format;
    const DocumentFormatDesc& desc = as_interface.format();

    REQUIRE(desc.name == "test.null");
    REQUIRE_FALSE(desc.label.empty());
    REQUIRE_FALSE(desc.extensions.empty());
    REQUIRE(desc.extensions.front() == "null");
    REQUIRE(desc.is_text);

    // The same object every call: a registry holds the reference, so a description rebuilt per call
    // would leave it pointing at a temporary.
    REQUIRE(&as_interface.format() == &desc);
}

TEST_CASE("persist.format.writes_and_reads_through_the_interface", "[persist]") {
    // Both directions reached through a base reference, which is how a plugin's format is used. Writing
    // clears `out` first: a caller that reused a buffer would otherwise append a document to whatever
    // was in it, and the failure would look like a corrupt file rather than a caller's mistake.
    const NullFormat format;
    const IDocumentFormat& as_interface = format;

    qp::graph::Graph graph;
    const ViewLayouts layouts;

    std::string bytes = "stale contents";
    REQUIRE(as_interface.to_bytes(DocumentSource{graph, layouts, "title"}, bytes) == DocumentRefusal::ok);
    REQUIRE(bytes == "null");

    DocumentSnapshot snapshot;
    REQUIRE(as_interface.from_bytes("null", snapshot) == DocumentRefusal::ok);
    REQUIRE(snapshot.title == "loaded");

    // A refusal leaves the snapshot as it was rather than half-filled.
    REQUIRE(as_interface.from_bytes("not a document", snapshot) == DocumentRefusal::not_a_document);
    REQUIRE(snapshot.title == "loaded");
}

TEST_CASE("persist.snapshot.is_move_only_and_survives_a_move", "[persist]") {
    // `graph::Graph` is move-only by design -- copying it would copy generation counters, and the two
    // copies' handles would both claim to be valid -- so a snapshot that held one has to be move-only
    // too. `DocumentSnapshot` deletes its copy operations explicitly rather than leaving them
    // implicitly deleted, so the compiler error names the class rather than `Graph`.
    REQUIRE_FALSE(std::is_copy_constructible_v<DocumentSnapshot>);
    REQUIRE_FALSE(std::is_copy_assignable_v<DocumentSnapshot>);
    REQUIRE(std::is_move_constructible_v<DocumentSnapshot>);
    REQUIRE(std::is_move_assignable_v<DocumentSnapshot>);

    // A default-constructed snapshot is a valid empty document: that is what "new document" means, and
    // a caller should not have to construct anything to get one.
    DocumentSnapshot first;
    REQUIRE(first.graph.node_count() == 0);
    REQUIRE(first.graph.edge_count() == 0);
    REQUIRE(first.layouts.empty());
    REQUIRE(first.title.empty());

    first.title = "moved";
    const auto node = first.graph.add_node("demo.spring_damper");
    REQUIRE(node.has_value());
    first.layouts.set("graph", "{\"x\":1}");

    DocumentSnapshot second = std::move(first);
    REQUIRE(second.title == "moved");
    REQUIRE(second.graph.node_count() == 1);
    REQUIRE(second.layouts.size() == 1);
    REQUIRE(second.layouts.get("graph") == "{\"x\":1}");

    // And a move assignment replaces what was there, so a caller can reuse one snapshot for a sequence
    // of loads without accumulating state from the previous document.
    DocumentSnapshot third;
    third.title = "stale";
    third = std::move(second);
    REQUIRE(third.title == "moved");
    REQUIRE(third.graph.node_count() == 1);
}

TEST_CASE("persist.source.borrows_both_parts", "[persist]") {
    // References rather than pointers: "a source with no graph" is not a state that has to be checked
    // for, because it cannot be built. What is worth asserting is the other half of that decision --
    // the source *observes*, so writing it does not copy the graph, and a change to the graph after the
    // source was made is visible through it.
    qp::graph::Graph graph;
    ViewLayouts layouts;
    const DocumentSource source{graph, layouts, "before"};

    REQUIRE(source.title == "before");
    REQUIRE(&source.graph == &graph);
    REQUIRE(&source.layouts == &layouts);

    const auto node = graph.add_node("demo.spring_damper");
    REQUIRE(node.has_value());
    layouts.set("graph", "layout");

    // Through the same source object: nothing was copied at construction.
    REQUIRE(source.graph.node_count() == 1);
    REQUIRE(source.layouts.get("graph") == "layout");
}
