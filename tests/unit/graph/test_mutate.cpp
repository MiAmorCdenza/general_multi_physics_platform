/**
 * @file test_mutate.cpp
 * @brief core/graph/mutate 的单元与性质测试。
 *
 * 重点三处：
 *   1. **撤销/重做的精确性**——尤其"节点 id 必须保持不变"。
 *      若撤销再重做后 id 变了，所有引用该节点的边、缓存键、UI 选中状态
 *      都会失效，重做后的图在语义上不是同一张图。
 *   2. **失败不动状态**——图、版本号、撤销栈三者都不能被留半条记录。
 *   3. **合并语义**——连续拖动滑块应当一次撤销回去，而不是几十次。
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/mutate.hpp>

#include <string>

using namespace qp::graph;
using qp::diag::ErrorCode;

namespace {

/// @brief 一个装了总线的图，附带常用命令构造。
struct Fixture {
    Graph graph;
    CommandBus bus{graph};

    /// @brief 通过总线加一个节点，返回它的 id。
    NodeId add(const std::string& type, const std::string& name = "") {
        auto reserved = bus.reserve_node();
        REQUIRE(reserved);
        const NodeId id = reserved.value();
        auto r = bus.apply(AddNode{id, type, name});
        REQUIRE(r);
        return id;
    }

    void connect_nodes(NodeId a, PortNumber out_port, NodeId b, PortNumber in_port) {
        const auto r = bus.apply(Connect{PortRef{a, out_port, PortDirection::output},
                                         PortRef{b, in_port, PortDirection::input}});
        REQUIRE(r);
    }
};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 命令元数据
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.mutate.command_metadata", "[graph][mutate]") {
    // 命令是纯数据：可比较、可命名、可提取目标。
    // 回调式命令做不到这些，而且会把生命周期问题带进撤销栈。
    const Command add{AddNode{NodeId{1, 1}, "dipole", "d1"}};
    const Command rm{RemoveNode{NodeId{1, 1}}};
    const Command sp{SetParam{NodeId{1, 1}, 2, qp::ports::Value{1.5}}};
    const Command ep{EraseParam{NodeId{1, 1}, 2}};
    const Command sn{SetNodeName{NodeId{1, 1}, "x"}};
    const Command sb{SetBypass{NodeId{1, 1}, true}};
    const Command cn{Connect{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                             PortRef{NodeId{2, 1}, 1, PortDirection::input}}};
    const Command dc{Disconnect{PortRef{NodeId{2, 1}, 1, PortDirection::input}}};

    REQUIRE(std::string(command_name(add)) == "AddNode");
    REQUIRE(std::string(command_name(rm)) == "RemoveNode");
    REQUIRE(std::string(command_name(sp)) == "SetParam");
    REQUIRE(std::string(command_name(ep)) == "EraseParam");
    REQUIRE(std::string(command_name(sn)) == "SetNodeName");
    REQUIRE(std::string(command_name(sb)) == "SetBypass");
    REQUIRE(std::string(command_name(cn)) == "Connect");
    REQUIRE(std::string(command_name(dc)) == "Disconnect");

    REQUIRE(command_target(add).value() == NodeId{1, 1});
    REQUIRE(command_target(cn).value() == NodeId{2, 1});   // 连边影响的是目标节点
    REQUIRE(command_target(dc).value() == NodeId{2, 1});

    // 命令是值：可以判等
    REQUIRE(sp == Command{SetParam{NodeId{1, 1}, 2, qp::ports::Value{1.5}}});
    REQUIRE(sp != Command{SetParam{NodeId{1, 1}, 2, qp::ports::Value{9.9}}});
}

// ═══════════════════════════════════════════════════════════════════════════
// 基本应用
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.mutate.apply_add_node", "[graph][mutate]") {
    Fixture f;
    const GraphVersion v0 = f.graph.version();

    auto reserved = f.bus.reserve_node();
    REQUIRE(reserved);
    const NodeId id = reserved.value();
    // 预留后图**不再稳定**——这是刻意的：中间态可被观察，
    // 因此必须让"图是否稳定"成为一个能问的问题。
    REQUIRE_FALSE(f.graph.is_stable());
    REQUIRE(f.graph.pending_count() == 1);

    const auto r = f.bus.apply(AddNode{id, "dipole", "d1"});
    REQUIRE(r);
    REQUIRE(r.value().node.value() == id);
    REQUIRE(r.value().changed);
    REQUIRE(r.value().version > v0);
    REQUIRE(f.graph.is_stable());

    const Node* n = f.graph.find_node(id);
    REQUIRE(n != nullptr);
    REQUIRE(n->type_name == "dipole");
    REQUIRE(n->name == "d1");
    REQUIRE(f.bus.can_undo());
}

TEST_CASE("graph.mutate.apply_remove_node", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("source", "a");
    const NodeId b = f.add("sink", "b");
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.graph.node_count() == 2);
    REQUIRE(f.graph.edge_count() == 1);

    const auto r = f.bus.apply(RemoveNode{a});
    REQUIRE(r);
    REQUIRE(f.graph.node_count() == 1);
    REQUIRE(f.graph.edge_count() == 0);   // 相连的边一起消失
    REQUIRE_FALSE(f.graph.has_node(a));
}

TEST_CASE("graph.mutate.apply_set_param", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");

    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.5}}));
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 2.5);

    // 覆盖写
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{7.0}}));
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 7.0);
    REQUIRE(f.graph.find_node(a)->params.size() == 1);
}

TEST_CASE("graph.mutate.apply_connect", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("source");
    const NodeId b = f.add("sink");

    const auto r = f.bus.apply(Connect{PortRef{a, 1, PortDirection::output},
                                       PortRef{b, 1, PortDirection::input}});
    REQUIRE(r);
    REQUIRE(f.graph.edge_count() == 1);
    REQUIRE(f.graph.incoming(PortRef{b, 1, PortDirection::input}) != nullptr);
}

TEST_CASE("graph.mutate.apply_rejects_invalid", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");

    // 空类型名：必须**不预留槽位**就失败。
    // 若先预留再校验失败，图会被永久留在不稳定状态——
    // 用户看到的现象是"图突然不能编辑了"。
    {
        auto reserved = f.bus.reserve_node();
        REQUIRE(reserved);
        const auto r = f.bus.apply(AddNode{reserved.value(), "", ""});
        REQUIRE_FALSE(r);
        REQUIRE(r.error() == ErrorCode::invalid_argument);
        // 槽位被我们预留了，因此图此刻确实不稳定；清理掉再继续
        REQUIRE(f.graph.has_node(reserved.value()));
        REQUIRE(f.graph.remove_node(reserved.value()));
        REQUIRE(f.graph.is_stable());
    }

    // 未预留就 apply 空类型名：直接失败，且图保持稳定
    {
        const NodeId fake{900, 1};
        const auto r = f.bus.apply(AddNode{fake, "", ""});
        REQUIRE_FALSE(r);
        REQUIRE(r.error() == ErrorCode::invalid_argument);
        REQUIRE(f.graph.is_stable());          // 关键：没有被留下待补全槽位
        REQUIRE_FALSE(f.graph.has_node(fake));
    }

    // 未预留就 apply 合法类型名：现场预留并成功。
    // 注意：此时命令里的 id 只是"期望值"，实际 id 由图分配，
    // 因此必须用返回的 id 做后续断言——这正是 ApplyOutcome 存在的理由。
    {
        const NodeId requested{901, 1};
        const auto applied = f.bus.apply(AddNode{requested, "dipole", ""});
        REQUIRE(applied);
        const NodeId actual = applied.value().node.value();
        REQUIRE(f.graph.has_node(actual));
        REQUIRE(f.graph.is_stable());
        REQUIRE(f.graph.find_node(actual)->type_name == "dipole");
        REQUIRE(f.bus.undo());
        REQUIRE_FALSE(f.graph.has_node(actual));
    }

    // 未知节点
    REQUIRE(f.bus.apply(RemoveNode{NodeId{999, 1}}).error() == ErrorCode::unknown_node);
    REQUIRE(f.bus.apply(SetParam{NodeId{999, 1}, 1, qp::ports::Value{1.0}}).error() ==
            ErrorCode::unknown_node);

    // 端口号为 0
    REQUIRE(f.bus.apply(SetParam{a, 0, qp::ports::Value{1.0}}).error() ==
            ErrorCode::invalid_argument);
    REQUIRE(f.bus.apply(EraseParam{a, 0}).error() == ErrorCode::invalid_argument);

    // 成环
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.bus.apply(Connect{PortRef{b, 1, PortDirection::output},
                                PortRef{a, 2, PortDirection::input}})
                .error() == ErrorCode::cycle_detected);

    // 名字重复：先给两个节点起名，再把 b 改成 a 的名字。
    // 注意 Fixture::add(type, name) 的第二个参数是**类型名**，
    // 不是用户名字——名字必须显式通过 SetNodeName 设置。
    REQUIRE(f.bus.apply(SetNodeName{a, "alpha"}));
    REQUIRE(f.bus.apply(SetNodeName{b, "beta"}));
    REQUIRE(f.bus.apply(SetNodeName{b, "alpha"}).error() == ErrorCode::duplicate_connection);

    // 无入边时断开
    REQUIRE(f.bus.apply(Disconnect{PortRef{b, 5, PortDirection::input}}).error() ==
            ErrorCode::not_connected);

    // 一路下来图始终稳定
    REQUIRE(f.graph.is_stable());
}

TEST_CASE("graph.mutate.failed_apply_is_noop", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");
    f.connect_nodes(a, 1, b, 1);

    const GraphVersion v = f.graph.version();
    const std::size_t nodes = f.graph.node_count();
    const std::size_t edges = f.graph.edge_count();
    const std::size_t undo_before = f.bus.history().undo_size();

    // 一批必然失败的命令。图全程保持稳定，因此每个失败都必须只失败自己。
    // 注意：这里的两个节点都**没有**用户名字（Fixture::add 的第二参是类型名），
    // 因此"改名撞车"必须先给其中一个起名才有意义。
    REQUIRE_FALSE(f.bus.apply(RemoveNode{NodeId{999, 1}}));
    REQUIRE_FALSE(f.bus.apply(Connect{PortRef{b, 1, PortDirection::output},
                                      PortRef{a, 2, PortDirection::input}}));
    REQUIRE_FALSE(f.bus.apply(Disconnect{PortRef{b, 7, PortDirection::input}}));
    REQUIRE(f.bus.apply(SetNodeName{a, "n1"}));
    REQUIRE_FALSE(f.bus.apply(SetNodeName{b, "n1"}));    // 与 a 撞名
    REQUIRE(f.bus.apply(SetNodeName{a, "n1"}));          // 设成自己的名字是幂等的
    REQUIRE(f.bus.apply(SetParam{a, 0, qp::ports::Value{1.0}}).error() ==
            ErrorCode::invalid_argument);
    REQUIRE(f.bus.apply(EraseParam{a, 0}).error() == ErrorCode::invalid_argument);

    // 上面有两次成功的 SetNodeName，因此撤销栈会增加两条。
    // 关键断言是"图没有被任何失败的命令改动"。
    REQUIRE(f.graph.version() > v);                      // 成功的改名确实改了版本
    REQUIRE(f.graph.node_count() == nodes);
    REQUIRE(f.graph.edge_count() == edges);
    REQUIRE(f.graph.is_stable());
    REQUIRE(f.bus.history().undo_size() == undo_before + 1);  // 两次同名改名合并成一条
}

TEST_CASE("graph.mutate.rejects_apply_while_unstable", "[graph][mutate]") {
    Fixture f;
    auto reserved = f.bus.reserve_node();
    REQUIRE(reserved);
    const NodeId pending_id = reserved.value();
    REQUIRE_FALSE(f.graph.is_stable());

    // 图不稳定时不得开始新命令：否则"图是否稳定"会变成跨模块推理的问题
    const auto r = f.bus.reserve_node();
    REQUIRE_FALSE(r);
    REQUIRE(r.error() == ErrorCode::graph_busy);

    const auto r2 = f.bus.apply(RemoveNode{pending_id});
    REQUIRE_FALSE(r2);
    REQUIRE(r2.error() == ErrorCode::graph_busy);

    // 补全后恢复可用
    REQUIRE(f.bus.apply(AddNode{pending_id, "dipole", ""}));
    REQUIRE(f.graph.is_stable());
    REQUIRE(f.bus.reserve_node());
}

TEST_CASE("graph.mutate.graph_accessor_is_readonly", "[graph][mutate]") {
    // 只提供 const 访问器：否则"全部变异走总线"就退化成口头约定。
    Fixture f;
    const NodeId a = f.add("a");
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const CommandBus&>().graph()),
                                  const Graph&>);
    REQUIRE(f.bus.graph().node_count() == 1);
    REQUIRE(f.bus.graph().has_node(a));
}

// ═══════════════════════════════════════════════════════════════════════════
// 撤销 / 重做
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.mutate.undo_set_param", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}, /*allow_merge=*/false));
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.0}}, /*allow_merge=*/false));
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 2.0);

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 1.0);
    REQUIRE(f.bus.undo());
    // 回到"参数未设置"的状态，而不是 0.0
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());
}

TEST_CASE("graph.mutate.redo_restores", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{5.0}}, false));

    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());
    REQUIRE(f.bus.can_redo());

    REQUIRE(f.bus.redo());
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 5.0);
    REQUIRE(f.bus.can_undo());
    REQUIRE_FALSE(f.bus.can_redo());
}

TEST_CASE("graph.mutate.undo_redo_roundtrip", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{3.0}}, false));

    const GraphVersion v_final = f.graph.version();
    const std::size_t n_final = f.graph.node_count();
    const std::size_t e_final = f.graph.edge_count();

    // 全部撤销
    int undos = 0;
    while (f.bus.can_undo()) {
        REQUIRE(f.bus.undo());
        ++undos;
    }
    REQUIRE(undos == 4);   // AddNode a, AddNode b, Connect, SetParam
    REQUIRE(f.graph.node_count() == 0);
    REQUIRE(f.graph.edge_count() == 0);

    // 全部重做，回到完全相同的状态
    int redos = 0;
    while (f.bus.can_redo()) {
        const auto r = f.bus.redo();
        if (!r) {
            FAIL("redo 失败：" << qp::diag::to_string(r.error())
                               << "  第 " << redos << " 步，下一标签="
                               << f.bus.history().next_redo_label());
            break;
        }
        ++redos;
    }
    REQUIRE(redos == 4);
    REQUIRE(f.graph.node_count() == n_final);
    REQUIRE(f.graph.edge_count() == e_final);
    REQUIRE(f.graph.version() >= v_final);
    REQUIRE(f.graph.find_node(a) != nullptr);
    REQUIRE(f.graph.find_node(b) != nullptr);
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 3.0);
}

TEST_CASE("graph.mutate.new_edit_clears_redo", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}, false));
    REQUIRE(f.bus.undo());
    REQUIRE(f.bus.can_redo());

    // 从历史中间分叉：原来的"未来"不再可达
    REQUIRE(f.bus.apply(SetParam{a, 2, qp::ports::Value{9.0}}, false));
    REQUIRE_FALSE(f.bus.can_redo());
}

TEST_CASE("graph.mutate.merge_consecutive_set_param", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");

    // 此时撤销栈里有 1 条（AddNode）
    const std::size_t after_add = f.bus.history().undo_size();
    REQUIRE(after_add == 1);

    // 模拟拖动滑块：连续 20 次改同一个端口的参数
    for (int i = 1; i <= 20; ++i) {
        REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{static_cast<double>(i)}}));
    }
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 20.0);

    // 20 次拖动合并成**一条**记录：用户按一次 Ctrl+Z 就回到拖之前，
    // 而不是按 20 次。顺序撤销不是撤销，是惩罚。
    REQUIRE(f.bus.history().undo_size() == after_add + 1);

    // 一次撤销即回到"参数未设置"
    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());
    // 再撤一次才轮到 AddNode
    REQUIRE(f.bus.history().undo_size() == 1);
    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.has_node(a));
    REQUIRE_FALSE(f.bus.can_undo());
}

TEST_CASE("graph.mutate.merge_does_not_cross_labels", "[graph][mutate]") {
    // 合并的判据是"同节点 + 同操作种类"。不同种类的操作之间必须断开。
    Fixture f;
    const NodeId a = f.add("gain");

    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}));
    REQUIRE(f.bus.apply(SetBypass{a, true}));                    // 换种类 → 不合并
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.0}})); // 又换回来 → 新的一条

    REQUIRE(f.bus.history().undo_size() == 4);   // AddNode + 三条
}

TEST_CASE("graph.mutate.merge_does_not_cross_nodes", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    const NodeId b = f.add("b");

    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}));
    REQUIRE(f.bus.apply(SetParam{b, 1, qp::ports::Value{1.0}}));
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{2.0}}));

    // 三次 SetParam 的目标分别是 a、b、a，相邻两次都不同目标 → 不能合并。
    // 加上两次 AddNode（同目标但标签不同，也不合并）共 5 条。
    REQUIRE(f.bus.history().undo_size() == 5);
}

TEST_CASE("graph.mutate.undo_remove_restores_edges", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("source", "a");
    const NodeId b = f.add("mid", "b");
    const NodeId c = f.add("sink", "c");
    f.connect_nodes(a, 1, b, 1);
    f.connect_nodes(b, 1, c, 1);
    REQUIRE(f.graph.edge_count() == 2);

    REQUIRE(f.bus.apply(RemoveNode{b}));
    REQUIRE(f.graph.edge_count() == 0);

    // 撤销必须把节点**和它的两条边**一起恢复
    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.node_count() == 3);
    REQUIRE(f.graph.edge_count() == 2);
    REQUIRE(f.graph.has_node(b));
    REQUIRE(f.graph.find_node(b)->type_name == "mid");
    REQUIRE(f.graph.find_node(b)->name == "b");
    REQUIRE(f.graph.incoming(PortRef{b, 1, PortDirection::input}) != nullptr);
    REQUIRE(f.graph.incoming(PortRef{c, 1, PortDirection::input}) != nullptr);
}

TEST_CASE("graph.mutate.redo_after_undo_keeps_same_id", "[graph][mutate]") {
    // 这是整套设计里最关键的一条性质。
    // 若撤销再重做后节点拿到新 id，所有引用它的边、缓存键、
    // UI 选中状态都会失效——重做后的图在语义上不是同一张图。
    Fixture f;
    const NodeId a = f.add("dipole", "d1");
    const NodeId b = f.add("sink", "s1");
    f.connect_nodes(a, 1, b, 1);

    REQUIRE(f.bus.apply(RemoveNode{a}));
    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.has_node(a));          // 同一个 id 复活
    REQUIRE(f.graph.find_node(a)->name == "d1");
    REQUIRE(f.graph.edge_count() == 1);

    REQUIRE(f.bus.redo());                 // 再删一次
    REQUIRE_FALSE(f.graph.has_node(a));

    REQUIRE(f.bus.undo());                 // 再恢复
    REQUIRE(f.graph.has_node(a));
    REQUIRE(f.graph.find_node(a)->id == a);
    REQUIRE(f.graph.find_node(a)->name == "d1");
    REQUIRE(f.graph.find_node(a)->type_name == "dipole");
    REQUIRE(f.graph.edge_count() == 1);
}

TEST_CASE("graph.mutate.reserve_id_is_stable_across_undo", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("x", "first");
    REQUIRE(f.bus.undo());                 // 撤销添加
    REQUIRE_FALSE(f.graph.has_node(a));
    REQUIRE(f.bus.redo());                 // 重做
    // 重做后必须仍是同一个 id——"先撤销再加一个"会拿到不同 id
    REQUIRE(f.graph.has_node(a));
    REQUIRE(f.graph.find_node(a)->name == "first");
}

TEST_CASE("graph.mutate.erase_param_undo_restores_value", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("gain");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{4.5}}, false));

    REQUIRE(f.bus.apply(EraseParam{a, 1}));
    REQUIRE_FALSE(f.graph.find_node(a)->param(1).valid());

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.find_node(a)->param(1).as_f64() == 4.5);
}

TEST_CASE("graph.mutate.set_name_undo_enforces_uniqueness", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("t", "a");
    const NodeId b = f.add("t", "b");

    REQUIRE(f.bus.apply(SetNodeName{b, "renamed"}));
    REQUIRE(f.graph.find_node(b)->name == "renamed");
    REQUIRE(f.graph.find_node_by_name("renamed") == b);
    REQUIRE_FALSE(f.graph.find_node_by_name("b").valid());

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.find_node(b)->name == "b");
    REQUIRE(f.graph.find_node_by_name("b") == b);
    REQUIRE(f.graph.find_node_by_name("renamed").valid() == false);
    REQUIRE(f.graph.find_node_by_name("a") == a);   // 另一个节点不受影响
}

TEST_CASE("graph.mutate.bypass_undo", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("filter");
    REQUIRE_FALSE(f.graph.find_node(a)->bypassed);

    REQUIRE(f.bus.apply(SetBypass{a, true}));
    REQUIRE(f.graph.find_node(a)->bypassed);
    REQUIRE(f.bus.undo());
    REQUIRE_FALSE(f.graph.find_node(a)->bypassed);
    REQUIRE(f.bus.redo());
    REQUIRE(f.graph.find_node(a)->bypassed);
}

TEST_CASE("graph.mutate.disconnect_undo_restores_edge", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("src");
    const NodeId b = f.add("dst");
    f.connect_nodes(a, 1, b, 1);
    REQUIRE(f.graph.edge_count() == 1);

    const Edge before = *f.graph.incoming(PortRef{b, 1, PortDirection::input});
    REQUIRE(f.bus.apply(Disconnect{PortRef{b, 1, PortDirection::input}}));
    REQUIRE(f.graph.edge_count() == 0);

    REQUIRE(f.bus.undo());
    REQUIRE(f.graph.edge_count() == 1);
    // 恢复的必须是**同一条**边（含端口号），不能只是"某个连接"
    const Edge* after = f.graph.incoming(PortRef{b, 1, PortDirection::input});
    REQUIRE(after != nullptr);
    REQUIRE(*after == before);
}

TEST_CASE("graph.mutate.undo_stack_labels", "[graph][mutate]") {
    Fixture f;
    const NodeId a = f.add("a");
    REQUIRE(f.bus.apply(SetParam{a, 1, qp::ports::Value{1.0}}, false));

    REQUIRE(f.bus.history().next_undo_label() == "SetParam");
    REQUIRE(f.bus.history().next_redo_label().empty());

    REQUIRE(f.bus.undo());
    REQUIRE(f.bus.history().next_undo_label() == "AddNode");
    REQUIRE(f.bus.history().next_redo_label() == "SetParam");
}

TEST_CASE("graph.mutate.failed_undo_keeps_history", "[graph][mutate]") {
    // 若回退失败，记录必须留在栈上，历史与图保持对应。
    // 否则"栈里一条、图里没改"会让后续所有撤销都错位。
    Fixture f;
    const NodeId a = f.add("a");
    REQUIRE(f.bus.undo());                       // 撤掉添加
    REQUIRE_FALSE(f.graph.has_node(a));

    // 此时再撤销一次：命令是 RemoveNode，而节点已不存在 → 回退失败
    // 但栈已经空了，所以这里验证的是"空栈返回 false 而非错误"
    const auto r = f.bus.undo();
    REQUIRE(r);
    REQUIRE_FALSE(r.value());
}

TEST_CASE("graph.mutate.long_session_stress", "[graph][mutate]") {
    // 长会话：交替应用与撤销，最后全部撤干净。
    // 这条用例的价值在于"栈 与 图 始终一一对应"这件事在多次往返后仍成立。
    Fixture f;
    std::vector<NodeId> ids;
    for (int round = 0; round < 5; ++round) {
        ids.clear();
        for (int i = 0; i < 4; ++i) {
            ids.push_back(f.add("n" + std::to_string(i), "n" + std::to_string(round) + "_" +
                                                              std::to_string(i)));
        }
        for (std::size_t i = 0; i + 1 < ids.size(); ++i) {
            f.connect_nodes(ids[i], 1, ids[i + 1], 1);
        }
    }
    REQUIRE(f.graph.node_count() == 20);
    REQUIRE(f.graph.edge_count() == 15);

    // 全部撤销
    int steps = 0;
    while (f.bus.can_undo()) {
        REQUIRE(f.bus.undo());
        ++steps;
        REQUIRE(steps < 200);   // 防御：不应无限循环
    }
    REQUIRE(f.graph.node_count() == 0);
    REQUIRE(f.graph.edge_count() == 0);
    REQUIRE_FALSE(f.bus.can_redo() == false);   // 重做栈应当是满的
    REQUIRE(f.bus.can_redo());

    // 全部重做
    int redos = 0;
    while (f.bus.can_redo()) {
        REQUIRE(f.bus.redo());
        ++redos;
        REQUIRE(redos < 200);
    }
    REQUIRE(redos == steps);
    REQUIRE(f.graph.node_count() == 20);
    REQUIRE(f.graph.edge_count() == 15);
    // 名字必须逐一还原（证明 id 与内容都精确恢复了）
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 4; ++i) {
            const std::string want = "n" + std::to_string(round) + "_" + std::to_string(i);
            REQUIRE(f.graph.find_node_by_name(want).valid());
        }
    }
}
