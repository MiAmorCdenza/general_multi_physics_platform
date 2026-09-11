/**
 * @file evaluator.cpp
 * @brief Evaluation: Kahn topological order + content-addressed cache.
 *
 * One ordering detail needs care: **all of a node's inputs must be ready before the
 * node is evaluated**. The topological order ensures this, breaking ties inside one
 * layer by **slot index** -- part of "deterministic": without a fixed tie-break two
 * runs could call in a different order, and hidden state would then give different results.
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

    // Topological order (Kahn), ties broken by slot index
    std::vector<std::size_t> indegree(n, 0);
    for (const auto& e : g.edges()) {
        if (e.to.node.index < n) ++indegree[e.to.node.index];
    }

    // Everything computed this round, for downstream nodes to read
    std::vector<EvalResult::Output> values;

    const auto lookup = [&values](NodeId node, PortNumber port) -> qp::ports::Value {
        for (const auto& o : values) {
            if (o.node == node && o.port == port) return o.value;
        }
        return qp::ports::Value{};
    };

    // Enqueue by slot index: makes the processing order within a layer deterministic
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

        // -- Collect inputs --------------------------------------------------
        std::vector<std::pair<PortNumber, qp::ports::Value>> inputs;
        for (const auto& in_port : desc->inputs) {
            const Edge* e = g.incoming(PortRef{id, in_port.number, PortDirection::input});
            if (e != nullptr) {
                inputs.emplace_back(in_port.number, lookup(e->from.node, e->from.port));
            } else {
                // Unconnected -> use the parameter value; no parameter either -> invalid default
                const qp::ports::Value p = node->param(in_port.number);
                if (p.valid()) inputs.emplace_back(in_port.number, p);
            }
        }

        // Output port numbers (ascending, so the order is deterministic)
        std::vector<PortNumber> out_ports;
        out_ports.reserve(desc->outputs.size());
        for (const auto& o : desc->outputs) out_ports.push_back(o.number);
        std::sort(out_ports.begin(), out_ports.end());

        // -- Bypass: pass the first input straight to the first output -------
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
            // -- Cache key: node identity (with generation) + type + params + inputs ---
            CacheKey key{};
            key.node = id;                    // With generation: slot reuse cannot hit another node's result
            key.type_name = node->type_name;

            std::string text;
            ValueHash h = kFnvOffsetBasis;
            h = mix_bytes(h, key.type_name.data(), key.type_name.size());
            h = mix_u64(h, static_cast<std::uint64_t>(id.index));
            h = mix_u64(h, static_cast<std::uint64_t>(id.generation));

            // Parameters are mixed in sorted by port number (internal storage order is not semantic)
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

            // Inputs likewise
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

            // -- Look up the cache -------------------------------------------
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

        // -- Advance downstream ----------------------------------------------
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
        // A cycle: the structure layer forbids it, but the evaluator does not assume that
        return Result<EvalStats>{ErrorCode::cycle_detected};
    }

    for (auto& v : values) {
        out.set(v.node, v.port, std::move(v.value));
    }
    return Result<EvalStats>{stats};
}

}  // namespace qp::graph
