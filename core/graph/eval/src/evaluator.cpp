/**
 * @file evaluator.cpp
 * @brief 求值的实现：Kahn 拓扑序 + 内容寻址缓存。
 *
 * 顺序上有一处必须小心：**节点被求值之前，它的全部输入必须已经就绪**。
 * 这由拓扑序保证，而拓扑序在同一层内有多个可选节点时按**槽位索引**
 * 打破平局——这是"确定性"的一部分：不固定平局规则，
 * 两次运行的调用顺序就可能不同，而任何有隐藏状态的实现都会因此给出不同结果。
 */
#include <qp/graph/eval/evaluator.hpp>

#include <algorithm>
#include <deque>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

[[nodiscard]] const NodeDesc* desc_of(const Graph& g, const EvalContext& ctx, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr || n->type_name.empty()) return nullptr;
    return ctx.catalog->find(n->type_name);
}

}  // namespace

qp::ports::Value EvalResult::get(NodeId node, PortNumber port) const noexcept {
    for (const auto& o : outputs_) {
        if (o.node == node && o.port == port) return o.value;
    }
    return qp::ports::Value{};
}

Result<EvalStats> evaluate_graph(const Graph& g, const EvalContext& ctx, EvalResult& out) {
    EvalStats stats{};
    if (!ctx.valid()) {
        return Result<EvalStats>{ErrorCode::internal_error};
    }

    const std::size_t n = g.slots().size();

    // 拓扑序（Kahn），平局按槽位索引
    std::vector<std::size_t> indegree(n, 0);
    for (const auto& e : g.edges()) {
        if (e.to.node.index < n) ++indegree[e.to.node.index];
    }

    // 本轮已算出的全部输出，供下游取用
    std::vector<EvalResult::Output> values;

    const auto lookup = [&values](NodeId node, PortNumber port) -> qp::ports::Value {
        for (const auto& o : values) {
            if (o.node == node && o.port == port) return o.value;
        }
        return qp::ports::Value{};
    };

    // 按槽位索引入队：保证同层节点的处理顺序确定
    std::deque<NodeId> ready;
    for (std::size_t i = 1; i < g.slots().size(); ++i) {
        const auto& s = g.slots()[i];
        if (s.occupied && indegree[i] == 0) ready.push_back(s.node.id);
    }

    std::size_t processed = 0;
    while (!ready.empty()) {
        const NodeId id = ready.front();
        ready.pop_front();
        ++processed;
        ++stats.nodes_visited;

        const Node* node = g.find_node(id);
        const NodeDesc* desc = desc_of(g, ctx, id);
        if (node == nullptr || desc == nullptr) {
            return Result<EvalStats>{ErrorCode::unknown_node};
        }

        // ── 收集输入 ────────────────────────────────────────────────────────
        std::vector<std::pair<PortNumber, qp::ports::Value>> inputs;
        for (const auto& in_port : desc->inputs) {
            const Edge* e = g.incoming(PortRef{id, in_port.number, PortDirection::input});
            if (e != nullptr) {
                inputs.emplace_back(in_port.number, lookup(e->from.node, e->from.port));
            } else {
                // 未连线 → 用参数值；参数也没有 → 用默认构造的无效值
                const qp::ports::Value p = node->param(in_port.number);
                if (p.valid()) inputs.emplace_back(in_port.number, p);
            }
        }

        // 输出端口号列表（升序，保证顺序确定）
        std::vector<PortNumber> out_ports;
        out_ports.reserve(desc->outputs.size());
        for (const auto& o : desc->outputs) out_ports.push_back(o.number);
        std::sort(out_ports.begin(), out_ports.end());

        // ── 绕过：把第 1 个输入直接透传到第 1 个输出 ────────────────────────
        if (node->bypassed) {
            ++stats.nodes_skipped;
            if (!out_ports.empty()) {
                qp::ports::Value passthrough{};
                for (const auto& [port, v] : inputs) {
                    (void)port;
                    passthrough = v;
                    break;
                }
                values.push_back(EvalResult::Output{id, out_ports.front(), passthrough});
            }
        } else {
            // ── 缓存键：节点身份（含世代）+ 类型 + 参数 + 输入 ──────────────
            CacheKey key{};
            key.node = id;                    // 含世代：槽位复用不会命中别人的结果
            key.type_name = node->type_name;

            std::string text;
            ValueHash h = kFnvOffsetBasis;
            h = mix_bytes(h, key.type_name.data(), key.type_name.size());
            h = mix_u64(h, static_cast<std::uint64_t>(id.index));
            h = mix_u64(h, static_cast<std::uint64_t>(id.generation));

            // 参数按端口号排序后混入（节点内部存储顺序不参与语义）
            std::vector<std::pair<PortNumber, qp::ports::Value>> params;
            params.reserve(node->params.size());
            for (const auto& p : node->params) {
                params.emplace_back(p.number, p.value);
            }
            std::sort(params.begin(), params.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            h = hash_port_values(h, params);
            for (const auto& [port, v] : params) {
                text += "P";
                text += std::to_string(port);
                text += '=';
                text += canonical_text(v);
                text += ';';
            }

            // 输入同理
            std::vector<std::pair<PortNumber, qp::ports::Value>> sorted_inputs = inputs;
            std::sort(sorted_inputs.begin(), sorted_inputs.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            h = hash_port_values(h, sorted_inputs);
            for (const auto& [port, v] : sorted_inputs) {
                text += "I";
                text += std::to_string(port);
                text += '=';
                text += canonical_text(v);
                text += ';';
            }
            key.content = h;
            key.content_text = std::move(text);

            // ── 查缓存 ──────────────────────────────────────────────────────
            const CacheEntry* hit = ctx.cache == nullptr ? nullptr : ctx.cache->find(key);
            if (hit != nullptr) {
                ++stats.cache_hits;
                for (const auto& [port, v] : hit->outputs) {
                    values.push_back(EvalResult::Output{id, port, v});
                }
            } else {
                ++stats.nodes_computed;
                Result<std::vector<std::pair<PortNumber, qp::ports::Value>>> r =
                    ctx.evaluator->evaluate(id, *desc, sorted_inputs);
                if (!r) return Result<EvalStats>{r.error()};

                auto produced = std::move(r).value();
                if (ctx.cache != nullptr) {
                    ctx.cache->put(std::move(key), produced);
                }
                for (const auto& [port, v] : produced) {
                    values.push_back(EvalResult::Output{id, port, v});
                }
            }
        }

        // ── 推进下游 ────────────────────────────────────────────────────────
        for (const auto& e : g.edges()) {
            if (e.from.node != id) continue;
            if (e.to.node.index >= n) continue;
            if (indegree[e.to.node.index] == 0) continue;
            if (--indegree[e.to.node.index] == 0) {
                ready.push_back(e.to.node);
            }
        }
    }

    if (processed < g.node_count()) {
        // 有环（结构层已保证不会发生，但求值器不假设这一点）
        return Result<EvalStats>{ErrorCode::cycle_detected};
    }

    for (auto& v : values) {
        out.set(v.node, v.port, std::move(v.value));
    }
    return Result<EvalStats>{stats};
}

}  // namespace qp::graph
