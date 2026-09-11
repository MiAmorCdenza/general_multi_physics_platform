/**
 * @file test_structure.cpp
 * @brief core/graph/structure 的单元与性质测试。
 *
 * 重点在**失败分支**：每个变异函数都必须验证"失败后图与版本号都不变"。
 * 这是撤销栈能工作的前提，也是契约里专门写下来的 @post。
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/structure.hpp>

#include <set>
#include <string>

using namespace qp::graph;
using qp::diag::ErrorCode;

namespace {

/// @brief 建一个 n1 → n2 → n3 的链，返回三个句柄。
struct Chain {
    Graph g;
    NodeId a{};
    NodeId b{};
    NodeId c{};
};

Chain make_chain() {
    Chain ch;
    ch.a = ch.g.add_node("source").value();
    ch.b = ch.g.add_node("gain").value();
    ch.c = ch.g.add_node("sink").value();
    REQUIRE(ch.g.connect(PortRef{ch.a, 1, PortDirection::output},
                         PortRef{ch.b, 1, PortDirection::input}));
    REQUIRE(ch.g.connect(PortRef{ch.b, 1, PortDirection::output},
                         PortRef{ch.c, 1, PortDirection::input}));
    return ch;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 节点
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.structure.empty_graph", "[graph][structure]") {
    const Graph g;
    REQUIRE(g.node_count() == 0);
    REQUIRE(g.edge_count() == 0);
    REQUIRE(g.slot_count() == 0);
    REQUIRE(g.version() == 1);
    REQUIRE_FALSE(g.has_node(NodeId{0, 1}));
    REQUIRE(g.find_node(NodeId{5, 5}) == nullptr);
}

TEST_CASE("graph.structure.add_node", "[graph][structure]") {
    Graph g;
    const GraphVersion v0 = g.version();

    const auto r = g.add_node("dipole");
    REQUIRE(r);
    const NodeId id = r.value();
    REQUIRE(id.valid());
    REQUIRE(g.node_count() == 1);
    REQUIRE(g.version() > v0);          // 成功变异递增版本

    const Node* n = g.find_node(id);
    REQUIRE(n != nullptr);
    REQUIRE(n->type_name == "dipole");
    REQUIRE(n->id == id);
    REQUIRE(n->name.empty());
}

TEST_CASE("graph.structure.add_node_uses_fresh_generation", "[graph][structure]") {
    // 槽位复用后世代必须不同：否则老句柄会"复活"成新节点，
    // 撤销栈与缓存键会静默指向错对象。
    Graph g;
    const NodeId first = g.add_node("a").value();
    REQUIRE(g.remove_node(first));

    const NodeId second = g.add_node("b").value();
    REQUIRE(second.index == first.index);              // 复用了同一个槽位
    REQUIRE(second.generation != first.generation);    // 但世代不同
    REQUIRE(g.find_node(first) == nullptr);            // 老句柄失效
    REQUIRE(g.find_node(second) != nullptr);
    REQUIRE_FALSE(g.has_node(first));
}

TEST_CASE("graph.structure.remove_node_invalidates_handle", "[graph][structure]") {
    Graph g;
    const NodeId id = g.add_node("x").value();
    const GraphVersion before_remove = g.version();

    REQUIRE(g.remove_node(id));
    REQUIRE(g.node_count() == 0);
    REQUIRE(g.version() > before_remove);
    REQUIRE(g.find_node(id) == nullptr);

    // 重复删除：失败，且不改变图
    const GraphVersion v = g.version();
    const auto again = g.remove_node(id);
    REQUIRE_FALSE(again);
    REQUIRE(again.error() == ErrorCode::unknown_node);
    REQUIRE(g.version() == v);
}

TEST_CASE("graph.structure.remove_node_drops_edges", "[graph][structure]") {
    Chain ch = make_chain();
    REQUIRE(ch.g.edge_count() == 2);

    REQUIRE(ch.g.remove_node(ch.b));       // 删中间节点
    REQUIRE(ch.g.node_count() == 2);
    // 两条边都连着 b，必须一起消失——否则图里会留下悬垂边
    REQUIRE(ch.g.edge_count() == 0);
}

TEST_CASE("graph.structure.node_count_tracks_slots", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const NodeId c = g.add_node("c").value();
    REQUIRE(g.node_count() == 3);
    REQUIRE(g.slot_count() == 3);

    REQUIRE(g.remove_node(b));
    REQUIRE(g.node_count() == 2);
    REQUIRE(g.slot_count() == 3);       // 空槽保留（不移动其他节点）

    // 新节点复用空槽
    const NodeId d = g.add_node("d").value();
    REQUIRE(g.node_count() == 3);
    REQUIRE(g.slot_count() == 3);
    REQUIRE(d.index == b.index);
    // 其他句柄不受影响
    REQUIRE(g.has_node(a));
    REQUIRE(g.has_node(c));
    REQUIRE(g.has_node(d));
}

TEST_CASE("graph.structure.find_node_by_user_name", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node_named("spring", "spring_1").value();
    const NodeId b = g.add_node_named("spring", "spring_2").value();
    REQUIRE(a != b);
    REQUIRE(g.find_node(a)->name == "spring_1");
    REQUIRE(g.find_node(b)->name == "spring_2");

    REQUIRE(g.find_node_by_name("spring_1") == a);
    REQUIRE(g.find_node_by_name("spring_2") == b);
    REQUIRE_FALSE(g.find_node_by_name("nope").valid());
    REQUIRE_FALSE(g.find_node_by_name("").valid());

    // 名字必须唯一：否则报告里"spring_1"指哪个节点就是歧义的
    const auto dup = g.add_node_named("spring", "spring_1");
    REQUIRE_FALSE(dup);
    REQUIRE(dup.error() == ErrorCode::duplicate_connection);
}

// ═══════════════════════════════════════════════════════════════════════════
// 边
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.structure.connect_and_lookup", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("source").value();
    const NodeId b = g.add_node("sink").value();

    const PortRef out{a, 1, PortDirection::output};
    const PortRef in{b, 1, PortDirection::input};
    REQUIRE(g.connect(out, in));
    REQUIRE(g.edge_count() == 1);

    const Edge* e = g.incoming(in);
    REQUIRE(e != nullptr);
    REQUIRE(e->from == out);
    REQUIRE(e->to == in);

    REQUIRE(g.edges().size() == 1);
}

TEST_CASE("graph.structure.incoming_lookup_by_input", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const NodeId c = g.add_node("c").value();

    REQUIRE(g.connect(PortRef{a, 1, PortDirection::output},
                      PortRef{b, 1, PortDirection::input}));
    REQUIRE(g.connect(PortRef{c, 1, PortDirection::output},
                      PortRef{b, 2, PortDirection::input}));

    // 同一节点的不同输入端口各自有自己的入边
    const Edge* e1 = g.incoming(PortRef{b, 1, PortDirection::input});
    const Edge* e2 = g.incoming(PortRef{b, 2, PortDirection::input});
    REQUIRE(e1 != nullptr);
    REQUIRE(e2 != nullptr);
    REQUIRE(e1->from.node == a);
    REQUIRE(e2->from.node == c);

    // 无入边的端口返回 nullptr
    REQUIRE(g.incoming(PortRef{b, 3, PortDirection::input}) == nullptr);
    REQUIRE(g.incoming(PortRef{a, 1, PortDirection::input}) == nullptr);
    // 输出端口不作为"入边查询"的键
    REQUIRE(g.incoming(PortRef{a, 1, PortDirection::output}) == nullptr);
}

TEST_CASE("graph.structure.connect_rejects_duplicate_input", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const NodeId c = g.add_node("c").value();
    const PortRef in{b, 1, PortDirection::input};

    REQUIRE(g.connect(PortRef{a, 1, PortDirection::output}, in));

    // 一个输入端口至多一条入边。用"最后连的赢"掩盖它会在学生
    // 改连线顺序时产生难以复现的结果差异。
    const GraphVersion v = g.version();
    const auto r = g.connect(PortRef{c, 1, PortDirection::output}, in);
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::duplicate_connection);
    REQUIRE(g.version() == v);          // 失败不动版本号
    REQUIRE(g.edge_count() == 1);
    REQUIRE(g.incoming(in)->from.node == a);   // 原来的边仍在
}

TEST_CASE("graph.structure.connect_rejects_unknown_node", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();

    // 无效句柄
    REQUIRE_FALSE(g.connect(PortRef{}, PortRef{a, 1, PortDirection::input}));
    REQUIRE(g.connect(PortRef{}, PortRef{a, 1, PortDirection::input}).error() ==
            ErrorCode::unknown_node);

    // 已删除的句柄（世代不匹配）
    const NodeId dead = g.add_node("dead").value();
    REQUIRE(g.remove_node(dead));

    // 快照必须在**所有成功变异之后**取：add_node 与 remove_node 都会递增版本
    const GraphVersion v = g.version();

    const auto r = g.connect(PortRef{dead, 1, PortDirection::output},
                             PortRef{a, 1, PortDirection::input});
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::unknown_node);

    REQUIRE(g.version() == v);
    REQUIRE(g.edge_count() == 0);
}

TEST_CASE("graph.structure.connect_rejects_direction", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const NodeId b = g.add_node("b").value();
    const GraphVersion v = g.version();

    // input → output 是反的
    REQUIRE_FALSE(g.connect(PortRef{a, 1, PortDirection::input},
                            PortRef{b, 1, PortDirection::output}));
    // input → input
    REQUIRE_FALSE(g.connect(PortRef{a, 1, PortDirection::input},
                            PortRef{b, 1, PortDirection::input}));
    // output → output
    REQUIRE_FALSE(g.connect(PortRef{a, 1, PortDirection::output},
                            PortRef{b, 1, PortDirection::output}));

    REQUIRE(g.version() == v);
    REQUIRE(g.edge_count() == 0);
}

TEST_CASE("graph.structure.connect_rejects_self_loop", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();
    const GraphVersion v = g.version();

    const auto r = g.connect(PortRef{a, 1, PortDirection::output},
                             PortRef{a, 1, PortDirection::input});
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::cycle_detected);
    REQUIRE(g.version() == v);
}

TEST_CASE("graph.structure.connect_rejects_cycle", "[graph][structure]") {
    Chain ch = make_chain();   // a → b → c

    // c → a 会成环
    const GraphVersion v = ch.g.version();
    const auto r = ch.g.connect(PortRef{ch.c, 1, PortDirection::output},
                                PortRef{ch.a, 2, PortDirection::input});
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::cycle_detected);
    REQUIRE(ch.g.version() == v);
    REQUIRE(ch.g.edge_count() == 2);

    // 更长的环：c → a(2)，然后 a(2) → b(2)?
    // 已经拒绝了 c→a，所以先验证间接环：再造一条 a → c 的候选
    const auto r2 = ch.g.connect(PortRef{ch.c, 1, PortDirection::output},
                                 PortRef{ch.a, 1, PortDirection::input});
    REQUIRE_FALSE(r2);
    REQUIRE(r2.error() == ErrorCode::cycle_detected);
}

TEST_CASE("graph.structure.disconnect_removes_edge", "[graph][structure]") {
    Chain ch = make_chain();
    const PortRef mid_in{ch.b, 1, PortDirection::input};
    REQUIRE(ch.g.incoming(mid_in) != nullptr);

    const GraphVersion before = ch.g.version();
    REQUIRE(ch.g.disconnect(mid_in));
    REQUIRE(ch.g.edge_count() == 1);
    REQUIRE(ch.g.version() > before);
    REQUIRE(ch.g.incoming(mid_in) == nullptr);

    // 再断一次：失败且不动图
    const GraphVersion v = ch.g.version();
    const auto again = ch.g.disconnect(mid_in);
    REQUIRE_FALSE(again);
    REQUIRE(again.error() == ErrorCode::not_connected);
    REQUIRE(ch.g.version() == v);

    // 对输出端口调用 disconnect 是参数错误
    const auto wrong = ch.g.disconnect(PortRef{ch.a, 1, PortDirection::output});
    REQUIRE_FALSE(wrong);
    REQUIRE(wrong.error() == ErrorCode::invalid_argument);
}

// ═══════════════════════════════════════════════════════════════════════════
// 版本号与强异常保证
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.structure.version_bumps_on_success_only", "[graph][structure]") {
    Graph g;
    const NodeId a = g.add_node("a").value();

    const GraphVersion v0 = g.version();
    // 失败的操作不动版本号
    REQUIRE_FALSE(g.remove_node(NodeId{999, 1}));
    REQUIRE(g.version() == v0);
    REQUIRE_FALSE(g.connect(PortRef{}, PortRef{a, 1, PortDirection::input}));
    REQUIRE(g.version() == v0);
    REQUIRE_FALSE(g.add_node(""));   // 空类型名
    REQUIRE(g.version() == v0);

    // 成功的操作递增
    REQUIRE(g.add_node("b"));
    REQUIRE(g.version() > v0);
}

TEST_CASE("graph.structure.failed_mutation_is_noop", "[graph][structure]") {
    // 强异常保证是撤销栈能工作的前提：调用方必须能推理"失败了到底变了没有"。
    Chain ch = make_chain();

    struct Snapshot {
        std::size_t nodes;
        std::size_t edges;
        GraphVersion version;
        std::size_t slots;
    };
    const Snapshot snap{ch.g.node_count(), ch.g.edge_count(), ch.g.version(),
                        ch.g.slot_count()};

    // 一批必然失败的变异
    REQUIRE_FALSE(ch.g.remove_node(NodeId{999, 1}));
    REQUIRE_FALSE(ch.g.connect(PortRef{ch.c, 1, PortDirection::output},
                               PortRef{ch.a, 1, PortDirection::input}));   // 成环
    REQUIRE_FALSE(ch.g.disconnect(PortRef{ch.c, 5, PortDirection::input})); // 无入边
    REQUIRE_FALSE(ch.g.add_node_named("", "empty_type"));                   // 类型名空
    // 名字重复：先成功加一个，再加同名的必然失败
    REQUIRE(ch.g.add_node_named("t", "same"));
    const Snapshot snap2{ch.g.node_count(), ch.g.edge_count(), ch.g.version(),
                         ch.g.slot_count()};
    REQUIRE_FALSE(ch.g.add_node_named("t", "same"));   // 名字重复
    REQUIRE(ch.g.node_count() == snap2.nodes);
    REQUIRE(ch.g.edge_count() == snap2.edges);
    REQUIRE(ch.g.version() == snap2.version);
    REQUIRE(ch.g.slot_count() == snap2.slots);

    // 回到最初的快照做整体核对。
    // 注意：中间成功加过一个节点，因此节点数与槽位数各 +1，边数不变。
    REQUIRE(ch.g.edge_count() == snap.edges);
    REQUIRE(ch.g.node_count() == snap.nodes + 1);
    REQUIRE(ch.g.slot_count() == snap.slots + 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// 确定性与规模
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.structure.deterministic_iteration_order", "[graph][structure]") {
    // 遍历顺序必须稳定：UI 刷新、序列化、缓存键都依赖它。
    Graph g;
    std::vector<NodeId> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(g.add_node("n").value());
    }
    REQUIRE(g.slot_count() == 5);
    // slots() 含索引 0 的哨兵槽，因此比 slot_count 多一个
    REQUIRE(g.slots().size() == g.slot_count() + 1);
    REQUIRE_FALSE(g.slots()[0].occupied);          // 哨兵永不占用
    for (std::size_t i = 0; i < ids.size(); ++i) {
        REQUIRE(g.slots()[i + 1].node.id == ids[i]);   // 从索引 1 开始
        REQUIRE(g.slots()[i + 1].occupied);
    }
}

TEST_CASE("graph.structure.no_cycle_after_many_connects", "[graph][structure]") {
    // 建一条长链，然后尝试所有反向连边——全部必须被拒绝
    Graph g;
    std::vector<NodeId> chain;
    constexpr int kN = 12;
    for (int i = 0; i < kN; ++i) {
        chain.push_back(g.add_node("n").value());
    }
    for (int i = 0; i + 1 < kN; ++i) {
        REQUIRE(g.connect(PortRef{chain[i], 1, PortDirection::output},
                          PortRef{chain[i + 1], 1, PortDirection::input}));
    }
    REQUIRE(g.edge_count() == static_cast<std::size_t>(kN - 1));

    // 任何 i > j 的连边都会成环
    int rejected = 0;
    for (int i = 1; i < kN; ++i) {
        for (int j = 0; j < i; ++j) {
            const auto r = g.connect(PortRef{chain[i], 2, PortDirection::output},
                                     PortRef{chain[j], 2, PortDirection::input});
            REQUIRE_FALSE(r);
            REQUIRE(r.error() == ErrorCode::cycle_detected);
            ++rejected;
        }
    }
    REQUIRE(rejected == kN * (kN - 1) / 2);
    REQUIRE(g.edge_count() == static_cast<std::size_t>(kN - 1));   // 一条都没加进去
}

TEST_CASE("graph.structure.clear_resets", "[graph][structure]") {
    Chain ch = make_chain();
    const GraphVersion v = ch.g.version();
    ch.g.clear();
    REQUIRE(ch.g.node_count() == 0);
    REQUIRE(ch.g.edge_count() == 0);
    REQUIRE(ch.g.slot_count() == 0);
    REQUIRE(ch.g.version() > v);        // 清空也是一次变异
    REQUIRE_FALSE(ch.g.has_node(ch.a));
}
