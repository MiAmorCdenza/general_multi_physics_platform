/**
 * @file validate.cpp
 * @brief Implementation of load-time validation.
 *
 * All five layers of checking **run to completion before returning**, rather than stopping at
 * the first error: fixing and retrying N times to open one experiment is unacceptable in class.
 */
#include <qp/graph/validate/validate.hpp>

#include <string>

namespace qp::graph {
namespace {

using qp::diag::ErrorCode;

[[nodiscard]] const NodeDesc* desc_of(const Graph& g, const ResolveContext& ctx, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr || n->type_name.empty()) return nullptr;
    return ctx.catalog->find(n->type_name);
}

/// @brief User-facing name for a node: prefer the user name, then the type name.
[[nodiscard]] std::string node_label(const Graph& g, NodeId id) {
    const Node* n = g.find_node(id);
    if (n == nullptr) return "<已删除>";
    if (!n->name.empty()) return n->name;
    if (!n->type_name.empty()) return n->type_name;
    return "<未命名>";
}

/// @brief Turn e into a readable endpoint description.
[[nodiscard]] std::string port_label(const NodeDesc* d, PortNumber port, bool is_output) {
    const PortDesc* p = d == nullptr ? nullptr : d->find_port(port, is_output);
    if (p == nullptr) return "#" + std::to_string(port);
    return p->label.empty() ? p->name : p->label;
}

}  // namespace

Report validate_graph(const Graph& g, const ResolveContext& ctx, const ValidateOptions& options) {
    Report report;
    if (!ctx.valid()) {
        report.error(ErrorCode::internal_error, "校验上下文不完整（缺少节点目录或端口类型表）");
        return report;
    }

    // -- 1. Structure: node types must be registered --------------------------
    for (const auto& s : g.slots()) {
        if (!s.occupied) continue;
        const NodeId id = s.node.id;

        if (s.node.type_name.empty()) {
            report.error(ErrorCode::missing_field, "节点类型名为空（预留槽位尚未补全）", id, 0,
                         false, "先补全节点类型，或删除该节点");
            continue;
        }
        const NodeDesc* d = ctx.catalog->find(s.node.type_name);
        if (d == nullptr) {
            report.error(ErrorCode::unknown_node, "未知的节点类型「" + s.node.type_name + "」",
                         id, 0, false, "该类型可能来自未加载的插件");
            continue;
        }

        // -- 5. Domain permission ---------------------------------------------
        //
        // Domain semantics (the Domain enum) belong to core/graph/domain. This code reads only the
        // flags the caller passes, so the two modules stay independent (domain uses our Report).
        //
        // `domain_allows_field` / `domain_allows_particle` mean "**the current domain** is
        // that domain", and **exactly one of the two is true**. The test is therefore "the
        // current domain is not allowed", **not** "either domain is not allowed" -- the latter
        // would flag normal baked-domain nodes (the early OR form made exactly that mistake).
        if (options.check_domain) {
            const bool violates =
                (options.domain_allows_field && !d->allow_in_field_domain) ||
                (options.domain_allows_particle && !d->allow_in_particle_domain);
            if (violates) {
                report.error(ErrorCode::plugin_capability_missing,
                             "节点类型「" + d->type_name + "」不允许出现在 " +
                                 options.domain_name + " 域",
                             id, 0, false,
                             options.domain_runs_every_frame
                                 ? std::string{"实时域每帧执行，禁止阻塞与分配"}
                                 : std::string{});
            }
        }

        // -- 4. Required params -----------------------------------------------
        if (options.require_required_params) {
            for (const auto& in_port : d->inputs) {
                const bool connected =
                    !in_port.connectable ||
                    g.incoming(PortRef{id, in_port.number, PortDirection::input}) != nullptr;
                if (!in_port.required) continue;
                if (in_port.connectable &&
                    g.incoming(PortRef{id, in_port.number, PortDirection::input}) != nullptr) {
                    continue;
                }
                if (!s.node.param(in_port.number).valid()) {
                    const std::string label =
                        in_port.label.empty() ? in_port.name : in_port.label;
                    report.error(ErrorCode::missing_field,
                                 "必需输入「" + label + "」既未连线也未填值", id, in_port.number,
                                 false, connected ? std::string{} : "连一条线或填一个值");
                }
            }
        }
    }

    // -- 2. Port existence + 3. Connection compatibility ----------------------
    const DimensionMap dims = resolve_dimensions(g, ctx);

    for (const auto& e : g.edges()) {
        const NodeDesc* from_desc = desc_of(g, ctx, e.from.node);
        const NodeDesc* to_desc = desc_of(g, ctx, e.to.node);

        if (from_desc == nullptr || to_desc == nullptr) {
            // Layer 1 above already reported "unknown node type"; do not repeat it here
            continue;
        }

        const PortDesc* from_port = from_desc->find_port(e.from.port, /*is_output=*/true);
        const PortDesc* to_port = to_desc->find_port(e.to.port, /*is_output=*/false);

        if (from_port == nullptr) {
            report.error(ErrorCode::unknown_port,
                         "节点「" + node_label(g, e.from.node) + "」没有输出端口 #" +
                             std::to_string(e.from.port),
                         e.from.node, e.from.port, true);
            continue;
        }
        if (to_port == nullptr) {
            report.error(ErrorCode::unknown_port,
                         "节点「" + node_label(g, e.to.node) + "」没有输入端口 #" +
                             std::to_string(e.to.port),
                         e.to.node, e.to.port, false);
            continue;
        }

        const qp::ports::PortTypeDesc* from_type = ctx.types->find(from_port->type);
        const qp::ports::PortTypeDesc* to_type = ctx.types->find(to_port->type);
        if (from_type == nullptr || to_type == nullptr) {
            report.error(ErrorCode::unknown_port_type,
                         "端口类型未注册：输出「" + port_label(from_desc, e.from.port, true) +
                             "」→ 输入「" + port_label(to_desc, e.to.port, false) + "」",
                         e.to.node, e.to.port, false);
            continue;
        }

        // Resolve `same_as_input` into a concrete dimension before judging
        qp::ports::PortTypeDesc resolved_from = *from_type;
        qp::ports::PortTypeDesc resolved_to = *to_type;
        if (const ResolvedDimension* r = dims.find(e.from.node, e.from.port);
            r != nullptr && r->known) {
            resolved_from.constraint = qp::ports::DimensionConstraint::exact;
            resolved_from.dimension = r->dimension;
        }
        if (resolved_to.constraint == qp::ports::DimensionConstraint::same_as_input) {
            // same_as_input on an input means "follow the upstream", so it matches by construction
            resolved_to.constraint = qp::ports::DimensionConstraint::any;
        }

        const qp::ports::ConnectionCheck chk =
            qp::ports::check_connection(resolved_from, qp::ports::PortDirection::output,
                                        resolved_to, qp::ports::PortDirection::input);
        if (!chk.acceptable()) {
            const std::string edge_name = "连接「" + port_label(from_desc, e.from.port, true) +
                                          "」→「" + port_label(to_desc, e.to.port, false) + "」";
            switch (chk.verdict) {
                case qp::ports::ConnectionVerdict::dimension_mismatch:
                    report.error(ErrorCode::dimension_mismatch, edge_name + " 的量纲不一致",
                                 e.to.node, e.to.port, false,
                                 "检查两端的单位：量纲不同的量不能直接相连");
                    break;
                case qp::ports::ConnectionVerdict::type_mismatch:
                    report.error(ErrorCode::type_mismatch, edge_name + " 的类型不兼容",
                                 e.to.node, e.to.port, false);
                    break;
                case qp::ports::ConnectionVerdict::unknown_type:
                    report.error(ErrorCode::unknown_port_type, edge_name + " 涉及未注册的类型",
                                 e.to.node, e.to.port, false);
                    break;
                case qp::ports::ConnectionVerdict::direction_mismatch:
                    report.error(ErrorCode::invalid_argument, edge_name + " 的方向错误",
                                 e.to.node, e.to.port, false);
                    break;
                default:
                    report.error(ErrorCode::type_mismatch, edge_name + " 不被接受", e.to.node,
                                 e.to.port, false);
                    break;
            }
            continue;
        }

        // An `any` port defeats type checking -- allowed, but it must leave a trace
        if (chk.has_any) {
            report.warn(ErrorCode::type_mismatch,
                        "连接「" + port_label(from_desc, e.from.port, true) + "」→「" +
                            port_label(to_desc, e.to.port, false) +
                            "」经过 any 端口，类型检查已失效",
                        e.to.node, e.to.port, false, "尽量给该端口一个具体类型");
        }
    }

    // -- Extra: unconnected required inputs (warned in the non-required case) --
    if (options.warn_unconnected_outputs) {
        for (const auto& s : g.slots()) {
            if (!s.occupied) continue;
            const NodeDesc* d = desc_of(g, ctx, s.node.id);
            if (d == nullptr) continue;
            for (const auto& out_port : d->outputs) {
                bool connected = false;
                for (const auto& e : g.edges()) {
                    if (e.from.node == s.node.id && e.from.port == out_port.number) {
                        connected = true;
                        break;
                    }
                }
                if (!connected) {
                    const std::string label =
                        out_port.label.empty() ? out_port.name : out_port.label;
                    report.warn(ErrorCode::not_connected,
                                "输出「" + label + "」未连接到任何下游", s.node.id,
                                out_port.number, true);
                }
            }
        }
    }

    return report;
}

Report check_edge(const Graph& g, const ResolveContext& ctx, PortRef from, PortRef to) {
    Report report;
    if (!ctx.valid()) {
        report.error(ErrorCode::internal_error, "校验上下文不完整");
        return report;
    }
    if (!from.valid() || !to.valid()) {
        report.error(ErrorCode::unknown_node, "连接端点无效");
        return report;
    }
    if (from.direction != PortDirection::output || to.direction != PortDirection::input) {
        report.error(ErrorCode::invalid_argument, "连接方向必须是 输出 → 输入");
        return report;
    }

    const NodeDesc* from_desc = desc_of(g, ctx, from.node);
    const NodeDesc* to_desc = desc_of(g, ctx, to.node);
    if (from_desc == nullptr) {
        report.error(ErrorCode::unknown_node, "源节点不存在或类型未注册", from.node);
        return report;
    }
    if (to_desc == nullptr) {
        report.error(ErrorCode::unknown_node, "目标节点不存在或类型未注册", to.node);
        return report;
    }

    const PortDesc* from_port = from_desc->find_port(from.port, true);
    const PortDesc* to_port = to_desc->find_port(to.port, false);
    if (from_port == nullptr) {
        report.error(ErrorCode::unknown_port, "源节点没有该输出端口", from.node, from.port, true);
    }
    if (to_port == nullptr) {
        report.error(ErrorCode::unknown_port, "目标节点没有该输入端口", to.node, to.port, false);
    }
    if (from_port == nullptr || to_port == nullptr) return report;

    const qp::ports::PortTypeDesc* from_type = ctx.types->find(from_port->type);
    const qp::ports::PortTypeDesc* to_type = ctx.types->find(to_port->type);
    if (from_type == nullptr || to_type == nullptr) {
        report.error(ErrorCode::unknown_port_type, "端口类型未注册", to.node, to.port, false);
        return report;
    }

    const DimensionMap dims = resolve_dimensions(g, ctx);
    qp::ports::PortTypeDesc resolved_from = *from_type;
    qp::ports::PortTypeDesc resolved_to = *to_type;
    if (const ResolvedDimension* r = dims.find(from.node, from.port); r != nullptr && r->known) {
        resolved_from.constraint = qp::ports::DimensionConstraint::exact;
        resolved_from.dimension = r->dimension;
    }
    if (resolved_to.constraint == qp::ports::DimensionConstraint::same_as_input) {
        resolved_to.constraint = qp::ports::DimensionConstraint::any;
    }

    const qp::ports::ConnectionCheck chk = qp::ports::check_connection(
        resolved_from, qp::ports::PortDirection::output, resolved_to,
        qp::ports::PortDirection::input);
    if (!chk.acceptable()) {
        if (chk.verdict == qp::ports::ConnectionVerdict::dimension_mismatch) {
            report.error(ErrorCode::dimension_mismatch, "两端量纲不一致", to.node, to.port, false);
        } else {
            report.error(ErrorCode::type_mismatch, "两端类型不兼容", to.node, to.port, false);
        }
        return report;
    }
    if (chk.has_any) {
        report.warn(ErrorCode::type_mismatch, "该连接经过 any 端口，类型检查已失效", to.node,
                    to.port, false);
    }
    return report;
}

}  // namespace qp::graph
