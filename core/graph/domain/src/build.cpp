/**
 * @file build.cpp
 * @brief Implementation of execution-plan construction and domain validation.
 */
#include <qp/graph/domain/build.hpp>

#include <algorithm>
#include <deque>
#include <string>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

[[nodiscard]] const NodeDesc* desc_of(const Graph& g, const PlanContext& ctx, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr || n->type_name.empty()) return nullptr;
    return ctx.catalog->find(n->type_name);
}

/// @brief User-facing label for a node.
[[nodiscard]] std::string node_label(const Graph& g, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr) return "<已删除>";
    if (!n->name.empty()) return n->name;
    if (!n->type_name.empty()) return n->type_name;
    return "<未命名>";
}

}  // namespace

bool allowed_in_domain(const NodeDesc& desc, Domain d) noexcept {
    switch (d) {
        case Domain::field:
            return desc.allow_in_field_domain;
        case Domain::particle:
            return desc.allow_in_particle_domain;
        case Domain::render:
            // The render domain does not evaluate, so it has no real-time constraint; any type may be declared a render item.
            return true;
    }
    return false;
}

ExecutionPlan build_plan(const Graph& g, const PlanContext& ctx,
                         const Declarations& declared) noexcept {
    ExecutionPlan plan;
    plan.field = DomainPlan{Domain::field};
    plan.particle = DomainPlan{Domain::particle};
    plan.render = DomainPlan{Domain::render};

    if (!ctx.valid() || declared.empty()) return plan;

    // -- 1. Collect reachable nodes **backwards** from the declared outputs --
    std::vector<NodeId> stack;
    std::vector<NodeId> reachable;
    for (const auto& d : declared.all()) {
        if (!d.valid()) continue;
        if (g.find_node(d.node) == nullptr) continue;   // a stale declaration is reported by check_declarations
        if (std::find(reachable.begin(), reachable.end(), d.node) == reachable.end() &&
            std::find(stack.begin(), stack.end(), d.node) == stack.end()) {
            stack.push_back(d.node);
        }
    }
    while (!stack.empty()) {
        const NodeId cur = stack.back();
        stack.pop_back();
        if (std::find(reachable.begin(), reachable.end(), cur) != reachable.end()) continue;
        reachable.push_back(cur);

        // Backwards: who feeds a value to me
        for (const auto& e : g.edges()) {
            if (e.to.node != cur) continue;
            const NodeId up = e.from.node;
            if (g.find_node(up) == nullptr) continue;
            if (std::find(reachable.begin(), reachable.end(), up) != reachable.end()) continue;
            stack.push_back(up);
        }
    }

    if (reachable.empty()) return plan;

    // -- 2. Kahn topological sort over the reachable subset, ties broken by slot index --
    //
    // Only count in-edges **inside the subset**: an edge pointing outside cannot exist
    // (the subset is the backwards-reachable closure, so any node feeding it is in it).
    const std::size_t n = g.slots().size();
    std::vector<std::size_t> indegree(n, 0);
    for (const auto& e : g.edges()) {
        if (e.to.node.index >= n) continue;
        const bool to_in = std::find(reachable.begin(), reachable.end(), e.to.node) !=
                           reachable.end();
        if (!to_in) continue;
        ++indegree[e.to.node.index];
    }

    // Prepare a ready queue ordered by slot index (min-heap semantics: take the smallest index each round)
    std::vector<NodeId> ready;
    for (const NodeId id : reachable) {
        if (id.index < n && indegree[id.index] == 0) ready.push_back(id);
    }
    std::sort(ready.begin(), ready.end(),
              [](NodeId a, NodeId b) { return a.index < b.index; });

    std::vector<NodeId> order;
    order.reserve(reachable.size());
    while (!ready.empty()) {
        const NodeId cur = ready.front();
        ready.erase(ready.begin());
        order.push_back(cur);

        for (const auto& e : g.edges()) {
            if (e.from.node != cur) continue;
            if (e.to.node.index >= n) continue;
            if (std::find(reachable.begin(), reachable.end(), e.to.node) == reachable.end()) {
                continue;
            }
            if (indegree[e.to.node.index] == 0) continue;
            if (--indegree[e.to.node.index] == 0) {
                // Keep the queue index-ordered on insert so the tie-break rule holds
                const auto pos = std::lower_bound(
                    ready.begin(), ready.end(), e.to.node,
                    [](NodeId a, NodeId b) { return a.index < b.index; });
                ready.insert(pos, e.to.node);
            }
        }
    }

    // -- 3. Bucket by domain ------------------------------------------------
    for (const NodeId id : order) {
        const NodeDesc* desc = desc_of(g, ctx, id);
        if (desc == nullptr) continue;   // an unknown type is reported by the validation layer

        if (allowed_in_domain(*desc, Domain::field) && desc->has_compute) {
            plan.field.add_node(id);
        }
        if (allowed_in_domain(*desc, Domain::particle) && desc->has_compute) {
            plan.particle.add_node(id);
        }
    }

    // The order must be filtered by domain too (preserving the original relative order)
    const auto filter_order = [&order](const DomainPlan& p) {
        std::vector<NodeId> out;
        for (const NodeId id : order) {
            if (std::find(p.nodes().begin(), p.nodes().end(), id) != p.nodes().end()) {
                out.push_back(id);
            }
        }
        return out;
    };
    plan.field.set_order(filter_order(plan.field));
    plan.particle.set_order(filter_order(plan.particle));

    // The render domain's "plan" is the declaration itself (it does not evaluate)
    for (const auto& d : declared.all()) {
        if (!d.valid()) continue;
        const NodeDesc* desc = desc_of(g, ctx, d.node);
        if (desc == nullptr) continue;
        if (!desc->has_compute) {
            plan.render.add_declared(d);
        }
    }

    return plan;
}

Report check_domains(const Graph& g, const PlanContext& ctx, Domain d) {
    Report report;
    if (!ctx.valid()) {
        report.error(ErrorCode::internal_error, "域校验上下文不完整（缺少节点目录）");
        return report;
    }
    for (const auto& s : g.slots()) {
        if (!s.occupied) continue;
        const NodeDesc* desc = desc_of(g, ctx, s.node.id);
        if (desc == nullptr) continue;   // an unknown type is reported by generic validation; do not spam it again

        if (!allowed_in_domain(*desc, d)) {
            report.error(ErrorCode::plugin_capability_missing,
                         "节点「" + node_label(g, s.node.id) + "」（类型 " + desc->type_name +
                             "）不允许出现在 " + to_string(d) + " 域",
                         s.node.id, 0, false,
                         runs_every_frame(d)
                             ? std::string{"the particle domain runs every frame: no blocking and no allocation"}
                             : std::string{});
        }
    }
    return report;
}

Report check_declarations(const Graph& g, const PlanContext& ctx,
                          const Declarations& declared) {
    Report report;
    if (!ctx.valid()) {
        report.error(ErrorCode::internal_error, "声明校验上下文不完整（缺少节点目录）");
        return report;
    }
    for (const auto& d : declared.all()) {
        if (!d.valid()) {
            report.error(ErrorCode::invalid_argument, "声明输出的句柄或端口号无效");
            continue;
        }
        const Node* node = g.find_node(d.node);
        if (node == nullptr) {
            report.error(ErrorCode::unknown_node,
                         "声明输出指向已不存在的节点 #" + std::to_string(d.node.index), d.node,
                         d.port, true, "this declaration is stale; delete it");
            continue;
        }
        const NodeDesc* desc = ctx.catalog->find(node->type_name);
        if (desc == nullptr) {
            report.error(ErrorCode::unknown_node,
                         "声明输出所在节点的类型「" + node->type_name + "」未注册", d.node,
                         d.port, true);
            continue;
        }
        if (desc->find_port(d.port, /*is_output=*/true) == nullptr) {
            report.error(ErrorCode::unknown_port,
                         "节点「" + node_label(g, d.node) + "」没有输出端口 #" +
                             std::to_string(d.port) + "（声明已失效）",
                         d.node, d.port, true, "this declaration is stale; delete it or point it at an existing port");
        }
    }
    return report;
}

}  // namespace qp::graph
