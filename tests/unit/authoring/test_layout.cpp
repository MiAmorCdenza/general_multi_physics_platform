/**
 * @file test_layout.cpp
 * @brief Tests for the layout contract and its algorithm registry.
 *
 * Test case ids match the @tests fields in the layout headers byte for byte.
 *
 * The test algorithms here are deliberately trivial -- a grid and a constant --
 * because this module must not contain a real layout algorithm any more than
 * `graph/kernels` may contain an integrator. What is under test is the contract:
 * a result covers the graph or fails, positions are exact integers, the same seed
 * reproduces the same result, and the registry refuses a name collision.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/authoring/layout.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace qp::authoring;
using namespace qp::graph;

namespace {

/// @brief Places nodes left to right on a fixed grid. Deterministic by construction.
class GridLayout final : public ILayoutAlgorithm {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test.grid"; }

    [[nodiscard]] qp::diag::Result<LayoutResult> compute(const Graph& graph,
                                                         std::uint64_t /*seed*/) override {
        LayoutResult result;
        std::int32_t column = 0;
        for (const NodeSlot& slot : graph.slots()) {
            // Slot 0 is a sentinel and a freed slot keeps its generation with an
            // invalid id, so "occupied" is a test on the id, not on the index.
            if (!slot.node.id.valid()) continue;
            result.placements.push_back(Placement{slot.node.id, Point{column * 200, 100}});
            ++column;
        }
        return result;
    }
};

/// @brief Places every node at the origin, and folds the seed into the result.
///
/// Stands in for a force-directed algorithm: the point is that the seed reaches
/// the algorithm and comes back out in the positions.
class SeededLayout final : public ILayoutAlgorithm {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test.seeded"; }

    [[nodiscard]] qp::diag::Result<LayoutResult> compute(const Graph& graph,
                                                         std::uint64_t seed) override {
        LayoutResult result;
        const auto offset = static_cast<std::int32_t>(seed % 1000);
        for (const NodeSlot& slot : graph.slots()) {
            if (!slot.node.id.valid()) continue;
            result.placements.push_back(Placement{slot.node.id, Point{offset, offset}});
        }
        return result;
    }
};

/// @brief Declines to lay out anything, to exercise the failure path.
class RefusingLayout final : public ILayoutAlgorithm {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "test.refusing"; }

    [[nodiscard]] qp::diag::Result<LayoutResult> compute(const Graph&,
                                                         std::uint64_t) override {
        return qp::diag::ErrorCode::not_implemented;
    }
};

/// @brief Builds a graph with `n` nodes named n0, n1, ...
Graph make_graph(int n) {
    Graph g;
    for (int i = 0; i < n; ++i) {
        (void)g.add_node("type" + std::to_string(i));
    }
    return g;
}

/// @brief The ids of every live node, in slot order.
std::vector<NodeId> node_ids(const Graph& g) {
    std::vector<NodeId> out;
    for (const NodeSlot& slot : g.slots()) {
        if (slot.node.id.valid()) out.push_back(slot.node.id);
    }
    return out;
}

}  // namespace

// ===========================================================================
// The contract
// ===========================================================================

TEST_CASE("authoring.layout.compute_returns_positions", "[authoring]") {
    const Graph g = make_graph(3);
    GridLayout grid;

    auto computed = grid.compute(g, 0);
    REQUIRE(computed.has_value());
    const LayoutResult& result = computed.value();

    // A layout that does not cover the graph is not usable: the view would have
    // to invent positions for the rest, and two views would invent differently.
    REQUIRE(result.size() == 3);
    REQUIRE(result.covers(g.node_count()));

    // The ids come from the graph rather than from an assumption about slot
    // numbering: slot 0 is a sentinel and a generation is not always 1, and a test
    // that hard-coded either would break the moment the allocator changed.
    const std::vector<NodeId> ids = node_ids(g);
    REQUIRE(ids.size() == 3);
    REQUIRE(result.position_of(ids[0]) == Point{0, 100});
    REQUIRE(result.position_of(ids[1]) == Point{200, 100});
    REQUIRE(result.position_of(ids[2]) == Point{400, 100});

    // An unplaced node reads as the origin rather than failing: a caller drawing
    // a graph asks about every node it is about to draw, and one absent placement
    // should not abort the draw.
    REQUIRE(result.position_of(NodeId{999, 1}) == Point{0, 0});

    // Positions are integers, so the saved document is byte-stable. This is the
    // reason the type is not a floating-point pair.
    REQUIRE(sizeof(Point::x) == 4);

    // The failure path reports an error rather than an empty result, so a caller
    // can tell "this algorithm cannot place your graph" from "your graph is
    // empty".
    RefusingLayout refusing;
    const auto refused = refusing.compute(g, 0);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == qp::diag::ErrorCode::not_implemented);
}

TEST_CASE("authoring.layout.compute_is_deterministic", "[authoring]") {
    const Graph g = make_graph(4);

    // Same graph and same seed, same result. A layout that read a clock or an
    // unseeded RNG would make the saved document differ between two runs of the
    // same experiment, and a diff of two documents would stop meaning "the
    // physics changed".
    SeededLayout seeded;
    const auto first = seeded.compute(g, 20260911);
    const auto second = seeded.compute(g, 20260911);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first.value().size() == second.value().size());
    for (std::size_t i = 0; i < first.value().size(); ++i) {
        REQUIRE(first.value().placements[i].id == second.value().placements[i].id);
        REQUIRE(first.value().placements[i].position == second.value().placements[i].position);
    }

    // A different seed is allowed to differ -- and here it does, which is what
    // proves the seed actually reaches the algorithm rather than being ignored.
    const std::vector<NodeId> ids = node_ids(g);
    REQUIRE_FALSE(ids.empty());
    const auto other = seeded.compute(g, 20260912);
    REQUIRE(other.has_value());
    REQUIRE(other.value().position_of(ids[0]) != first.value().position_of(ids[0]));

    // An empty graph is not an error: there is nothing to place, and a result
    // with no placements covers it exactly.
    const Graph empty;
    const auto nothing = seeded.compute(empty, 1);
    REQUIRE(nothing.has_value());
    REQUIRE(nothing.value().size() == 0);
    REQUIRE(nothing.value().covers(empty.node_count()));
}

// ===========================================================================
// The registry
// ===========================================================================

TEST_CASE("authoring.layout.registry_register_and_find", "[authoring]") {
    LayoutRegistry registry;
    REQUIRE(registry.size() == 0);
    REQUIRE(registry.find_by_name("test.grid") == nullptr);

    GridLayout grid;
    auto added = registry.add(LayoutDesc{"test.grid", "A fixed grid", &grid});
    REQUIRE(added.has_value());
    const LayoutId id = added.value();
    REQUIRE(id.valid());
    REQUIRE(registry.size() == 1);

    const LayoutDesc* found = registry.find_by_name("test.grid");
    REQUIRE(found != nullptr);
    REQUIRE(found->impl == &grid);
    REQUIRE(found->summary == "A fixed grid");
    REQUIRE(registry.find(id) == found);

    // The module ships no algorithm of its own: this is a registry, and an empty
    // one stays empty. A grid algorithm living here would be an integrator living
    // in kernels.
    LayoutRegistry pristine;
    REQUIRE(pristine.list().empty());
    REQUIRE(pristine.find_by_name("qp.layout.layered") == nullptr);

    // Listing is in registration order, so a chooser lists algorithms in the
    // order the user's plugins loaded.
    SeededLayout seeded;
    REQUIRE(registry.add(LayoutDesc{"test.seeded", "", &seeded}).has_value());
    REQUIRE(registry.list().size() == 2);
    REQUIRE(registry.list()[0].name == "test.grid");
    REQUIRE(registry.list()[1].name == "test.seeded");
}

TEST_CASE("authoring.layout.registry_duplicate_is_refused", "[authoring]") {
    LayoutRegistry registry;
    GridLayout grid;
    SeededLayout seeded;
    REQUIRE(registry.add(LayoutDesc{"test.grid", "", &grid}).has_value());

    SECTION("two algorithms cannot share a name") {
        // Refusing rather than overwriting: with two algorithms under one name,
        // "lay out this document" would depend on plugin load order, so the same
        // document would be arranged differently on two machines.
        const auto clash = registry.add(LayoutDesc{"test.grid", "other", &seeded});
        REQUIRE_FALSE(clash.has_value());
        REQUIRE(clash.error() == qp::diag::ErrorCode::duplicate_connection);
        REQUIRE(registry.size() == 1);
        REQUIRE(registry.find_by_name("test.grid")->impl == &grid);
    }

    SECTION("an invalid description is refused") {
        REQUIRE_FALSE(registry.add(LayoutDesc{"", "", &grid}).has_value());
        REQUIRE_FALSE(registry.add(LayoutDesc{"test.null", "", nullptr}).has_value());
        REQUIRE(registry.size() == 1);
    }
}

TEST_CASE("authoring.layout.registry_remove", "[authoring]") {
    LayoutRegistry registry;
    GridLayout grid;
    SeededLayout seeded;
    const auto first = registry.add(LayoutDesc{"test.grid", "", &grid});
    const auto second = registry.add(LayoutDesc{"test.seeded", "", &seeded});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    REQUIRE(registry.remove(first.value()).has_value());

    // The removed algorithm's name is free again, which is what makes a plugin
    // reload possible. Leaving the name taken would make an unloaded plugin
    // permanently unloadable.
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.find_by_name("test.grid") == nullptr);
    REQUIRE(registry.find(first.value()) == nullptr);
    REQUIRE(registry.find_by_name("test.seeded") != nullptr);
    REQUIRE(registry.add(LayoutDesc{"test.grid", "", &grid}).has_value());

    // Removing twice is reported, not ignored: an unload path that runs twice has
    // a bug worth noticing.
    REQUIRE_FALSE(registry.remove(first.value()).has_value());
    REQUIRE_FALSE(registry.remove(LayoutId{}).has_value());
}
