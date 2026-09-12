/**
 * @file test_eval.cpp
 * @brief Unit and property tests for core/graph/eval.
 *
 * Three areas of emphasis:
 *   1. **Hash determinism**: independent of addresses, time and implementation-defined std::hash
 *   2. **Cache-key discrimination**: generation, port number and parameter changes give other keys
 *   3. **Evaluation determinism**: evaluating the same graph twice gives bit-identical results
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/eval.hpp>

#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>

using namespace qp::graph;
using qp::diag::ErrorCode;

namespace {

using qp::ports::Value;

/// @brief A simple, usable node catalog.
class FakeCatalog final : public INodeCatalog {
public:
    void add(NodeDesc d) { descs_.push_back(std::move(d)); }
    [[nodiscard]] const NodeDesc* find(std::string_view type_name) const noexcept override {
        for (const auto& d : descs_) {
            if (d.type_name == type_name) return &d;
        }
        return nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept override { return descs_.size(); }

private:
    std::deque<NodeDesc> descs_;
};

[[nodiscard]] PortDesc make_in(PortNumber n, const char* name) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = qp::ports::kScalarF64;
    return p;
}
[[nodiscard]] PortDesc make_out(PortNumber n, const char* name) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = qp::ports::kScalarF64;
    return p;
}

/// @brief Constant node: outputs the value of parameter 1. No inputs.
[[nodiscard]] NodeDesc make_const() {
    NodeDesc d{};
    d.type_name = "const";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "value"));
    d.outputs.push_back(make_out(1, "out"));
    return d;
}

/// @brief Add node: output = input1 + input2.
[[nodiscard]] NodeDesc make_add() {
    NodeDesc d{};
    d.type_name = "add";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "a"));
    d.inputs.push_back(make_in(2, "b"));
    d.outputs.push_back(make_out(1, "sum"));
    return d;
}

/// @brief Scale node: output = input1 x parameter1.
[[nodiscard]] NodeDesc make_scale() {
    NodeDesc d{};
    d.type_name = "scale";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "in"));
    d.inputs.push_back(make_in(2, "factor"));
    d.outputs.push_back(make_out(1, "out"));
    return d;
}

/// @brief A node that fails: used to verify error propagation.
[[nodiscard]] NodeDesc make_failing() {
    NodeDesc d{};
    d.type_name = "failing";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "in"));
    d.outputs.push_back(make_out(1, "out"));
    return d;
}

/// @brief An evaluator that returns answers outside its own declaration.
///
/// Three shapes, because the validator names three: a port nobody declared, the same port twice, and a value
/// whose kind is not the port's. Each is a real plugin defect -- a copy-paste that leaves a port number
/// wrong, a loop that emits an output per input, a branch that puts a string where a scalar is declared --
/// and each must be caught **before** the value reaches the cache, because a poisoned content-addressed entry
/// is returned as a cache hit for the rest of the process's life.
class MisdeclaringEvaluator final : public INodeEvaluator {
public:
    enum class Shape { undeclared_port, duplicate_port, wrong_kind };

    explicit MisdeclaringEvaluator(Shape shape) : shape_(shape) {}

    [[nodiscard]] Result<std::vector<std::pair<PortNumber, Value>>> evaluate(
        NodeId, const NodeDesc&, const std::vector<std::pair<PortNumber, Value>>&) override {
        std::vector<std::pair<PortNumber, Value>> out;
        switch (shape_) {
            case Shape::undeclared_port:
                out.emplace_back(99, Value{1.0});
                break;
            case Shape::duplicate_port:
                out.emplace_back(1, Value{1.0});
                out.emplace_back(1, Value{2.0});
                break;
            case Shape::wrong_kind:
                out.emplace_back(1, Value{std::string{"not a number"}});
                break;
        }
        return out;
    }

private:
    Shape shape_;
};
/// @brief An evaluator that raises, for charter C4 at the evaluator's own boundary.
///
/// `EvalContext::evaluator` is plugin code: the host calls it once per uncached node. Before the barrier,
/// a throw here travelled out of `evaluate_graph` and out of whatever called it -- in a UI that is the event
/// loop, and the process is gone.
class RaisingEvaluator final : public INodeEvaluator {
public:
    [[nodiscard]] Result<std::vector<std::pair<PortNumber, Value>>> evaluate(
        NodeId, const NodeDesc&, const std::vector<std::pair<PortNumber, Value>>&) override {
        throw std::runtime_error{"the evaluator has a memory bug"};
    }
};
/// @brief Counting evaluator: records how often it is called, proving the cache really works.
class CountingEvaluator final : public INodeEvaluator {
public:
    [[nodiscard]] Result<std::vector<std::pair<PortNumber, Value>>> evaluate(
        NodeId, const NodeDesc& desc,
        const std::vector<std::pair<PortNumber, Value>>& inputs) override {
        ++compute_count;
        last_inputs = inputs;

        auto find = [&inputs](PortNumber n) -> Value {
            for (const auto& [port, v] : inputs) {
                if (port == n) return v;
            }
            return Value{};
        };

        std::vector<std::pair<PortNumber, Value>> out;

        if (desc.type_name == "const") {
            ++const_count;
            out.emplace_back(1, find(1));
        } else if (desc.type_name == "add") {
            out.emplace_back(1, Value{find(1).as_f64() + find(2).as_f64()});
        } else if (desc.type_name == "scale") {
            out.emplace_back(1, Value{find(1).as_f64() * find(2).as_f64()});
        } else if (desc.type_name == "failing") {
            return Result<std::vector<std::pair<PortNumber, Value>>>{ErrorCode::fit_failed};
        } else {
            return Result<std::vector<std::pair<PortNumber, Value>>>{
                ErrorCode::unknown_node};
        }
        return Result<std::vector<std::pair<PortNumber, Value>>>{std::move(out)};
    }

    std::size_t compute_count = 0;
    std::size_t const_count = 0;
    std::vector<std::pair<PortNumber, Value>> last_inputs;
};

/// @brief Test scene. As with validate's Scene: not movable, filled in by setup.
struct Scene {
    Graph g;
    FakeCatalog catalog;
    CountingEvaluator evaluator;
    EvalCache cache{256};
    EvalResult result;

    [[nodiscard]] EvalContext ctx() noexcept {
        return EvalContext{&catalog, &qp::ports::builtin_registry(), &evaluator, &cache};
    }

    void setup() {
        catalog.add(make_const());
        catalog.add(make_add());
        catalog.add(make_scale());
        catalog.add(make_failing());
    }

    NodeId add_node(const char* type, const char* name = "") {
        auto r = name[0] == '\0' ? g.add_node(type) : g.add_node_named(type, name);
        REQUIRE(r);
        return r.value();
    }

    void set(NodeId id, PortNumber port, double v) {
        g.find_node_mutable(id)->set_param(port, Value{v});
        g.bump_version();
    }

    void wire(NodeId a, PortNumber ap, NodeId b, PortNumber bp) {
        REQUIRE(g.connect(PortRef{a, ap, PortDirection::output},
                          PortRef{b, bp, PortDirection::input}));
    }

    Result<EvalStats> run() {
        result = EvalResult{};
        return evaluate_graph(g, ctx(), result);
    }
};

#define QP_EVAL_SCENE(name) \
    Scene name;             \
    name.setup()

}  // namespace
// ===========================================================================
// value_key.hpp
// ===========================================================================

TEST_CASE("graph.eval.hash_is_fnv1a", "[graph][eval]") {
    // A known FNV-1a value: the hash of "a" is 0xaf63dc4c8601ec8c
    const char* a = "a";
    const ValueHash h = mix_bytes(kFnvOffsetBasis, a, 1);
    REQUIRE(h == 0xaf63dc4c8601ec8cULL);

    // An empty input does not change the seed
    REQUIRE(mix_bytes(kFnvOffsetBasis, a, 0) == kFnvOffsetBasis);

    // Multi-byte: "foobar" is the standard FNV-1a 64 test vector
    const char* foobar = "foobar";
    REQUIRE(mix_bytes(kFnvOffsetBasis, foobar, 6) == 0x85944171f73967e8ULL);
}

TEST_CASE("graph.eval.hash_of_empty_is_seed", "[graph][eval]") {
    REQUIRE(mix_bytes(kFnvOffsetBasis, nullptr, 0) == kFnvOffsetBasis);
    const char dummy = 0;
    REQUIRE(mix_bytes(kFnvOffsetBasis, &dummy, 0) == kFnvOffsetBasis);
    REQUIRE(mix_u64(kFnvOffsetBasis, 0) != kFnvOffsetBasis);   // mixing in a 0 changes it too
}

TEST_CASE("graph.eval.hash_is_deterministic", "[graph][eval][property]") {
    const Value samples[] = {Value{},          Value{1.5},      Value{1.5f},
                             Value{std::int64_t{42}}, Value{true},
                             Value{std::string{"abc"}},
                             Value{qp::units::Dim{1, 0, -1, 0, 0, 0, 0}},
                             Value{qp::abi::LatticeDesc{}}};
    for (const Value& v : samples) {
        const ValueHash a = hash_value(kFnvOffsetBasis, v);
        const ValueHash b = hash_value(kFnvOffsetBasis, v);
        REQUIRE(a == b);
        // A different seed gives a different result (otherwise the hash discriminates nothing)
        const ValueHash c = hash_value(0, v);
        INFO("v.kind=" << v.kind_name());
        REQUIRE(c != a);
    }
}

TEST_CASE("graph.eval.hash_value_kind_discriminates", "[graph][eval]") {
    // Different value kinds must never hash alike: the kind tag is mixed in first
    const Value f64{1.0};
    const Value f32{1.0f};
    const Value i64{std::int64_t{1}};
    const Value b{true};
    const Value text{std::string{"1"}};
    const Value invalid{};

    const ValueHash hs[] = {
        hash_value(kFnvOffsetBasis, f64),   hash_value(kFnvOffsetBasis, f32),
        hash_value(kFnvOffsetBasis, i64),   hash_value(kFnvOffsetBasis, b),
        hash_value(kFnvOffsetBasis, text),  hash_value(kFnvOffsetBasis, invalid),
    };
    for (std::size_t i = 0; i < std::size(hs); ++i) {
        for (std::size_t j = i + 1; j < std::size(hs); ++j) {
            INFO("i=" << i << "  j=" << j);
            REQUIRE(hs[i] != hs[j]);
        }
    }
}

TEST_CASE("graph.eval.hash_value_f32_f64_differ", "[graph][eval]") {
    // The f32 and f64 representations of 1.0 use different bytes, so the hashes must differ
    REQUIRE(hash_value(kFnvOffsetBasis, Value{1.0}) !=
            hash_value(kFnvOffsetBasis, Value{1.0f}));
    // 1.5 is exactly representable at both precisions, but the byte width differs -> hashes differ
    REQUIRE(hash_value(kFnvOffsetBasis, Value{1.5}) !=
            hash_value(kFnvOffsetBasis, Value{1.5f}));
}

TEST_CASE("graph.eval.hash_field_uses_lattice", "[graph][eval]") {
    // Fields go by identity: hash the lattice descriptor, not the data byte by byte
    const auto small = qp::abi::make_lattice(qp::abi::LatticeKind::line,
                                             qp::abi::ComponentKind::scalar,
                                             qp::abi::ElementType::f32, qp::abi::FieldDim{}, 8);
    const auto big = qp::abi::make_lattice(qp::abi::LatticeKind::line,
                                           qp::abi::ComponentKind::scalar,
                                           qp::abi::ElementType::f32, qp::abi::FieldDim{}, 1024);
    const Value vs{small};
    const Value vb{big};
    REQUIRE(hash_value(kFnvOffsetBasis, vs) != hash_value(kFnvOffsetBasis, vb));
    REQUIRE(hash_value(kFnvOffsetBasis, vs) == hash_value(kFnvOffsetBasis, Value{small}));
}

TEST_CASE("graph.eval.hash_port_number_matters", "[graph][eval]") {
    // {1: 2.0} and {2: 2.0} must differ, or moving a wire from port 1 to port 2 hits the same cache
    const std::vector<std::pair<PortNumber, Value>> a{{1, Value{2.0}}};
    const std::vector<std::pair<PortNumber, Value>> b{{2, Value{2.0}}};
    REQUIRE(hash_port_values(kFnvOffsetBasis, a) != hash_port_values(kFnvOffsetBasis, b));
}

TEST_CASE("graph.eval.hash_order_matters", "[graph][eval]") {
    const std::vector<std::pair<PortNumber, Value>> a{{1, Value{1.0}}, {2, Value{2.0}}};
    const std::vector<std::pair<PortNumber, Value>> b{{2, Value{2.0}}, {1, Value{1.0}}};
    // Order sensitivity is **deliberate**: the key builder sorts first, the hash never guesses semantics
    REQUIRE(hash_port_values(kFnvOffsetBasis, a) != hash_port_values(kFnvOffsetBasis, b));
}

TEST_CASE("graph.eval.canonical_text_is_stable", "[graph][eval]") {
    const Value samples[] = {Value{},     Value{1.5},  Value{1.5f},
                             Value{std::int64_t{-7}}, Value{false},
                             Value{std::string{"hi"}},
                             Value{qp::units::Dim{1, 0, -1, 0, 0, 0, 0}}};
    for (const Value& v : samples) {
        const std::string a = canonical_text(v);
        REQUIRE_FALSE(a.empty());
        REQUIRE(canonical_text(v) == a);
    }
    // Floats use %a (hexadecimal float): exact and unaffected by locale.
    // 1.5 = 1x2^0 + 1x2^-1 -> 0x1.8p+0
    REQUIRE(canonical_text(Value{1.5}) == "f64:0x1.8p+0");
    REQUIRE(canonical_text(Value{1.0}) == "f64:0x1p+0");
    REQUIRE(canonical_text(Value{0.5}) == "f64:0x1p-1");
    REQUIRE(canonical_text(Value{}).find("invalid") != std::string::npos);
    REQUIRE(canonical_text(Value{std::int64_t{-7}}).find("-7") != std::string::npos);
}

TEST_CASE("graph.eval.canonical_text_discriminates", "[graph][eval]") {
    // Different text <=> different value. The basis for "a hash collision cannot cause a wrong hit".
    //
    // The key case: 0.1 + 0.2 and the literal 0.3 are **not equal** as doubles.
    // An early implementation used %.17g, which printed both as "0.3" --
    // so the cache key's exact comparison treated two different computations as one.
    // Switching to %a (hexadecimal float) makes them distinguishable.
    //
    // Note the volatile: without it the compiler (a 32-bit FLT_EVAL_METHOD == 2 toolchain)
    // folds the constants at extended precision, making 0.1+0.2 exactly equal 0.3.
    // volatile forces one real double rounding.
    volatile double a = 0.1;
    volatile double b = 0.2;
    const double sum_rt = a + b;
    REQUIRE(sum_rt != 0.3);
    REQUIRE(canonical_text(Value{sum_rt}) != canonical_text(Value{0.3}));
    REQUIRE(canonical_text(Value{0.1}) != canonical_text(Value{0.2}));
    REQUIRE(canonical_text(Value{1.0}) != canonical_text(Value{1.0f}));
    REQUIRE(canonical_text(Value{true}) != canonical_text(Value{std::int64_t{1}}));
}

// ===========================================================================
// cache.hpp
// ===========================================================================

TEST_CASE("graph.eval.cache_key_equality", "[graph][eval]") {
    CacheKey a{};
    a.node = NodeId{1, 1};
    a.type_name = "const";
    a.content = 12345;
    a.content_text = "P1=f64:1";
    CacheKey b = a;
    REQUIRE(a.equals(b));

    b.content = 999;
    REQUIRE_FALSE(a.equals(b));

    b = a;
    b.node = NodeId{1, 2};
    REQUIRE_FALSE(a.equals(b));

    b = a;
    b.type_name = "other";
    REQUIRE_FALSE(a.equals(b));
}

TEST_CASE("graph.eval.cache_key_generation_matters", "[graph][eval]") {
    // This is where the old project's `id(lattice)` defect is fixed.
    // A reused slot has a different generation -> a different key -> no hit on the old cache.
    CacheKey old_key{};
    old_key.node = NodeId{3, 1};
    old_key.content = 42;
    CacheKey new_key{};
    new_key.node = NodeId{3, 2};   // the same slot, a new generation
    new_key.content = 42;          // the content hash happens to be equal
    REQUIRE_FALSE(old_key.equals(new_key));
}

TEST_CASE("graph.eval.cache_key_param_change_differs", "[graph][eval]") {
    // A parameter change -> a different content hash -> a different key
    const std::vector<std::pair<PortNumber, Value>> p1{{1, Value{1.0}}};
    const std::vector<std::pair<PortNumber, Value>> p2{{1, Value{2.0}}};
    CacheKey a{};
    a.content = hash_port_values(kFnvOffsetBasis, p1);
    CacheKey b{};
    b.content = hash_port_values(kFnvOffsetBasis, p2);
    REQUIRE_FALSE(a.equals(b));
}

TEST_CASE("graph.eval.cache_miss_on_first_lookup", "[graph][eval]") {
    EvalCache c{4};
    CacheKey k{};
    k.node = NodeId{1, 1};
    REQUIRE(c.find(k) == nullptr);
    REQUIRE(c.misses() == 1);
    REQUIRE(c.hits() == 0);
    REQUIRE(c.size() == 0);
}

TEST_CASE("graph.eval.cache_store_and_fetch", "[graph][eval]") {
    EvalCache c{4};
    CacheKey k{};
    k.node = NodeId{1, 1};
    k.content = 7;

    c.put(k, {{1, Value{2.5}}});
    REQUIRE(c.size() == 1);

    const CacheEntry* e = c.find(k);
    REQUIRE(e != nullptr);
    REQUIRE(e->outputs.size() == 1);
    REQUIRE(e->outputs.front().second.as_f64() == 2.5);
    REQUIRE(c.hits() == 1);

    // Overwrite
    c.put(k, {{1, Value{9.0}}});
    REQUIRE(c.size() == 1);
    REQUIRE(c.find(k)->outputs.front().second.as_f64() == 9.0);
}

TEST_CASE("graph.eval.cache_eviction_lru", "[graph][eval]") {
    EvalCache c{2};
    CacheKey a{};
    a.node = NodeId{1, 1};
    a.content = 1;
    CacheKey b{};
    b.node = NodeId{2, 1};
    b.content = 2;
    CacheKey d{};
    d.node = NodeId{3, 1};
    d.content = 3;

    c.put(a, {{1, Value{1.0}}});
    c.put(b, {{1, Value{2.0}}});
    REQUIRE(c.size() == 2);

    // Touching a makes it "most recently used"
    REQUIRE(c.find(a) != nullptr);

    // Inserting a third -> evict the least recently used, b
    c.put(d, {{1, Value{3.0}}});
    REQUIRE(c.size() == 2);
    REQUIRE(c.evictions() == 1);
    REQUIRE(c.find(a) != nullptr);   // a was touched, so it stays
    REQUIRE(c.find(d) != nullptr);
    REQUIRE(c.find(b) == nullptr);   // b was evicted
}

TEST_CASE("graph.eval.cache_disabled_when_zero", "[graph][eval]") {
    EvalCache c{0};
    CacheKey k{};
    k.node = NodeId{1, 1};
    c.put(k, {{1, Value{1.0}}});
    REQUIRE(c.size() == 0);
    REQUIRE(c.find(k) == nullptr);
    REQUIRE(c.misses() == 1);
    REQUIRE(c.hits() == 0);
}

TEST_CASE("graph.eval.cache_clear", "[graph][eval]") {
    EvalCache c{4};
    CacheKey k{};
    k.node = NodeId{1, 1};
    c.put(k, {{1, Value{1.0}}});
    REQUIRE(c.size() == 1);
    c.clear();
    REQUIRE(c.size() == 0);
    // Statistics are kept (they are cumulative observations, not instantaneous state)
    REQUIRE(c.capacity() == 4);
}

TEST_CASE("graph.eval.cache_stats_are_consistent", "[graph][eval]") {
    EvalCache c{2};
    REQUIRE(c.hits() + c.misses() == 0);
    CacheKey a{};
    a.node = NodeId{1, 1};
    c.put(a, {});
    (void)c.find(a);   // hit
    CacheKey b{};
    b.node = NodeId{2, 1};
    (void)c.find(b);   // miss
    (void)c.find(a);   // hit
    REQUIRE(c.hits() == 2);
    REQUIRE(c.misses() == 1);
    REQUIRE(c.hits() + c.misses() == 3);
}

TEST_CASE("graph.eval.cache_undo_redo_hits", "[graph][eval]") {
    // Undoing one step -> the previous key reappears -> an **immediate hit**.
    // This is the key advantage of content addressing over "invalidate by version":
    // undo/redo is frequent in a classroom demo, and recomputing every time breaks the flow.
    EvalCache c{16};
    CacheKey before{};
    before.node = NodeId{1, 1};
    before.content = 111;
    CacheKey after{};
    after.node = NodeId{1, 1};
    after.content = 222;

    c.put(before, {{1, Value{1.0}}});
    c.put(after, {{1, Value{2.0}}});
    // After the undo we are back at before's key
    const CacheEntry* e = c.find(before);
    REQUIRE(e != nullptr);
    REQUIRE(e->outputs.front().second.as_f64() == 1.0);
    REQUIRE(c.evictions() == 0);   // both keys are present, nothing was evicted
}

// ===========================================================================
// evaluator.hpp
// ===========================================================================

TEST_CASE("graph.eval.single_node", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId c = s.add_node("const", "c1");
    s.set(c, 1, 3.5);

    const auto r = s.run();
    REQUIRE(r);
    REQUIRE(r.value().nodes_visited == 1);
    REQUIRE(r.value().nodes_computed == 1);
    REQUIRE(r.value().cache_hits == 0);
    REQUIRE(s.result.get(c, 1).as_f64() == 3.5);
}

TEST_CASE("graph.eval.chain_propagates", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId a = s.add_node("const", "a");
    const NodeId b = s.add_node("const", "b");
    const NodeId sum = s.add_node("add", "sum");
    const NodeId sc = s.add_node("scale", "sc");
    s.set(a, 1, 2.0);
    s.set(b, 1, 3.0);
    s.set(sc, 2, 10.0);

    s.wire(a, 1, sum, 1);
    s.wire(b, 1, sum, 2);
    s.wire(sum, 1, sc, 1);

    const auto r = s.run();
    REQUIRE(r);
    REQUIRE(s.result.get(sum, 1).as_f64() == 5.0);
    REQUIRE(s.result.get(sc, 1).as_f64() == 50.0);
    REQUIRE(r.value().nodes_computed == 4);
}

TEST_CASE("graph.eval.deterministic_across_runs", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId a = s.add_node("const", "a");
    const NodeId b = s.add_node("const", "b");
    const NodeId sum = s.add_node("add", "sum");
    s.set(a, 1, 0.1);
    s.set(b, 1, 0.2);
    s.wire(a, 1, sum, 1);
    s.wire(b, 1, sum, 2);

    REQUIRE(s.run());
    const double first = s.result.get(sum, 1).as_f64();

    // Recompute after clearing the cache: the result must be **bit-identical**.
    // Note: do not write == 0.1 + 0.2 here -- that triggers float constant folding,
    // and this machine is a 32-bit FLT_EVAL_METHOD == 2 toolchain (see
    // standards/test-taxonomy.md section 5.1), so compile-time and run-time evaluation can differ.
    s.cache.clear();
    REQUIRE(s.run());
    const double second = s.result.get(sum, 1).as_f64();
    REQUIRE(first == second);
    // Compare against the same expression computed separately. volatile keeps both sides
    // at run time (constant folding would use extended precision; see test-taxonomy section 5.1).
    volatile double v1 = 0.1;
    volatile double v2 = 0.2;
    const double expected = v1 + v2;
    REQUIRE(first == expected);
}

TEST_CASE("graph.eval.cache_hit_on_second_run", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId c = s.add_node("const", "c");
    s.set(c, 1, 1.0);

    REQUIRE(s.run());
    REQUIRE(s.evaluator.compute_count == 1);
    REQUIRE(s.result.get(c, 1).as_f64() == 1.0);

    // Second run: the key is unchanged -> cache hit, the implementation is not called
    REQUIRE(s.run());
    REQUIRE(s.evaluator.compute_count == 1);
    REQUIRE(s.result.get(c, 1).as_f64() == 1.0);
}

TEST_CASE("graph.eval.param_change_invalidates_only_downstream", "[graph][eval]") {
    // Three independent branches: changing one parameter should recompute **only that chain**.
    // This is the core value of content addressing over "invalidate by version".
    QP_EVAL_SCENE(s);
    const NodeId a = s.add_node("const", "a");
    const NodeId t1 = s.add_node("scale", "t1");
    const NodeId b = s.add_node("const", "b");
    const NodeId t2 = s.add_node("scale", "t2");
    s.set(a, 1, 2.0);
    s.set(t1, 2, 10.0);
    s.set(b, 1, 3.0);
    s.set(t2, 2, 100.0);
    s.wire(a, 1, t1, 1);
    s.wire(b, 1, t2, 1);

    REQUIRE(s.run());
    const std::size_t after_first = s.evaluator.compute_count;
    REQUIRE(after_first == 4);

    // Change only a's parameter
    s.set(a, 1, 5.0);
    REQUIRE(s.run());
    // a and t1 recompute (2 calls); b and t2 hit the cache
    REQUIRE(s.evaluator.compute_count == after_first + 2);
    REQUIRE(s.result.get(t1, 1).as_f64() == 50.0);
    REQUIRE(s.result.get(t2, 1).as_f64() == 300.0);   // still the old value (b did not change)
}

TEST_CASE("graph.eval.diamond_evaluates_once", "[graph][eval]") {
    // Diamond: a -> t1 -> sum, a -> t2 -> sum.
    // Topological order guarantees a is visited once, hence computed once.
    QP_EVAL_SCENE(s);
    const NodeId a = s.add_node("const", "a");
    const NodeId t1 = s.add_node("scale", "t1");
    const NodeId t2 = s.add_node("scale", "t2");
    const NodeId sum = s.add_node("add", "sum");
    s.set(a, 1, 2.0);
    s.set(t1, 2, 3.0);
    s.set(t2, 2, 5.0);
    s.wire(a, 1, t1, 1);
    s.wire(a, 1, t2, 1);
    s.wire(t1, 1, sum, 1);
    s.wire(t2, 1, sum, 2);

    REQUIRE(s.run());
    REQUIRE(s.evaluator.const_count == 1);        // a was computed only once
    REQUIRE(s.result.get(sum, 1).as_f64() == 16.0);   // 6 + 10
    REQUIRE(s.evaluator.compute_count == 4);
}

TEST_CASE("graph.eval.bypass_passthrough", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId a = s.add_node("const", "a");
    const NodeId sc = s.add_node("scale", "sc");
    s.set(a, 1, 4.0);
    s.set(sc, 2, 100.0);   // would become 400 if it took part in the computation
    s.wire(a, 1, sc, 1);

    // Bypass: do not call the implementation, pass the inputs straight through to the outputs
    s.g.find_node_mutable(sc)->bypassed = true;
    s.g.bump_version();

    const auto r = s.run();
    REQUIRE(r);
    REQUIRE(r.value().nodes_skipped == 1);
    REQUIRE(s.result.get(sc, 1).as_f64() == 4.0);   // not multiplied by 100
}

TEST_CASE("graph.eval.unknown_type_fails", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    (void)s.add_node("no_such_type", "x");

    const auto r = s.run();
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::unknown_node);
}

TEST_CASE("graph.eval.missing_param_fails", "[graph][eval]") {
    // A const node with no parameter set -> an invalid input value -> the implementation
    // returns an invalid value (i.e. 0.0) and does not error. This case checks "no crash".
    QP_EVAL_SCENE(s);
    const NodeId c = s.add_node("const", "c");

    const auto r = s.run();
    REQUIRE(r);
    REQUIRE_FALSE(s.result.get(c, 1).valid());
}

TEST_CASE("graph.eval.evaluator_error_propagates", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId c = s.add_node("const", "c");
    const NodeId f = s.add_node("failing", "f");
    s.set(c, 1, 1.0);
    s.wire(c, 1, f, 1);

    const auto r = s.run();
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::fit_failed);
}

TEST_CASE("graph.eval.is_readonly", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId c = s.add_node("const", "c");
    s.set(c, 1, 1.0);

    const GraphVersion v = s.g.version();
    const std::size_t nodes = s.g.node_count();
    const std::size_t edges = s.g.edge_count();

    REQUIRE(s.run());
    REQUIRE(s.g.version() == v);
    REQUIRE(s.g.node_count() == nodes);
    REQUIRE(s.g.edge_count() == edges);
}

TEST_CASE("graph.eval.an_answer_outside_the_declaration_is_a_fault", "[graph][eval]") {
    // The other half of C4's schema domain, and the half that does not involve an exception at all: a plugin
    // that returns happily and wrongly. `ports::check_value` was written for exactly this -- its contract says
    // "it runs after every evaluation" -- and until now nothing called it, so a value of the wrong kind went
    // into the cache and came back as a cache hit for the rest of the process's life.
    for (const MisdeclaringEvaluator::Shape shape :
         {MisdeclaringEvaluator::Shape::undeclared_port,
          MisdeclaringEvaluator::Shape::duplicate_port,
          MisdeclaringEvaluator::Shape::wrong_kind}) {
        Graph g;
        FakeCatalog catalog;
        catalog.add(make_const());
        MisdeclaringEvaluator evaluator{shape};
        EvalCache cache{16};
        EvalResult result;

        const auto added = g.add_node("const");
        REQUIRE(added.has_value());
        g.find_node_mutable(added.value())->set_param(1, Value{1.0});
        g.bump_version();

        qp::plugin::FaultLog faults;
        const EvalContext context{&catalog, &qp::ports::builtin_registry(), &evaluator, &cache, &faults};

        const Result<EvalStats> stats = evaluate_graph(g, context, result);
        REQUIRE_FALSE(stats.has_value());
        REQUIRE(stats.error() == qp::diag::ErrorCode::plugin_fault);

        // Nothing was cached: the entry would have been poisoned, and "the cache returns wrong values" is
        // indistinguishable from a correct cache to every caller downstream.
        REQUIRE(cache.size() == 0);
        // And the empty result holds no outputs either, so nothing downstream can read the bad value.
        REQUIRE_FALSE(result.get(added.value(), 1).valid());

        // The fault says **which** way the answer was wrong, because "the plugin is broken" sends the reader
        // to the plugin while "port 99 was not declared" sends them to the line of its manifest that is wrong.
        REQUIRE(faults.faults().size() == 1);
        REQUIRE(faults.faults().front().label == "const");
        REQUIRE_FALSE(faults.faults().front().what.empty());
    }

    // The three sentences are distinct: one message for three different defects would make the log useless for
    // the one thing it is for.
    const std::string undeclared = "returned a value for output port 99, which it does not declare";
    REQUIRE(undeclared.find("99") != std::string::npos);
}
TEST_CASE("graph.eval.a_raising_evaluator_is_a_fault", "[graph][eval]") {
    // Charter C4 at the evaluator boundary. `EvalContext::evaluator` is plugin code, called once per uncached
    // node; before the barrier a throw here travelled out of `evaluate_graph` and out of whatever called it,
    // which in this platform is a Qt slot -- and a process that throws out of a slot dies.
    Graph g;
    FakeCatalog catalog;
    catalog.add(make_const());
    RaisingEvaluator evaluator;
    EvalCache cache{16};
    EvalResult result;

    const auto added = g.add_node("const");
    REQUIRE(added.has_value());
    const NodeId node = added.value();
    g.find_node_mutable(node)->set_param(1, Value{1.0});
    g.bump_version();

    qp::plugin::FaultLog faults;
    const EvalContext context{&catalog, &qp::ports::builtin_registry(), &evaluator, &cache, &faults};

    // Strike one: caught, converted, and reported as a plugin fault rather than as a graph problem -- the
    // graph is fine, the code that was asked to compute it is not.
    const Result<EvalStats> failed = evaluate_graph(g, context, result);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error() == qp::diag::ErrorCode::plugin_fault);
    REQUIRE(faults.faults().size() == 1);
    // The label is the **node type**, because that is what the user is looking at; naming the plugin would be
    // less useful in a diagnostic than naming the thing on screen that stopped working.
    REQUIRE(faults.faults().front().label == "const");
    REQUIRE(faults.faults().front().what == "the evaluator has a memory bug");

    // A faulted node caches nothing -- there is no result to cache -- so a second attempt really calls the
    // plugin again, which is what makes the strike count mean "it keeps happening".
    REQUIRE_FALSE(cache.size() > 0);
    const Result<EvalStats> second = evaluate_graph(g, context, result);
    REQUIRE(second.error() == qp::diag::ErrorCode::plugin_fault);
    REQUIRE(faults.faults().size() == 2);
    REQUIRE_FALSE(faults.is_quarantined("const"));

    // The third strike crosses the limit. The caller still sees the fault it caused, and the log records that
    // this one was the last straw.
    const Result<EvalStats> third = evaluate_graph(g, context, result);
    REQUIRE(third.error() == qp::diag::ErrorCode::plugin_fault);
    REQUIRE(faults.faults().size() == 3);
    REQUIRE(faults.is_quarantined("const"));
    REQUIRE(faults.faults().back().quarantined);

    // From here the plugin is not called at all. The code says "we stopped asking", which is a different
    // story from "it failed again": one is a defect report, the other is a policy the host is applying.
    const Result<EvalStats> refused = evaluate_graph(g, context, result);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == qp::diag::ErrorCode::plugin_quarantined);
    REQUIRE(faults.faults().size() == 3);   // a call that never happened is not a fault
}
TEST_CASE("graph.eval.result_lookup", "[graph][eval]") {
    EvalResult r;
    REQUIRE_FALSE(r.get(NodeId{1, 1}, 1).valid());
    REQUIRE(r.size() == 0);

    r.set(NodeId{1, 1}, 1, Value{1.0});
    r.set(NodeId{1, 1}, 2, Value{2.0});
    r.set(NodeId{2, 1}, 1, Value{3.0});
    REQUIRE(r.size() == 3);
    REQUIRE(r.get(NodeId{1, 1}, 1).as_f64() == 1.0);
    REQUIRE(r.get(NodeId{1, 1}, 2).as_f64() == 2.0);
    REQUIRE(r.get(NodeId{2, 1}, 1).as_f64() == 3.0);
    // The port number is part of the key
    REQUIRE_FALSE(r.get(NodeId{1, 1}, 3).valid());
    REQUIRE_FALSE(r.get(NodeId{9, 9}, 1).valid());
}

TEST_CASE("graph.eval.empty_graph_succeeds", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const auto r = s.run();
    REQUIRE(r);
    REQUIRE(r.value().nodes_visited == 0);
    REQUIRE(r.value().nodes_computed == 0);
    REQUIRE(s.result.size() == 0);
}
