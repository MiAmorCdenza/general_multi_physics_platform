/**
 * @file test_ir.cpp
 * @brief core/graph/ir 的单元与性质测试。
 *
 * 用例 id 与 core/graph/ir/include/qp/graph/ir.hpp 的 @tests 字段逐字对应。
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/ir.hpp>

#include <string>
#include <type_traits>
#include <vector>

using namespace qp::graph;
using qp::ports::Value;

// ═══════════════════════════════════════════════════════════════════════════
// ids.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.ids.node_default_is_invalid", "[graph][ir]") {
    const NodeId n;
    REQUIRE_FALSE(n.valid());
    REQUIRE(n.index == kNoSlot);
    REQUIRE(n.generation == kNoGeneration);

    // 只有一半有效也是无效：0 索引或 0 世代都不构成合法句柄
    REQUIRE_FALSE((NodeId{0, 5}.valid()));
    REQUIRE_FALSE((NodeId{5, 0}.valid()));
    REQUIRE((NodeId{5, 1}.valid()));
}

TEST_CASE("graph.ids.node_equality", "[graph][ir]") {
    STATIC_REQUIRE(sizeof(NodeId) == 8);
    STATIC_REQUIRE(std::is_trivially_copyable_v<NodeId>);
    STATIC_REQUIRE(NodeId{} == NodeId{});
    STATIC_REQUIRE((NodeId{1, 1} == NodeId{1, 1}));
    STATIC_REQUIRE((NodeId{1, 1} != NodeId{1, 2}));
    STATIC_REQUIRE((NodeId{1, 1} != NodeId{2, 1}));
}

TEST_CASE("graph.ids.node_generation_matters", "[graph][ir]") {
    // 世代是这套 ID 方案存在的**唯一理由**：
    // 槽位复用后，老句柄必须能与新句柄区分开，
    // 否则撤销栈/缓存键/UI 选中状态会静默指向别的节点。
    const NodeId old_handle{3, 1};
    const NodeId new_handle{3, 2};   // 同一个槽位，被复用
    REQUIRE(old_handle != new_handle);
    REQUIRE(old_handle.index == new_handle.index);
    REQUIRE(old_handle.generation != new_handle.generation);
}

TEST_CASE("graph.ids.port_ref_default_is_invalid", "[graph][ir]") {
    const PortRef r;
    REQUIRE_FALSE(r.valid());
    REQUIRE(r.port == kNoPort);
    REQUIRE(r.direction == PortDirection::input);

    REQUIRE_FALSE((PortRef{NodeId{}, 1, PortDirection::output}.valid()));   // 无节点
    REQUIRE_FALSE((PortRef{NodeId{1, 1}, 0, PortDirection::output}.valid())); // 无端口
    REQUIRE((PortRef{NodeId{1, 1}, 1, PortDirection::output}.valid()));
}

TEST_CASE("graph.ids.port_ref_equality", "[graph][ir]") {
    const PortRef a{NodeId{1, 1}, 2, PortDirection::input};
    const PortRef b{NodeId{1, 1}, 2, PortDirection::input};
    const PortRef c{NodeId{1, 1}, 2, PortDirection::output};
    REQUIRE(a == b);
    REQUIRE(a != c);   // 方向不同即不同
    REQUIRE(a != PortRef{NodeId{1, 2}, 2, PortDirection::input});   // 世代不同
}

// ═══════════════════════════════════════════════════════════════════════════
// descriptor.hpp
// ═══════════════════════════════════════════════════════════════════════════

namespace {

PortDesc make_port(PortNumber n, const char* name, qp::ports::PortTypeId t) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = t;
    return p;
}

NodeDesc make_node() {
    NodeDesc d{};
    d.type_name = "dipole";
    d.label = "偶极场";
    d.category = "磁场";
    d.inputs.push_back(make_port(1, "moment", qp::ports::kScalarF64));
    d.inputs.push_back(make_port(2, "tilt", qp::ports::kScalarF64));
    d.outputs.push_back(make_port(1, "field", qp::ports::kVectorField));
    d.has_compute = true;
    return d;
}

}  // namespace

TEST_CASE("graph.desc.port_basic", "[graph][ir]") {
    PortDesc p{};
    REQUIRE_FALSE(p.valid());
    p.number = 1;
    p.name = "mass";
    REQUIRE(p.valid());
    REQUIRE(p.label.empty());
    REQUIRE(p.connectable);      // 默认可连线
    REQUIRE_FALSE(p.required);   // 默认非必需
    REQUIRE(p.unit_factor == 1.0);   // 默认 1.0：用户直接填 SI 值
}

TEST_CASE("graph.desc.port_connectable_flag", "[graph][ir]") {
    // Param 与 Port 统一的落点：同一个结构，一个标志区分。
    // 这消除了"校验/UI/序列化各写两遍"的重复实现。
    PortDesc param{};
    param.number = 1;
    param.name = "density";
    param.connectable = false;   // 只能填值
    REQUIRE(param.valid());

    PortDesc socket{};
    socket.number = 2;
    socket.name = "signal";
    socket.connectable = true;
    REQUIRE(socket.valid());

    // 两者的结构完全相同——只有标志不同
    REQUIRE(sizeof(param) == sizeof(socket));
}

TEST_CASE("graph.desc.port_numeric_bounds", "[graph][ir]") {
    PortDesc p{};
    p.number = 1;
    p.name = "angle";
    p.type = qp::ports::kScalarF64;
    p.has_range = true;
    p.min_value = -180.0;
    p.max_value = 180.0;
    p.step = 1.0;
    p.unit_factor = 1.0;
    p.unit_symbol = "deg";   // 显示用；量纲仍由端口类型决定

    REQUIRE(p.has_range);
    REQUIRE(p.min_value == -180.0);
    REQUIRE(p.max_value == 180.0);
    REQUIRE(p.unit_symbol == "deg");
}

TEST_CASE("graph.desc.port_choices", "[graph][ir]") {
    PortDesc p{};
    p.number = 1;
    p.name = "axis";
    p.type = qp::ports::kEnum;
    p.choice_names = {"x", "y", "z"};
    p.choice_labels = {"X 轴", "Y 轴", "Z 轴"};

    REQUIRE(p.choice_names.size() == 3);
    REQUIRE(p.choice_names.size() == p.choice_labels.size());
    REQUIRE(p.choice_names[0] == "x");
}

TEST_CASE("graph.desc.input_view_lookup", "[graph][ir]") {
    const PortValues values{{1, Value{2.5}}, {2, Value{std::int64_t{7}}}};
    const InputView in{values};

    REQUIRE(in.f64(1) == 2.5);
    REQUIRE(in.i64(2) == 7);
    // 缺失端口返回兜底值，不崩
    REQUIRE(in.f64(99) == 0.0);
    REQUIRE(in.i64(99) == 0);
    REQUIRE_FALSE(in.boolean(99));
    REQUIRE(in.text(99).empty());
    // 类型不符也返回兜底值
    REQUIRE(in.i64(1) == 0);
}

TEST_CASE("graph.desc.node_basic", "[graph][ir]") {
    const NodeDesc d = make_node();
    REQUIRE(d.valid());
    REQUIRE(d.type_name == "dipole");
    REQUIRE(d.version == 1);
    REQUIRE(d.inputs.size() == 2);
    REQUIRE(d.outputs.size() == 1);
    REQUIRE(d.has_compute);
    // 默认域策略：烘焙域允许，实时域**拒绝**（保守）
    REQUIRE(d.allow_in_field_domain);
    REQUIRE_FALSE(d.allow_in_particle_domain);
}

TEST_CASE("graph.desc.node_port_lookup", "[graph][ir]") {
    const NodeDesc d = make_node();

    const PortDesc* m = d.find_port(1, /*is_output=*/false);
    REQUIRE(m != nullptr);
    REQUIRE(m->name == "moment");

    REQUIRE(d.find_port(2, false) != nullptr);
    REQUIRE(d.find_port(3, false) == nullptr);
    REQUIRE(d.find_port(1, true) != nullptr);          // 输出端口 1
    REQUIRE(d.find_port(2, true) == nullptr);
    REQUIRE(d.find_port(0, false) == nullptr);         // 0 号端口不存在

    const PortDesc* by_name = d.find_by_name("tilt", false);
    REQUIRE(by_name != nullptr);
    REQUIRE(by_name->number == 2);
    REQUIRE(d.find_by_name("nope", false) == nullptr);
    REQUIRE(d.find_by_name("field", true) != nullptr);
    REQUIRE(d.find_by_name("field", false) == nullptr);   // 输出名不在输入里
    REQUIRE(d.find_by_name("", false) == nullptr);
}

TEST_CASE("graph.desc.node_output_count", "[graph][ir]") {
    NodeDesc d = make_node();
    REQUIRE(d.output_count() == 1);
    d.outputs.clear();
    REQUIRE(d.output_count() == 0);   // 无输出 → 无需求值
}

TEST_CASE("graph.desc.node_has_hooks", "[graph][ir]") {
    // 节点只有五个字段级的"能力声明"，没有生命周期钩子。
    // 这是**上限**：加第六个之前必须走 ADR。
    const NodeDesc d = make_node();
    REQUIRE(d.has_compute);
    // 三个可选钩子的能力由 has_compute + 两个域标志表达，不含状态
    REQUIRE(d.allow_in_field_domain);
    REQUIRE_FALSE(d.allow_in_particle_domain);
}

// ═══════════════════════════════════════════════════════════════════════════
// node.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.node.construction", "[graph][ir]") {
    Node n{};
    REQUIRE_FALSE(n.id.valid());
    REQUIRE(n.type_name.empty());
    REQUIRE(n.params.empty());
    REQUIRE_FALSE(n.bypassed);
    REQUIRE(n.order_hint == 0);
}

TEST_CASE("graph.node.param_lookup", "[graph][ir]") {
    Node n{};
    n.id = NodeId{1, 1};
    n.type_name = "dipole";
    n.set_param(1, Value{3.5});
    n.set_param(2, Value{std::int64_t{4}});

    REQUIRE(n.param(1).as_f64() == 3.5);
    REQUIRE(n.param(2).as_i64() == 4);
    // 未设置的参数返回无效值
    REQUIRE_FALSE(n.param(99).valid());
    REQUIRE_FALSE(n.param(0).valid());
}

TEST_CASE("graph.node.set_param_replaces", "[graph][ir]") {
    Node n{};
    n.set_param(1, Value{1.0});
    n.set_param(1, Value{2.0});      // 替换，不是追加
    REQUIRE(n.params.size() == 1);
    REQUIRE(n.param(1).as_f64() == 2.0);

    // 替换可以改变类型
    n.set_param(1, Value{std::string{"text"}});
    REQUIRE(n.params.size() == 1);
    REQUIRE(n.param(1).as_text() == "text");

    // 删除
    REQUIRE(n.erase_param(1));
    REQUIRE_FALSE(n.erase_param(1));   // 已不存在
    REQUIRE(n.params.empty());
    REQUIRE_FALSE(n.param(1).valid());
}

TEST_CASE("graph.node.bypass_flag", "[graph][ir]") {
    Node n{};
    REQUIRE_FALSE(n.bypassed);
    n.bypassed = true;
    REQUIRE(n.bypassed);
    // 绕过是**运行期语义**，不改结构：节点仍在图里，边仍然存在
    REQUIRE(n.params.empty());
}

TEST_CASE("graph.node.user_name_is_separate_from_id", "[graph][ir]") {
    // id 是内部寻址句柄（含世代，删除后失效）；
    // name 是给人看的标签（YAML 键、报告引用、错误信息）。
    // 两者必须分开：改名不应影响任何内部引用。
    Node n{};
    n.id = NodeId{7, 3};
    n.name = "spring_1";
    n.type_name = "spring";

    REQUIRE(n.id.valid());
    REQUIRE(n.name == "spring_1");

    const NodeId id_before = n.id;
    n.name = "春天的弹簧";           // 改名
    REQUIRE(n.id == id_before);      // 内部句柄不变
}

// ═══════════════════════════════════════════════════════════════════════════
// edge.hpp
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("graph.edge.construction", "[graph][ir]") {
    const Edge e{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                 PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    REQUIRE(e.valid());
    REQUIRE(e.from.node == NodeId{1, 1});
    REQUIRE(e.to.node == NodeId{2, 1});
}

TEST_CASE("graph.edge_equality", "[graph][ir]") {
    const Edge a{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                 PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    Edge b = a;
    REQUIRE(a == b);
    b.to.port = 2;
    REQUIRE(a != b);
    b = a;
    b.from.node.generation = 2;   // 源节点世代变了 → 是另一条边
    REQUIRE(a != b);
    b = a;
    b.to.direction = PortDirection::output;   // 方向变了 → 是另一条边
    REQUIRE(a != b);
}

TEST_CASE("graph.edge_direction_invariant", "[graph][ir]") {
    // 不变量：from 必须是输出，to 必须是输入。
    // 方向反了不是"另一种合法的边"，而是错误。
    const Edge reversed{PortRef{NodeId{1, 1}, 1, PortDirection::input},
                        PortRef{NodeId{2, 1}, 1, PortDirection::output}};
    REQUIRE_FALSE(reversed.valid());

    const Edge out_to_out{PortRef{NodeId{1, 1}, 1, PortDirection::output},
                          PortRef{NodeId{2, 1}, 1, PortDirection::output}};
    REQUIRE_FALSE(out_to_out.valid());

    const Edge in_to_in{PortRef{NodeId{1, 1}, 1, PortDirection::input},
                        PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    REQUIRE_FALSE(in_to_in.valid());

    const Edge missing_node{PortRef{}, PortRef{NodeId{2, 1}, 1, PortDirection::input}};
    REQUIRE_FALSE(missing_node.valid());
}
