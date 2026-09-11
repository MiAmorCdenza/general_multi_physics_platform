/**
 * @file test_eval.cpp
 * @brief core/graph/eval 的单元与性质测试。
 *
 * 三组重点：
 *   1. **哈希的确定性**：不依赖地址、时间、实现定义的 std::hash
 *   2. **缓存键的判别力**：世代号、端口号、参数变化都必须导致不同的键
 *   3. **求值的确定性**：同一图两次求值给出逐位相同结果
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/eval.hpp>

#include <cstring>
#include <deque>
#include <string>

using namespace qp::graph;
using qp::diag::ErrorCode;

namespace {

using qp::ports::Value;

/// @brief 简单可用的节点目录。
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

/// @brief 常量节点：输出参数 1 的值。无输入。
[[nodiscard]] NodeDesc make_const() {
    NodeDesc d{};
    d.type_name = "const";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "value"));
    d.outputs.push_back(make_out(1, "out"));
    return d;
}

/// @brief 加法节点：输出 = 输入1 + 输入2。
[[nodiscard]] NodeDesc make_add() {
    NodeDesc d{};
    d.type_name = "add";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "a"));
    d.inputs.push_back(make_in(2, "b"));
    d.outputs.push_back(make_out(1, "sum"));
    return d;
}

/// @brief 倍率节点：输出 = 输入1 × 参数1。
[[nodiscard]] NodeDesc make_scale() {
    NodeDesc d{};
    d.type_name = "scale";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "in"));
    d.inputs.push_back(make_in(2, "factor"));
    d.outputs.push_back(make_out(1, "out"));
    return d;
}

/// @brief 会失败的节点：用于验证错误传播。
[[nodiscard]] NodeDesc make_failing() {
    NodeDesc d{};
    d.type_name = "failing";
    d.has_compute = true;
    d.inputs.push_back(make_in(1, "in"));
    d.outputs.push_back(make_out(1, "out"));
    return d;
}

/// @brief 计数求值器：记录被调用次数，用来证明缓存真的生效。
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

/// @brief 测试场景。与 validate 的 Scene 同理：不可移动，用 setup 填充。
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
// ═══════════════════════════════════════════════════════════════════════════
// value_key.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.eval.hash_is_fnv1a", "[graph][eval]") {
    // FNV-1a 的已知值："a" 的哈希是 0xaf63dc4c8601ec8c
    const char* a = "a";
    const ValueHash h = mix_bytes(kFnvOffsetBasis, a, 1);
    REQUIRE(h == 0xaf63dc4c8601ec8cULL);

    // 空输入不改变种子
    REQUIRE(mix_bytes(kFnvOffsetBasis, a, 0) == kFnvOffsetBasis);

    // 多字节："foobar" 是 FNV-1a 64 的标准测试向量
    const char* foobar = "foobar";
    REQUIRE(mix_bytes(kFnvOffsetBasis, foobar, 6) == 0x85944171f73967e8ULL);
}

TEST_CASE("graph.eval.hash_of_empty_is_seed", "[graph][eval]") {
    REQUIRE(mix_bytes(kFnvOffsetBasis, nullptr, 0) == kFnvOffsetBasis);
    const char dummy = 0;
    REQUIRE(mix_bytes(kFnvOffsetBasis, &dummy, 0) == kFnvOffsetBasis);
    REQUIRE(mix_u64(kFnvOffsetBasis, 0) != kFnvOffsetBasis);   // 混入一个 0 也会变
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
        // 不同种子给出不同结果（否则哈希没有区分力）
        const ValueHash c = hash_value(0, v);
        INFO("v.kind=" << v.kind_name());
        REQUIRE(c != a);
    }
}

TEST_CASE("graph.eval.hash_value_kind_discriminates", "[graph][eval]") {
    // 不同种类的值绝不能给出同一哈希：种类标签先行混入
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
    // 1.0 的 f32 与 f64 表示字节不同，因此哈希必须不同
    REQUIRE(hash_value(kFnvOffsetBasis, Value{1.0}) !=
            hash_value(kFnvOffsetBasis, Value{1.0f}));
    // 1.5 在两种精度下都精确可表示，但字节宽度不同 → 哈希仍不同
    REQUIRE(hash_value(kFnvOffsetBasis, Value{1.5}) !=
            hash_value(kFnvOffsetBasis, Value{1.5f}));
}

TEST_CASE("graph.eval.hash_field_uses_lattice", "[graph][eval]") {
    // 场走标识：按格子描述符哈希，不逐字节哈希数据
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
    // {1: 2.0} 与 {2: 2.0} 必须不同：否则"把线从端口 1 挪到端口 2"会命中同一缓存
    const std::vector<std::pair<PortNumber, Value>> a{{1, Value{2.0}}};
    const std::vector<std::pair<PortNumber, Value>> b{{2, Value{2.0}}};
    REQUIRE(hash_port_values(kFnvOffsetBasis, a) != hash_port_values(kFnvOffsetBasis, b));
}

TEST_CASE("graph.eval.hash_order_matters", "[graph][eval]") {
    const std::vector<std::pair<PortNumber, Value>> a{{1, Value{1.0}}, {2, Value{2.0}}};
    const std::vector<std::pair<PortNumber, Value>> b{{2, Value{2.0}}, {1, Value{1.0}}};
    // 顺序敏感是**刻意的**：键构造方负责先排序，而不是让哈希"猜"语义
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
    // 浮点用 %a（十六进制浮点）：精确且不受 locale 影响。
    // 1.5 = 1×2^0 + 1×2^-1 → 0x1.8p+0
    REQUIRE(canonical_text(Value{1.5}) == "f64:0x1.8p+0");
    REQUIRE(canonical_text(Value{1.0}) == "f64:0x1p+0");
    REQUIRE(canonical_text(Value{0.5}) == "f64:0x1p-1");
    REQUIRE(canonical_text(Value{}).find("invalid") != std::string::npos);
    REQUIRE(canonical_text(Value{std::int64_t{-7}}).find("-7") != std::string::npos);
}

TEST_CASE("graph.eval.canonical_text_discriminates", "[graph][eval]") {
    // 文本不同 ⟺ 值不同。这是"哈希碰撞不会导致错误命中"的基础。
    //
    // 关键案例：0.1 + 0.2 与字面量 0.3 在 double 下**不相等**。
    // 早期实现用 %.17g，它把两者都输出成 "0.3"——
    // 于是缓存键的精确比较会把两个不同的计算当成同一次。
    // 改用 %a（十六进制浮点）后两者可区分。
    //
    // 注意 volatile：不加它，编译器（FLT_EVAL_METHOD == 2 的 32 位工具链）
    // 会在扩展精度下折叠常量，使 0.1+0.2 恰好等于 0.3。
    // 加 volatile 强制它走一次真正的 double 舍入。
    volatile double a = 0.1;
    volatile double b = 0.2;
    const double sum_rt = a + b;
    REQUIRE(sum_rt != 0.3);
    REQUIRE(canonical_text(Value{sum_rt}) != canonical_text(Value{0.3}));
    REQUIRE(canonical_text(Value{0.1}) != canonical_text(Value{0.2}));
    REQUIRE(canonical_text(Value{1.0}) != canonical_text(Value{1.0f}));
    REQUIRE(canonical_text(Value{true}) != canonical_text(Value{std::int64_t{1}}));
}

// ═══════════════════════════════════════════════════════════════════════════
// cache.hpp
// ═══════════════════════════════════════════════════════════════════════════

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
    // 这是旧工程 `id(lattice)` 缺陷的修复落点。
    // 槽位复用后世代不同 → 键不同 → 不会命中旧节点的缓存。
    CacheKey old_key{};
    old_key.node = NodeId{3, 1};
    old_key.content = 42;
    CacheKey new_key{};
    new_key.node = NodeId{3, 2};   // 同一槽位，新世代
    new_key.content = 42;          // 内容哈希碰巧相同
    REQUIRE_FALSE(old_key.equals(new_key));
}

TEST_CASE("graph.eval.cache_key_param_change_differs", "[graph][eval]") {
    // 参数变化 → 内容哈希变化 → 键不同
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

    // 覆盖写
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

    // 访问 a 让它变"最近使用"
    REQUIRE(c.find(a) != nullptr);

    // 插入第三个 → 淘汰最久未使用的 b
    c.put(d, {{1, Value{3.0}}});
    REQUIRE(c.size() == 2);
    REQUIRE(c.evictions() == 1);
    REQUIRE(c.find(a) != nullptr);   // a 被访问过，保留
    REQUIRE(c.find(d) != nullptr);
    REQUIRE(c.find(b) == nullptr);   // b 被淘汰
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
    // 统计量保留（它们是累计观察量，不是瞬时状态）
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
    // 撤销回上一步 → 之前的键重新出现 → **立刻命中**。
    // 这是内容寻址相对"版本号失效"的关键优势：
    // 课堂演示里撤销/重做是高频操作，每次重算会打断节奏。
    EvalCache c{16};
    CacheKey before{};
    before.node = NodeId{1, 1};
    before.content = 111;
    CacheKey after{};
    after.node = NodeId{1, 1};
    after.content = 222;

    c.put(before, {{1, Value{1.0}}});
    c.put(after, {{1, Value{2.0}}});
    // 撤销后回到 before 的键
    const CacheEntry* e = c.find(before);
    REQUIRE(e != nullptr);
    REQUIRE(e->outputs.front().second.as_f64() == 1.0);
    REQUIRE(c.evictions() == 0);   // 两个键都在，没有被淘汰
}

// ═══════════════════════════════════════════════════════════════════════════
// evaluator.hpp
// ═══════════════════════════════════════════════════════════════════════════

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

    // 清空缓存后重算，结果必须**逐位相同**。
    // 注意不要在这里写 == 0.1 + 0.2：那会触发浮点常量折叠，
    // 而本机是 FLT_EVAL_METHOD == 2 的 32 位工具链（见
    // standards/test-taxonomy.md §5.1），编译期与运行期求值可能不同。
    s.cache.clear();
    REQUIRE(s.run());
    const double second = s.result.get(sum, 1).as_f64();
    REQUIRE(first == second);
    // 与另算一遍的同一个表达式比较。用 volatile 保证两侧都在运行期求值
    // （常量折叠会在扩展精度下算，见 standards/test-taxonomy.md §5.1）。
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

    // 第二次：键不变 → 命中缓存，实现不再被调用
    REQUIRE(s.run());
    REQUIRE(s.evaluator.compute_count == 1);
    REQUIRE(s.result.get(c, 1).as_f64() == 1.0);
}

TEST_CASE("graph.eval.param_change_invalidates_only_downstream", "[graph][eval]") {
    // 三个独立分支：改一个参数只应让**那一条链**重算。
    // 这是内容寻址相对"版本号失效"的核心价值。
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

    // 只改 a 的参数
    s.set(a, 1, 5.0);
    REQUIRE(s.run());
    // a 与 t1 重算（2 次）；b 与 t2 命中缓存
    REQUIRE(s.evaluator.compute_count == after_first + 2);
    REQUIRE(s.result.get(t1, 1).as_f64() == 50.0);
    REQUIRE(s.result.get(t2, 1).as_f64() == 300.0);   // 仍是旧值（b 未变）
}

TEST_CASE("graph.eval.diamond_evaluates_once", "[graph][eval]") {
    // 菱形：a → t1 → sum，a → t2 → sum。
    // 拓扑序保证 a 只被访问一次，因此只算一次。
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
    REQUIRE(s.evaluator.const_count == 1);        // a 只算了一次
    REQUIRE(s.result.get(sum, 1).as_f64() == 16.0);   // 6 + 10
    REQUIRE(s.evaluator.compute_count == 4);
}

TEST_CASE("graph.eval.bypass_passthrough", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    const NodeId a = s.add_node("const", "a");
    const NodeId sc = s.add_node("scale", "sc");
    s.set(a, 1, 4.0);
    s.set(sc, 2, 100.0);   // 若参与计算会变成 400
    s.wire(a, 1, sc, 1);

    // 绕过：不调用实现，把输入透传到输出
    s.g.find_node_mutable(sc)->bypassed = true;
    s.g.bump_version();

    const auto r = s.run();
    REQUIRE(r);
    REQUIRE(r.value().nodes_skipped == 1);
    REQUIRE(s.result.get(sc, 1).as_f64() == 4.0);   // 未乘 100
}

TEST_CASE("graph.eval.unknown_type_fails", "[graph][eval]") {
    QP_EVAL_SCENE(s);
    (void)s.add_node("no_such_type", "x");

    const auto r = s.run();
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::unknown_node);
}

TEST_CASE("graph.eval.missing_param_fails", "[graph][eval]") {
    // 常量节点没设参数 → 输入是无效值 → 实现返回无效值（ak 0.0），
    // 本身不报错。这条用例验证的是"未设参数不会崩溃"。
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
    // 端口号是键的一部分
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
