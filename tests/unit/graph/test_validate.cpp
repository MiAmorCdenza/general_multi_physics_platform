/**
 * @file test_validate.cpp
 * @brief Unit and property tests for core/graph/validate.
 *
 * Scenarios are built from a fake node catalog + the real built-in port type table.
 * Focus:
 *   1. **Dimension resolution** of `same_as_input` -- the step that needs the whole graph in view
 *   2. **Reporting every problem at once**, instead of stopping at the first error
 *   3. Validation is **read-only**: the graph and its version are completely unchanged across the call
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/validate.hpp>

#include <deque>
#include <string>

using namespace qp::graph;
using qp::diag::ErrorCode;

namespace {

using qp::ports::DimensionConstraint;
using qp::ports::NumericKind;
using qp::ports::PortTypeId;

/// @brief A simple usable node catalog: owns descriptions and looks them up by type name.
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
    // deque: push_back does not invalidate the address of existing elements (vector does)
    std::deque<NodeDesc> descs_;
};

[[nodiscard]] PortDesc in_port(PortNumber n, const char* name, PortTypeId t, bool required = false) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = t;
    p.required = required;
    return p;
}

[[nodiscard]] PortDesc out_port(PortNumber n, const char* name, PortTypeId t) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = t;
    p.connectable = true;
    return p;
}

/// @brief Build a type table for the tests: adds several scalar types with concrete dimensions.
///
/// A member function rather than a constructor fill: `PortTypeRegistry` is neither copyable nor movable,
/// so `Types` cannot move either, and `Scene` therefore cannot be returned by value.
/// Having the caller construct `Scene` first and then call this function sidesteps the whole move chain.
struct Types {
    qp::ports::PortTypeRegistry reg;

    void install() {
        add(qp::ports::kUserTypeBase + 1, "length_f64", NumericKind::f64,
            DimensionConstraint::exact, qp::units::dims::length);
        add(qp::ports::kUserTypeBase + 2, "time_f64", NumericKind::f64,
            DimensionConstraint::exact, qp::units::dims::time);
        add(qp::ports::kUserTypeBase + 3, "accel_f64", NumericKind::f64,
            DimensionConstraint::exact, qp::units::dims::acceleration);
        add(qp::ports::kUserTypeBase + 4, "same_as_input_f64", NumericKind::f64,
            DimensionConstraint::same_as_input, qp::units::Dim{});
        add(qp::ports::kUserTypeBase + 5, "anydim_f64", NumericKind::f64,
            DimensionConstraint::any, qp::units::Dim{});
        add(qp::ports::kUserTypeBase + 6, "count_i64", NumericKind::i64,
            DimensionConstraint::any, qp::units::Dim{});
    }

    static constexpr PortTypeId length_id = qp::ports::kUserTypeBase + 1;
    static constexpr PortTypeId time_id = qp::ports::kUserTypeBase + 2;
    static constexpr PortTypeId accel_id = qp::ports::kUserTypeBase + 3;
    static constexpr PortTypeId follow_id = qp::ports::kUserTypeBase + 4;
    static constexpr PortTypeId anydim_id = qp::ports::kUserTypeBase + 5;
    static constexpr PortTypeId count_id = qp::ports::kUserTypeBase + 6;

private:
    void add(PortTypeId id, const char* name, NumericKind nk, DimensionConstraint cc,
             qp::units::Dim dim) {
        qp::ports::PortTypeDesc d{};
        d.id = id;
        d.name = name;
        d.numeric = nk;
        d.constraint = cc;
        d.dimension = dim;
        REQUIRE(reg.register_type(d));
    }
};

/// @brief A "source" node: outputs one concrete dimension.
[[nodiscard]] NodeDesc make_source(const char* type, PortTypeId out_type) {
    NodeDesc d{};
    d.type_name = type;
    d.label = type;
    d.outputs.push_back(out_port(1, "value", out_type));
    return d;
}

/// @brief A "unit conversion / follower" node: one input, one same_as_input output.
[[nodiscard]] NodeDesc make_follower(const char* type) {
    NodeDesc d{};
    d.type_name = type;
    d.label = type;
    d.inputs.push_back(in_port(1, "in", Types::anydim_id));
    d.outputs.push_back(out_port(1, "out", Types::follow_id));
    return d;
}

/// @brief An "adder" node: two inputs (any dimension), output follows the input.
[[nodiscard]] NodeDesc make_adder(const char* type) {
    NodeDesc d{};
    d.type_name = type;
    d.label = type;
    d.inputs.push_back(in_port(1, "a", Types::anydim_id));
    d.inputs.push_back(in_port(2, "b", Types::anydim_id));
    d.outputs.push_back(out_port(1, "sum", Types::follow_id));
    return d;
}

/// @brief Test scenario: graph + catalog + type table + context.
struct Scene {
    Graph g;
    FakeCatalog catalog;
    Types types;

    [[nodiscard]] ResolveContext ctx() const noexcept { return ResolveContext{&catalog, &types.reg}; }

    NodeId add(const char* type, const char* name = "") {
        auto r = name[0] == '\0' ? g.add_node(type) : g.add_node_named(type, name);
        REQUIRE(r);
        return r.value();
    }

    void wire(NodeId a, PortNumber ap, NodeId b, PortNumber bp) {
        REQUIRE(g.connect(PortRef{a, ap, PortDirection::output},
                          PortRef{b, bp, PortDirection::input}));
    }

    [[nodiscard]] Report check(const ValidateOptions& o = {}) const {
        return validate_graph(g, ctx(), o);
    }
};

/// @brief The standard scenario: length source / acceleration source / follower / adder / acceleration sink.
///
/// A `void setup(Scene&)` instead of "return Scene by value": the `PortTypeRegistry` inside Scene
/// is neither copyable nor movable, so Scene itself cannot move.
void setup_scene(Scene& s) {
    s.types.install();
    s.catalog.add(make_source("length_src", Types::length_id));
    s.catalog.add(make_source("accel_src", Types::accel_id));
    s.catalog.add(make_follower("passthrough"));
    s.catalog.add(make_adder("add"));
    s.catalog.add([] {
        NodeDesc d{};
        d.type_name = "accel_sink";
        d.inputs.push_back(in_port(1, "a", Types::accel_id));
        return d;
    }());
}

/// @brief Convenience: construct a scenario and fill it in.
#define QP_SCENE(name) \
    Scene name;        \
    setup_scene(name)

}  // namespace

// ===========================================================================
// report.hpp
// ===========================================================================

TEST_CASE("graph.validate.report_ok", "[graph][validate]") {
    Report r;
    REQUIRE(r.ok());
    REQUIRE(r.empty());
    REQUIRE(r.size() == 0);
    REQUIRE(r.worst() == Severity::info);

    r.warn(ErrorCode::not_connected, "只是警告");
    REQUIRE(r.ok());                      // a warning does not block loading
    REQUIRE(r.worst() == Severity::warning);

    r.error(ErrorCode::dimension_mismatch, "这是错误");
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.worst() == Severity::error);
    REQUIRE(r.count(Severity::error) == 1);
    REQUIRE(r.count(Severity::warning) == 1);

    r.clear();
    REQUIRE(r.ok());
}

TEST_CASE("graph.validate.issue_text", "[graph][validate]") {
    Issue i{};
    i.severity = Severity::error;
    i.code = ErrorCode::dimension_mismatch;
    i.message = "量纲不一致";
    REQUIRE(i.to_text() == "[error] 量纲不一致");
    REQUIRE(i.is_error());

    i.node = NodeId{3, 2};
    i.port = 1;
    i.is_output = true;
    i.hint = "检查单位";
    const std::string text = i.to_text();
    REQUIRE(text.find("node#3/2") != std::string::npos);
    REQUIRE(text.find("out1") != std::string::npos);
    REQUIRE(text.find("量纲不一致") != std::string::npos);
    REQUIRE(text.find("检查单位") != std::string::npos);
    // Idempotent
    REQUIRE(i.to_text() == text);

    i.severity = Severity::info;
    REQUIRE_FALSE(i.is_error());
}

TEST_CASE("graph.validate.issue_location", "[graph][validate]") {
    // Locations use stable handles, not pointers: a report gets cached, printed, and sent to other threads
    Report r;
    r.error(ErrorCode::unknown_port, "没有这个端口", NodeId{7, 3}, 2, false, "补一个端口");
    REQUIRE(r.size() == 1);
    const Issue& i = r.issues().front();
    REQUIRE(i.node == NodeId{7, 3});
    REQUIRE(i.port == 2);
    REQUIRE_FALSE(i.is_output);
    REQUIRE(i.hint == "补一个端口");
    REQUIRE(i.code == ErrorCode::unknown_port);
}

TEST_CASE("graph.validate.report_collects_all", "[graph][validate]") {
    // Give every problem at once: stopping at the first error costs the user N fixes to open an experiment
    Report r;
    r.error(ErrorCode::missing_field, "e1");
    r.error(ErrorCode::dimension_mismatch, "e2");
    r.error(ErrorCode::unknown_node, "e3");
    REQUIRE(r.size() == 3);
    REQUIRE(r.count(Severity::error) == 3);

    const std::string text = r.to_text();
    REQUIRE(text.find("e1") != std::string::npos);
    REQUIRE(text.find("e2") != std::string::npos);
    REQUIRE(text.find("e3") != std::string::npos);
}

TEST_CASE("graph.validate.report_worst_severity", "[graph][validate]") {
    Report r;
    REQUIRE(r.worst() == Severity::info);
    r.warn(ErrorCode::not_connected, "w");
    REQUIRE(r.worst() == Severity::warning);
    r.error(ErrorCode::missing_field, "e");
    REQUIRE(r.worst() == Severity::error);
}

// ===========================================================================
// Dimension resolution
// ===========================================================================

TEST_CASE("graph.validate.resolve_dimension_from_port_type", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId len = s.add("length_src");
    const NodeId acc = s.add("accel_src");

    const DimensionMap m = resolve_dimensions(s.g, s.ctx());
    const ResolvedDimension* a = m.find(len, 1);
    const ResolvedDimension* b = m.find(acc, 1);
    REQUIRE(a != nullptr);
    REQUIRE(a->known);
    REQUIRE(a->dimension == qp::units::dims::length);
    REQUIRE(b != nullptr);
    REQUIRE(b->known);
    REQUIRE(b->dimension == qp::units::dims::acceleration);
}

TEST_CASE("graph.validate.resolve_same_as_input_chain", "[graph][validate]") {
    // length source -> follower -> follower -> follower: the dimension at the chain end must still be length.
    // This is the step that "only the whole graph in view can complete".
    QP_SCENE(s);
    const NodeId src = s.add("length_src");
    const NodeId f1 = s.add("passthrough");
    const NodeId f2 = s.add("passthrough");
    const NodeId f3 = s.add("passthrough");
    s.wire(src, 1, f1, 1);
    s.wire(f1, 1, f2, 1);
    s.wire(f2, 1, f3, 1);

    const DimensionMap m = resolve_dimensions(s.g, s.ctx());
    for (NodeId id : {f1, f2, f3}) {
        const ResolvedDimension* r = m.find(id, 1);
        REQUIRE(r != nullptr);
        REQUIRE(r->known);
        REQUIRE(r->dimension == qp::units::dims::length);
    }

    // The chain end feeds an acceleration sink -> a dimension mismatch must be reported
    const NodeId sink = s.add("accel_sink");
    s.wire(f3, 1, sink, 1);
    const Report rep = s.check();
    REQUIRE_FALSE(rep.ok());
    REQUIRE(rep.count(Severity::error) >= 1);
}

TEST_CASE("graph.validate.resolve_unknown_when_input_missing", "[graph][validate]") {
    // A same_as_input output with no connected input -> unknown, but **no error is reported**:
    // whether to report is the validation layer's call (for example a required input produces another error)
    QP_SCENE(s);
    const NodeId f = s.add("passthrough");

    const DimensionMap m = resolve_dimensions(s.g, s.ctx());
    const ResolvedDimension* r = m.find(f, 1);
    REQUIRE(r != nullptr);
    REQUIRE_FALSE(r->known);
}

TEST_CASE("graph.validate.resolve_ignores_unknown_types", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId weird = s.add("not_in_catalog");

    const DimensionMap m = resolve_dimensions(s.g, s.ctx());
    // An unknown type has no resolution result at all (rather than crashing)
    REQUIRE(m.find(weird, 1) == nullptr);
}

TEST_CASE("graph.validate.resolve_is_deterministic", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId src = s.add("length_src");
    const NodeId f1 = s.add("passthrough");
    const NodeId f2 = s.add("passthrough");
    s.wire(src, 1, f1, 1);
    s.wire(f1, 1, f2, 1);

    const DimensionMap a = resolve_dimensions(s.g, s.ctx());
    const DimensionMap b = resolve_dimensions(s.g, s.ctx());
    REQUIRE(a.size() == b.size());
    for (const auto& e : a.all()) {
        const ResolvedDimension* other = b.find(e.node, e.port);
        REQUIRE(other != nullptr);
        REQUIRE(other->known == e.known);
        if (e.known) REQUIRE(other->dimension == e.dimension);
    }
}

TEST_CASE("graph.validate.dimensions_lookup", "[graph][validate]") {
    DimensionMap m;
    REQUIRE(m.find(NodeId{1, 1}, 1) == nullptr);
    REQUIRE(m.size() == 0);

    m.set(NodeId{1, 1}, 1, qp::units::dims::length);
    m.set_unknown(NodeId{2, 1}, 1);
    REQUIRE(m.size() == 2);
    REQUIRE(m.find(NodeId{1, 1}, 1) != nullptr);
    REQUIRE(m.find(NodeId{1, 1}, 1)->known);
    REQUIRE(m.find(NodeId{2, 1}, 1) != nullptr);
    REQUIRE_FALSE(m.find(NodeId{2, 1}, 1)->known);
    // The port number is part of the key too
    REQUIRE(m.find(NodeId{1, 1}, 2) == nullptr);
}

TEST_CASE("graph.validate.dimensions_unknown_for_unset", "[graph][validate]") {
    // An output whose port type is any -> unknown
    QP_SCENE(s);
    s.catalog.add([] {
        NodeDesc d{};
        d.type_name = "anydim_src";
        d.outputs.push_back(out_port(1, "value", Types::anydim_id));
        return d;
    }());
    const NodeId id = s.add("anydim_src");
    const DimensionMap m = resolve_dimensions(s.g, s.ctx());
    const ResolvedDimension* r = m.find(id, 1);
    REQUIRE(r != nullptr);
    REQUIRE_FALSE(r->known);
}

TEST_CASE("graph.validate.resolve_adder_takes_first_connected_input", "[graph][validate]") {
    // Adder node: the output follows the **first connected** input
    QP_SCENE(s);
    const NodeId acc = s.add("accel_src");
    const NodeId add = s.add("add");
    s.wire(acc, 1, add, 1);   // input 1 takes the acceleration source

    const DimensionMap m = resolve_dimensions(s.g, s.ctx());
    const ResolvedDimension* r = m.find(add, 1);
    REQUIRE(r != nullptr);
    REQUIRE(r->known);
    REQUIRE(r->dimension == qp::units::dims::acceleration);
}

// ===========================================================================
// Whole-graph validation
// ===========================================================================

TEST_CASE("graph.validate.ok_on_consistent_graph", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId len = s.add("length_src", "L");
    const NodeId f = s.add("passthrough", "p");
    s.wire(len, 1, f, 1);

    const Report r = s.check();
    INFO(r.to_text());
    REQUIRE(r.ok());
    REQUIRE(r.size() == 0);
}

TEST_CASE("graph.validate.rejects_unknown_node_type", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId id = s.add("does_not_exist", "ghost");

    const Report r = s.check();
    REQUIRE_FALSE(r.ok());
    bool found = false;
    for (const auto& i : r.issues()) {
        if (i.code == ErrorCode::unknown_node && i.node == id) {
            found = true;
            REQUIRE(i.message.find("does_not_exist") != std::string::npos);
        }
    }
    REQUIRE(found);
}

TEST_CASE("graph.validate.rejects_missing_port", "[graph][validate]") {
    // The graph structure layer does not know whether a port number exists; only the validation layer sees NodeDesc
    QP_SCENE(s);
    const NodeId src = s.add("length_src");
    const NodeId f = s.add("passthrough");
    // passthrough has input port 1 only; connecting to port 5 must be rejected
    REQUIRE(s.g.connect(PortRef{src, 1, PortDirection::output},
                        PortRef{f, 5, PortDirection::input}));

    const Report r = s.check();
    REQUIRE_FALSE(r.ok());
    bool found = false;
    for (const auto& i : r.issues()) {
        if (i.code == ErrorCode::unknown_port && i.node == f && i.port == 5) found = true;
    }
    REQUIRE(found);
}

TEST_CASE("graph.validate.rejects_dimension_mismatch_on_edge", "[graph][validate]") {
    // An acceleration source wired straight into a "follower" input is legal (a follower constrains no dimension),
    // but the follower's output wired on to an acceleration sink must agree.
    // A genuine mismatch is built here: length source -> acceleration sink
    QP_SCENE(s);
    const NodeId len = s.add("length_src", "len");
    const NodeId sink = s.add("accel_sink", "sink");
    s.wire(len, 1, sink, 1);

    const Report r = s.check();
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.count(Severity::error) >= 1);
    bool found = false;
    for (const auto& i : r.issues()) {
        if (i.code == ErrorCode::dimension_mismatch) {
            found = true;
            REQUIRE(i.hint.find("单位") != std::string::npos);
        }
    }
    REQUIRE(found);
}

TEST_CASE("graph.validate.rejects_type_mismatch_on_edge", "[graph][validate]") {
    // An integer source into a floating-point input: the numeric kinds differ, so it must be rejected (no silent truncation)
    QP_SCENE(s);
    s.catalog.add([] {
        NodeDesc d{};
        d.type_name = "count_src";
        d.outputs.push_back(out_port(1, "n", Types::count_id));
        return d;
    }());
    const NodeId cnt = s.add("count_src");
    const NodeId f = s.add("passthrough");
    s.wire(cnt, 1, f, 1);

    const Report r = s.check();
    REQUIRE_FALSE(r.ok());
    bool found = false;
    for (const auto& i : r.issues()) {
        if (i.code == ErrorCode::type_mismatch) found = true;
    }
    REQUIRE(found);
}

TEST_CASE("graph.validate.rejects_missing_required_param", "[graph][validate]") {
    QP_SCENE(s);
    s.catalog.add([] {
        NodeDesc d{};
        d.type_name = "needs_mass";
        d.inputs.push_back(in_port(1, "mass", Types::length_id, /*required=*/true));
        d.outputs.push_back(out_port(1, "out", Types::anydim_id));
        return d;
    }());
    const NodeId n = s.add("needs_mass", "m1");

    Report r = s.check();
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.count(Severity::error) >= 1);

    // It passes once a value is filled in
    s.g.find_node_mutable(n)->set_param(1, qp::ports::Value{2.0});
    s.g.bump_version();
    Report r2 = s.check();
    INFO(r2.to_text());
    REQUIRE(r2.ok());

    // A connection satisfies it too
    s.g.find_node_mutable(n)->erase_param(1);
    s.g.bump_version();
    const NodeId len = s.add("length_src");
    s.wire(len, 1, n, 1);
    Report r3 = s.check();
    INFO(r3.to_text());
    REQUIRE(r3.ok());
}

TEST_CASE("graph.validate.rejects_domain_violation", "[graph][validate]") {
    QP_SCENE(s);
    s.catalog.add([] {
        NodeDesc d{};
        d.type_name = "bake_only";
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = false;   // banned in the real-time domain
        d.outputs.push_back(out_port(1, "v", Types::anydim_id));
        return d;
    }());
    const NodeId id = s.add("bake_only");

    // The domain check is now **parameterised**: validate does not depend on the Domain enum
    // from core/graph/domain (that would be a redefinition in a translation unit holding both).
    // `domain_allows_*` means "**is the current domain** that domain".
    ValidateOptions field_opts{};
    field_opts.check_domain = true;
    field_opts.domain_allows_field = true;       // current domain = bake domain
    field_opts.domain_allows_particle = false;
    field_opts.domain_name = "field";
    REQUIRE(s.check(field_opts).ok());   // the node allows baking -> no violation

    ValidateOptions particle_opts{};
    particle_opts.check_domain = true;
    particle_opts.domain_allows_field = false;
    particle_opts.domain_allows_particle = true;  // current domain = real-time domain
    particle_opts.domain_name = "particle";
    particle_opts.domain_runs_every_frame = true;
    const Report r = s.check(particle_opts);
    REQUIRE_FALSE(r.ok());
    bool found = false;
    for (const auto& i : r.issues()) {
        if (i.node == id && i.code == ErrorCode::plugin_capability_missing) {
            found = true;
            REQUIRE(i.message.find("particle") != std::string::npos);
        }
    }
    REQUIRE(found);

    // With the domain check off, no domain is reported
    ValidateOptions no_check{};
    no_check.check_domain = false;
    REQUIRE(s.check(no_check).ok());
}

TEST_CASE("graph.validate.collects_all_issues", "[graph][validate]") {
    // Give every problem at once: all three kinds of error must be reported when they coexist
    QP_SCENE(s);
    const NodeId ghost = s.add("no_such_type", "g");
    const NodeId len = s.add("length_src", "L");
    const NodeId sink = s.add("accel_sink", "S");
    s.wire(len, 1, sink, 1);                                   // dimension mismatch
    REQUIRE(s.g.connect(PortRef{len, 1, PortDirection::output},
                        PortRef{ghost, 1, PortDirection::input}));   // unknown type
    // Add one more with a missing port
    const NodeId f = s.add("passthrough", "P");
    REQUIRE(s.g.connect(PortRef{len, 1, PortDirection::output},
                        PortRef{f, 9, PortDirection::input}));

    const Report r = s.check();
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.size() >= 3);
    // All three kinds of error are present
    bool has_unknown_node = false, has_unknown_port = false, has_dim = false;
    for (const auto& i : r.issues()) {
        if (i.code == ErrorCode::unknown_node) has_unknown_node = true;
        if (i.code == ErrorCode::unknown_port) has_unknown_port = true;
        if (i.code == ErrorCode::dimension_mismatch) has_dim = true;
    }
    REQUIRE(has_unknown_node);
    REQUIRE(has_unknown_port);
    REQUIRE(has_dim);
}

TEST_CASE("graph.validate.is_readonly", "[graph][validate]") {
    // Validation must not modify the graph or its version -- otherwise "open an experiment" would have side effects
    QP_SCENE(s);
    const NodeId len = s.add("length_src");
    const NodeId f = s.add("passthrough");
    s.wire(len, 1, f, 1);
    const NodeId ghost = s.add("no_such_type");

    const GraphVersion v = s.g.version();
    const std::size_t nodes = s.g.node_count();
    const std::size_t edges = s.g.edge_count();

    (void)s.check();
    (void)s.check();
    (void)check_edge(s.g, s.ctx(), PortRef{len, 1, PortDirection::output},
                     PortRef{ghost, 1, PortDirection::input});

    REQUIRE(s.g.version() == v);
    REQUIRE(s.g.node_count() == nodes);
    REQUIRE(s.g.edge_count() == edges);
}

TEST_CASE("graph.validate.warns_on_any_port", "[graph][validate]") {
    // An any port voids type checking -- allowed, but it must leave a trace
    QP_SCENE(s);
    s.catalog.add([] {
        NodeDesc d{};
        d.type_name = "any_passthrough";
        d.inputs.push_back(in_port(1, "in", qp::ports::kAny));
        d.outputs.push_back(out_port(1, "out", qp::ports::kAny));
        return d;
    }());
    const NodeId src = s.add("length_src");
    const NodeId ap = s.add("any_passthrough");
    s.wire(src, 1, ap, 1);

    const Report r = s.check();
    REQUIRE(r.ok());                       // a warning does not block loading
    REQUIRE(r.count(Severity::warning) >= 1);
    bool found = false;
    for (const auto& i : r.issues()) {
        if (i.severity == Severity::warning && i.message.find("any") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("graph.validate.warns_on_unconnected_output_when_enabled", "[graph][validate]") {
    QP_SCENE(s);
    (void)s.add("length_src", "L");

    ValidateOptions quiet{};
    quiet.warn_unconnected_outputs = false;
    REQUIRE(s.check(quiet).ok());
    REQUIRE(s.check(quiet).count(Severity::warning) == 0);

    ValidateOptions loud{};
    loud.warn_unconnected_outputs = true;
    const Report r = s.check(loud);
    REQUIRE(r.ok());                       // still a warning
    REQUIRE(r.count(Severity::warning) >= 1);
}

TEST_CASE("graph.validate.allows_unconnected_optional_input", "[graph][validate]") {
    // Leaving a non-required input unconnected is legal
    QP_SCENE(s);
    (void)s.add("passthrough", "P");
    REQUIRE(s.check().ok());
}

TEST_CASE("graph.validate.deterministic", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId len = s.add("length_src");
    const NodeId sink = s.add("accel_sink");
    s.wire(len, 1, sink, 1);
    (void)s.add("no_such_type");

    const Report a = s.check();
    const Report b = s.check();
    REQUIRE(a.size() == b.size());
    REQUIRE(a.to_text() == b.to_text());
}

TEST_CASE("graph.validate.rejects_dangling_edge_to_deleted_node", "[graph][validate]") {
    // The structure layer deletes the edges along with a node, so normally no dangling edge is left behind.
    // What this verifies: even when an unregistered type appears in the graph, validation neither crashes nor misses it.
    QP_SCENE(s);
    const NodeId len = s.add("length_src");
    const NodeId ghost = s.add("no_such_type");
    s.wire(len, 1, ghost, 1);

    const Report r = s.check();
    REQUIRE_FALSE(r.ok());
    // The upstream type is unknown -> at least one unknown_node; the edge itself no longer floods the report
    REQUIRE(r.count(Severity::error) >= 1);
}

// ===========================================================================
// check_edge (dry run before connecting)
// ===========================================================================

TEST_CASE("graph.validate.check_edge_ok", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId len = s.add("length_src");
    const NodeId f = s.add("passthrough");

    const Report r = check_edge(s.g, s.ctx(), PortRef{len, 1, PortDirection::output},
                                PortRef{f, 1, PortDirection::input});
    REQUIRE(r.ok());
    REQUIRE(r.empty());
}

TEST_CASE("graph.validate.check_edge_rejects_mismatch", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId len = s.add("length_src");
    const NodeId sink = s.add("accel_sink");

    const Report r = check_edge(s.g, s.ctx(), PortRef{len, 1, PortDirection::output},
                                PortRef{sink, 1, PortDirection::input});
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.issues().front().code == ErrorCode::dimension_mismatch);
}

TEST_CASE("graph.validate.check_edge_rejects_unknown_ports", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId src = s.add("length_src");
    const NodeId f = s.add("passthrough");
    const NodeId ghost = s.add("no_such_type");

    // The target port does not exist
    Report r1 = check_edge(s.g, s.ctx(), PortRef{src, 1, PortDirection::output},
                           PortRef{f, 9, PortDirection::input});
    REQUIRE_FALSE(r1.ok());
    REQUIRE(r1.issues().front().code == ErrorCode::unknown_port);

    // The source node type is unregistered
    Report r2 = check_edge(s.g, s.ctx(), PortRef{ghost, 1, PortDirection::output},
                           PortRef{f, 1, PortDirection::input});
    REQUIRE_FALSE(r2.ok());
    REQUIRE(r2.issues().front().code == ErrorCode::unknown_node);

    // Wrong direction
    Report r3 = check_edge(s.g, s.ctx(), PortRef{f, 1, PortDirection::input},
                           PortRef{src, 1, PortDirection::output});
    REQUIRE_FALSE(r3.ok());
    REQUIRE(r3.issues().front().code == ErrorCode::invalid_argument);

    // Invalid handle
    Report r4 = check_edge(s.g, s.ctx(), PortRef{}, PortRef{f, 1, PortDirection::input});
    REQUIRE_FALSE(r4.ok());
}

TEST_CASE("graph.validate.check_edge_does_not_modify_graph", "[graph][validate]") {
    QP_SCENE(s);
    const NodeId len = s.add("length_src");
    const NodeId f = s.add("passthrough");
    const GraphVersion v = s.g.version();
    const std::size_t edges = s.g.edge_count();

    (void)check_edge(s.g, s.ctx(), PortRef{len, 1, PortDirection::output},
                     PortRef{f, 1, PortDirection::input});
    REQUIRE(s.g.version() == v);
    REQUIRE(s.g.edge_count() == edges);
}
