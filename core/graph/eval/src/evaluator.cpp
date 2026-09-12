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

#include <qp/ports/check.hpp>

#include <algorithm>
#include <deque>
#include <string>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

[[nodiscard]] const NodeDesc* desc_of(const Graph& g, const EvalContext& ctx, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr || n->type_name.empty()) return nullptr;
    return ctx.catalog->find(n->type_name);
}

/// @brief A sentence for the first way `produced` is outside what `desc` declares, or empty when it fits.
///
/// A sentence rather than a code, because the FaultLog records `what` and a report needs to say *which*
/// port and *which* kind: "the plugin is broken" sends the reader to the plugin, while "port 7 was not
/// declared" sends them to the line of its manifest that is wrong.
[[nodiscard]] std::string validate_outputs(
    const NodeDesc& desc, const EvalContext& ctx,
    const std::vector<std::pair<PortNumber, qp::ports::Value>>& produced) {
    for (std::size_t i = 0; i < produced.size(); ++i) {
        const PortNumber port = produced[i].first;

        // Declared at all?
        const PortDesc* declared = nullptr;
        for (const PortDesc& candidate : desc.outputs) {
            if (candidate.number == port) {
                declared = &candidate;
                break;
            }
        }
        if (declared == nullptr) {
            return "returned a value for output port " + std::to_string(port) +
                   ", which it does not declare";
        }

        // Declared once?
        for (std::size_t j = i + 1; j < produced.size(); ++j) {
            if (produced[j].first == port) {
                return "returned output port " + std::to_string(port) + " twice";
            }
        }

        // The right kind for that port?
        //
        // An **invalid** value is exempt, and the exemption is load-bearing rather than a convenience: in this
        // platform `ValueKind::invalid` means "not computed", the same way `UncertaintyKind::unknown` means
        // "not quantified". A node with an unset parameter legitimately produces no value, and the whole
        // downstream branch then produces none either -- `graph.eval.missing_param_fails` has asserted that
        // since before this check existed. A missing *required* parameter is a **graph** problem, and
        // `graph/validate` is where it is reported (that is what `PortDesc::required` is for); turning it into
        // a plugin fault here would blame the plugin for the user's half-filled form.
        if (!produced[i].second.valid()) continue;

        const qp::ports::PortTypeDesc* type = ctx.types->find(declared->type);
        if (type == nullptr) {
            return "declared output port " + std::to_string(port) + " with a port type that is not in the "
                   "registry";
        }
        if (const auto verdict = qp::ports::check_value(*type, produced[i].second); !verdict.has_value()) {
            return "returned a value for output port " + std::to_string(port) + " that is not a " +
                   std::string{type->name} + " (error " +
                   std::string{qp::diag::to_string(verdict.error())} + ")";
        }
    }
    return {};
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
                // The plugin boundary. A plugin that raises is caught here and reported as a fault, so the
                // host survives -- charter C4's schema domain. The label is the **node type**, because that
                // is what the user is looking at and what the report can name; the plugin's own identity
                // would be less useful in a diagnostic than the type it was asked to compute.
                //
                // Note what this does not claim: a plugin that segfaults still takes the process down. That
                // boundary is stated in `qp/plugin/guard.hpp` rather than implied away.
                Result<std::vector<std::pair<PortNumber, qp::ports::Value>>> r =
                    qp::plugin::call_guarded(desc->type_name, ctx.faults,
                                            [&] { return ctx.evaluator->evaluate(id, *desc, sorted_inputs); });
                if (!r) return Result<EvalStats>{r.error()};

                auto produced = std::move(r).value();

                // The **schema domain**: the plugin's answer is checked against what the plugin itself
                // declared, before the value reaches the cache or a downstream node. `ports::check_value`'s
                // contract has always said "it runs after every evaluation" -- and nothing called it, which
                // made "a plugin returned a value of the wrong type" a corruption path rather than a caught
                // fault. A wrong-kind value entering a content-addressed cache does not mislead once: every
                // later run that hits that entry gets the same wrong value, and the statistics report a cache
                // **hit**, which is the opposite of a warning.
                if (std::string bad = validate_outputs(*desc, ctx, produced); !bad.empty()) {
                    if (ctx.faults != nullptr) {
                        (void)ctx.faults->record(std::string{desc->type_name},
                                                 ErrorCode::plugin_fault, std::move(bad));
                    }
                    return Result<EvalStats>{ErrorCode::plugin_fault};
                }

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
