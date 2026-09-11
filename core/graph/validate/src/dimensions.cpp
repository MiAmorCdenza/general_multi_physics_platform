/**
 * @file dimensions.cpp
 * @brief 量纲解析的实现。
 *
 * 算法：Kahn 拓扑排序 + 沿序推进。
 *
 * 每个节点的每个输出端口，其量纲按以下优先级确定：
 *   1. 端口类型声明为 `exact`  → 直接用声明的量纲
 *   2. 端口类型声明为 `any`    → 未知（不约束，也无从推断）
 *   3. 端口类型声明为 `same_as_input` → 取**第一个已连线的输入端口**的量纲；
 *      若该输入本身是 `same_as_input`，则取它上游那条边的源输出量纲
 *      （上游已在拓扑序中先被解析）
 *
 * 遇到无法解析的情况**标记为未知并继续**，绝不崩溃或死循环——
 * 解析器可能被用在"结构校验还没跑"的时刻。
 */
#include <qp/graph/validate/dimensions.hpp>

#include <algorithm>
#include <deque>

namespace qp::graph {
namespace {

/// @brief 取节点类型描述。未注册返回 nullptr。
[[nodiscard]] const NodeDesc* desc_of(const Graph& g, const ResolveContext& ctx, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr || n->type_name.empty()) return nullptr;
    return ctx.catalog->find(n->type_name);
}

/// @brief 取输入端口的描述。找不到返回 nullptr。
[[nodiscard]] const PortDesc* input_desc(const NodeDesc* d, PortNumber port) {
    return d == nullptr ? nullptr : d->find_port(port, /*is_output=*/false);
}

/// @brief 取输出端口的描述。找不到返回 nullptr。
[[nodiscard]] const PortDesc* output_desc(const NodeDesc* d, PortNumber port) {
    return d == nullptr ? nullptr : d->find_port(port, /*is_output=*/true);
}

}  // namespace

const ResolvedDimension* DimensionMap::find(NodeId node, PortNumber port) const noexcept {
    for (const auto& e : entries_) {
        if (e.node == node && e.port == port) return &e;
    }
    return nullptr;
}

DimensionMap resolve_dimensions(const Graph& g, const ResolveContext& ctx) noexcept {
    DimensionMap out;
    if (!ctx.valid()) return out;

    // ── 拓扑序（Kahn）────────────────────────────────────────────────────────
    //
    // 入度 = 该节点有多少个"有入边的输入端口"。
    const std::size_t n = g.slots().size();
    std::vector<std::size_t> indegree(n, 0);
    for (const auto& e : g.edges()) {
        if (e.to.node.index < n) ++indegree[e.to.node.index];
    }

    std::deque<NodeId> ready;
    for (const auto& s : g.slots()) {
        if (s.occupied && s.node.id.index < n && indegree[s.node.id.index] == 0) {
            ready.push_back(s.node.id);
        }
    }

    // 已解析的输出端口量纲，按 (node, port) 查询
    DimensionMap resolved;

    // 取某个输入端口的来源输出端口。无入边返回无效。
    const auto source_of = [&g](NodeId node, PortNumber port) -> PortRef {
        const Edge* e = g.incoming(PortRef{node, port, PortDirection::input});
        return e == nullptr ? PortRef{} : e->from;
    };

    std::size_t processed = 0;
    while (!ready.empty()) {
        const NodeId id = ready.front();
        ready.pop_front();
        ++processed;

        const NodeDesc* d = desc_of(g, ctx, id);
        if (d != nullptr) {
            for (const auto& out_port : d->outputs) {
                const qp::ports::PortTypeDesc* t = ctx.types->find(out_port.type);
                if (t == nullptr) {
                    resolved.set_unknown(id, out_port.number);
                    continue;
                }
                switch (t->constraint) {
                    case qp::ports::DimensionConstraint::exact:
                        resolved.set(id, out_port.number, t->dimension);
                        break;

                    case qp::ports::DimensionConstraint::any:
                        resolved.set_unknown(id, out_port.number);
                        break;

                    case qp::ports::DimensionConstraint::same_as_input: {
                        // 取第一个已连线的输入端口的量纲
                        bool found = false;
                        for (const auto& in_port : d->inputs) {
                            const PortRef src = source_of(id, in_port.number);
                            if (!src.valid()) continue;
                            const ResolvedDimension* r =
                                resolved.find(src.node, src.port);
                            if (r != nullptr && r->known) {
                                resolved.set(id, out_port.number, r->dimension);
                                found = true;
                            } else {
                                // 上游还没解析出来（或上游本身未知）
                                resolved.set_unknown(id, out_port.number);
                                found = true;
                            }
                            break;
                        }
                        if (!found) {
                            // 没有任何已连线的输入 → 无从推断
                            resolved.set_unknown(id, out_port.number);
                        }
                        break;
                    }
                }
            }
        }

        // 推进下游
        for (const auto& e : g.edges()) {
            if (e.from.node != id) continue;
            if (e.to.node.index >= n) continue;
            if (indegree[e.to.node.index] == 0) continue;
            if (--indegree[e.to.node.index] == 0) {
                ready.push_back(e.to.node);
            }
        }
    }

    // 有环（理论上不该发生：结构层已保证无环）→ 剩余节点标为未知，而不是崩溃
    if (processed < g.node_count()) {
        for (const auto& s : g.slots()) {
            if (!s.occupied) continue;
            const NodeDesc* d = desc_of(g, ctx, s.node.id);
            if (d == nullptr) continue;
            for (const auto& out_port : d->outputs) {
                if (resolved.find(s.node.id, out_port.number) == nullptr) {
                    resolved.set_unknown(s.node.id, out_port.number);
                }
            }
        }
    }

    // 把结果搬到输出（按 slots 顺序，保证确定性）
    for (const auto& s : g.slots()) {
        if (!s.occupied) continue;
        const NodeDesc* d = desc_of(g, ctx, s.node.id);
        if (d == nullptr) continue;
        for (const auto& out_port : d->outputs) {
            const ResolvedDimension* r = resolved.find(s.node.id, out_port.number);
            if (r == nullptr) {
                out.set_unknown(s.node.id, out_port.number);
            } else if (r->known) {
                out.set(s.node.id, out_port.number, r->dimension);
            } else {
                out.set_unknown(s.node.id, out_port.number);
            }
        }
    }
    return out;
}

}  // namespace qp::graph
