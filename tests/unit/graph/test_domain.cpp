/**
 * @file test_domain.cpp
 * @brief Unit and property tests for core/graph/domain.
 *
 * Three focus areas:
 *   1. **Domain capabilities**: the real-time domain forbids blocking, allocation, exceptions, virtual calls
 *   2. **Declared outputs**: a third kind of graph element, neither node nor edge
 *   3. **Pull-based pruning**: the plan covers only the subgraph reaching declared outputs, in fixed order
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/graph/domain.hpp>

#include <deque>
#include <string>

using namespace qp::graph;

namespace {

/// @brief A simple, serviceable node catalog.
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
    std::deque<NodeDesc> descs_;
};

[[nodiscard]] PortDesc in_port(PortNumber n, const char* name) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = qp::ports::kScalarF64;
    return p;
}
[[nodiscard]] PortDesc out_port(PortNumber n, const char* name) {
    PortDesc p{};
    p.number = n;
    p.name = name;
    p.type = qp::ports::kScalarF64;
    return p;
}

/// @brief An ordinary evaluable node (one input, one output).
[[nodiscard]] NodeDesc make_processor(const char* type, bool field, bool particle) {
    NodeDesc d{};
    d.type_name = type;
    d.has_compute = true;
    d.allow_in_field_domain = field;
    d.allow_in_particle_domain = particle;
    d.inputs.push_back(in_port(1, "in"));
    d.outputs.push_back(out_port(1, "out"));
    return d;
}

/// @brief A purely declarative node (no compute): a render item.
[[nodiscard]] NodeDesc make_render_item(const char* type) {
    NodeDesc d{};
    d.type_name = type;
    d.has_compute = false;          // a render item is never evaluated
    d.allow_in_field_domain = true;
    d.allow_in_particle_domain = true;
    d.inputs.push_back(in_port(1, "data"));
    return d;
}

struct Scene {
    Graph g;
    FakeCatalog catalog;
    Declarations declared;

    void setup() {
        catalog.add(make_processor("source", true, true));      // allowed in both domains
        catalog.add(make_processor("baker", true, false));      // bake only
        catalog.add(make_processor("fast", false, true));       // real-time only
        catalog.add(make_render_item("draw_field"));            // a render item
    }

    [[nodiscard]] PlanContext ctx() const noexcept { return PlanContext{&catalog}; }

    NodeId add_node(const char* type, const char* name = "") {
        auto r = name[0] == '\0' ? g.add_node(type) : g.add_node_named(type, name);
        REQUIRE(r);
        return r.value();
    }

    void wire(NodeId a, PortNumber ap, NodeId b, PortNumber bp) {
        REQUIRE(g.connect(PortRef{a, ap, PortDirection::output},
                          PortRef{b, bp, PortDirection::input}));
    }

    [[nodiscard]] ExecutionPlan plan() const noexcept {
        return build_plan(g, ctx(), declared);
    }
};

#define QP_DOMAIN_SCENE(name) \
    Scene name;               \
    name.setup()

/// @brief Finds the domain a node belongs to inside the plan. Returns nullptr when absent.
[[nodiscard]] const DomainPlan* find_plan_for(const ExecutionPlan& p, NodeId id) {
    for (Domain d : {Domain::field, Domain::particle}) {
        const DomainPlan& dp = p.of(d);
        for (NodeId n : dp.nodes()) {
            if (n == id) return &dp;
        }
    }
    return nullptr;
}

}  // namespace

// ===========================================================================
// domain.hpp
// ===========================================================================

TEST_CASE("graph.domain.predicates", "[graph][domain]") {
    // The enumerator values of the three domains are frozen
    STATIC_REQUIRE(static_cast<int>(Domain::field) == 0);
    STATIC_REQUIRE(static_cast<int>(Domain::particle) == 1);
    STATIC_REQUIRE(static_cast<int>(Domain::render) == 2);
    STATIC_REQUIRE(sizeof(Domain) == 1);

    REQUIRE(std::string(to_string(Domain::field)) == "field");
    REQUIRE(std::string(to_string(Domain::particle)) == "particle");
    REQUIRE(std::string(to_string(Domain::render)) == "render");

    // Only the real-time domain runs every frame
    REQUIRE(runs_every_frame(Domain::particle));
    REQUIRE_FALSE(runs_every_frame(Domain::field));
    REQUIRE_FALSE(runs_every_frame(Domain::render));

    // The render domain takes no part in evaluation
    REQUIRE(participates_in_evaluation(Domain::field));
    REQUIRE(participates_in_evaluation(Domain::particle));
    REQUIRE_FALSE(participates_in_evaluation(Domain::render));
}

TEST_CASE("graph.domain.capabilities", "[graph][domain]") {
    // The bake domain: may block, allocate, throw, call foreign code, and use virtual calls
    const DomainCapabilities f = capabilities_of(Domain::field);
    REQUIRE(f.may_block);
    REQUIRE(f.may_allocate);
    REQUIRE(f.may_throw);
    REQUIRE(f.may_call_foreign);
    REQUIRE(f.may_use_virtual);

    // The real-time domain: all forbidden. Their shared consequence is "unpredictable frame time".
    const DomainCapabilities p = capabilities_of(Domain::particle);
    REQUIRE_FALSE(p.may_block);
    REQUIRE_FALSE(p.may_allocate);
    REQUIRE_FALSE(p.may_throw);
    REQUIRE_FALSE(p.may_call_foreign);
    REQUIRE_FALSE(p.may_use_virtual);

    // Idempotence
    REQUIRE(capabilities_of(Domain::particle).may_allocate == p.may_allocate);
}

// ===========================================================================
// declaration.hpp
// ===========================================================================

TEST_CASE("graph.domain.declaration_add", "[graph][domain]") {
    Declarations d;
    REQUIRE(d.empty());
    REQUIRE(d.size() == 0);

    const DeclaredOutput o{NodeId{1, 1}, 1};
    REQUIRE(o.valid());
    REQUIRE(d.add(o));
    REQUIRE(d.size() == 1);
    REQUIRE(d.contains(o));
    REQUIRE(d.all().front() == o);

    // Invalid declarations are refused
    REQUIRE_FALSE(d.add(DeclaredOutput{}));
    REQUIRE_FALSE(d.add(DeclaredOutput{NodeId{1, 1}, 0}));
    REQUIRE(d.size() == 1);
}

TEST_CASE("graph.domain.declaration_dedup", "[graph][domain]") {
    Declarations d;
    const DeclaredOutput o{NodeId{1, 1}, 1};
    REQUIRE(d.add(o));
    REQUIRE_FALSE(d.add(o));      // adding a duplicate is idempotent
    REQUIRE(d.size() == 1);

    // The same node on a different port is a different declaration
    REQUIRE(d.add(DeclaredOutput{NodeId{1, 1}, 2}));
    REQUIRE(d.size() == 2);
}

TEST_CASE("graph.domain.declaration_remove", "[graph][domain]") {
    Declarations d;
    const DeclaredOutput a{NodeId{1, 1}, 1};
    const DeclaredOutput b{NodeId{1, 1}, 2};
    const DeclaredOutput c{NodeId{2, 1}, 1};
    REQUIRE(d.add(a));
    REQUIRE(d.add(b));
    REQUIRE(d.add(c));

    REQUIRE(d.remove(a));
    REQUIRE_FALSE(d.remove(a));    // already removed
    REQUIRE(d.size() == 2);

    // Bulk removal by node (called when the node itself is deleted)
    REQUIRE(d.remove_node(NodeId{1, 1}) == 1);   // only b is left
    REQUIRE(d.size() == 1);
    REQUIRE(d.contains(c));
    REQUIRE(d.remove_node(NodeId{9, 9}) == 0);   // a node that does not exist
}

TEST_CASE("graph.domain.declaration_lookup", "[graph][domain]") {
    Declarations d;
    REQUIRE_FALSE(d.contains(DeclaredOutput{NodeId{1, 1}, 1}));
    REQUIRE(d.add(DeclaredOutput{NodeId{1, 1}, 1}));
    REQUIRE(d.contains(DeclaredOutput{NodeId{1, 1}, 1}));
    REQUIRE_FALSE(d.contains(DeclaredOutput{NodeId{1, 1}, 2}));
    REQUIRE_FALSE(d.contains(DeclaredOutput{NodeId{2, 1}, 1}));
    REQUIRE_FALSE(d.contains(DeclaredOutput{}));

    d.clear();
    REQUIRE(d.empty());
}

// ===========================================================================
// execution plan
// ===========================================================================

TEST_CASE("graph.domain.plan_empty_when_no_declaration", "[graph][domain]") {
    QP_DOMAIN_SCENE(s);
    const NodeId a = s.add_node("source", "a");

    // No declaration at all -> all three plans are empty. That is the pruning: undeclared, unevaluated.
    const ExecutionPlan p = s.plan();
    REQUIRE(p.field.empty());
    REQUIRE(p.particle.empty());
    REQUIRE(p.render.empty());
    REQUIRE(p.field.size() == 0);

    // Declaring it changes nothing while the node is gone (a stale declaration)
    s.declared.add(DeclaredOutput{a, 1});
    REQUIRE(p.field.size() == 0);   // the old plan is still the one in hand
    REQUIRE(s.plan().field.size() == 1);
}

TEST_CASE("graph.domain.plan_covers_only_reachable", "[graph][domain]") {
    // a -> b -> c, plus an isolated d.
    // Declaring only the output of c -> the plan must hold a, b, c and **not d**.
    QP_DOMAIN_SCENE(s);
    const NodeId a = s.add_node("source", "a");
    const NodeId b = s.add_node("source", "b");
    const NodeId c = s.add_node("source", "c");
    const NodeId isolated = s.add_node("source", "isolated");
    s.wire(a, 1, b, 1);
    s.wire(b, 1, c, 1);

    s.declared.add(DeclaredOutput{c, 1});
    const ExecutionPlan p = s.plan();

    REQUIRE(p.field.size() == 3);
    REQUIRE(find_plan_for(p, a) != nullptr);
    REQUIRE(find_plan_for(p, b) != nullptr);
    REQUIRE(find_plan_for(p, c) != nullptr);
    REQUIRE(find_plan_for(p, isolated) == nullptr);   // pruned away

    // Declaring two outputs -> their union
    s.declared.add(DeclaredOutput{isolated, 1});
    REQUIRE(s.plan().field.size() == 4);
}

TEST_CASE("graph.domain.plan_order_is_topological", "[graph][domain]") {
    // A chain a -> b -> c: the order must be a, b, c
    QP_DOMAIN_SCENE(s);
    const NodeId a = s.add_node("source", "a");
    const NodeId b = s.add_node("source", "b");
    const NodeId c = s.add_node("source", "c");
    s.wire(a, 1, b, 1);
    s.wire(b, 1, c, 1);
    s.declared.add(DeclaredOutput{c, 1});

    // ExecutionPlan must be stored in a named object: `const auto& o = s.plan().field.order();`
    // binds the reference to a temporary **destroyed at the end of the full expression** (dangling).
    // That UB "happens to work" on GCC and hands back an empty vector on MSVC.
    const ExecutionPlan plan = s.plan();
    const auto& order = plan.field.order();
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == a);
    REQUIRE(order[1] == b);
    REQUIRE(order[2] == c);

    // The order must really be topological: every edge's source precedes its target
    for (const auto& e : s.g.edges()) {
        const auto pos_from = std::find(order.begin(), order.end(), e.from.node);
        const auto pos_to = std::find(order.begin(), order.end(), e.to.node);
        if (pos_from == order.end() || pos_to == order.end()) continue;
        REQUIRE(pos_from < pos_to);
    }
}

TEST_CASE("graph.domain.plan_order_is_deterministic", "[graph][domain]") {
    // When one layer holds several nodes, the slot index breaks the tie.
    // Without fixing that, two runs could visit them in different orders, and any
    // implementation carrying hidden state would then produce different results.
    //
    // Note: **each input port takes at most one incoming edge** (a hard invariant of structure).
    // A join therefore needs two input ports; several wires into one port cannot build the graph.
    QP_DOMAIN_SCENE(s);
    s.catalog.add([] {
        NodeDesc d{};
        d.type_name = "join2";
        d.has_compute = true;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = true;
        d.inputs.push_back(in_port(1, "a"));
        d.inputs.push_back(in_port(2, "b"));
        d.outputs.push_back(out_port(1, "out"));
        return d;
    }());

    const NodeId src = s.add_node("source", "src");
    const NodeId b1 = s.add_node("source", "b1");
    const NodeId b2 = s.add_node("source", "b2");
    const NodeId b3 = s.add_node("source", "b3");
    const NodeId mid = s.add_node("join2", "mid");
    const NodeId sink = s.add_node("join2", "sink");

    s.wire(src, 1, b1, 1);
    s.wire(src, 1, b2, 1);
    s.wire(src, 1, b3, 1);
    // b1 + b2 -> mid and b3 + mid -> sink (a diamond plus one direct wire)
    s.wire(b1, 1, mid, 1);
    s.wire(b2, 1, mid, 2);
    s.wire(b3, 1, sink, 1);
    s.wire(mid, 1, sink, 2);
    s.declared.add(DeclaredOutput{sink, 1});

    const ExecutionPlan plan1 = s.plan();
    const ExecutionPlan plan2 = s.plan();
    const auto& o1 = plan1.field.order();
    const auto& o2 = plan2.field.order();
    REQUIRE(o1 == o2);
    REQUIRE(o1.size() == 6);
    // src must precede the three branches
    REQUIRE(o1.front() == src);
    // sink must come last
    REQUIRE(o1.back() == sink);
    // The three branches are ordered by **slot index** (b1 < b2 < b3)
    const auto pos_b1 = std::find(o1.begin(), o1.end(), b1) - o1.begin();
    const auto pos_b2 = std::find(o1.begin(), o1.end(), b2) - o1.begin();
    const auto pos_b3 = std::find(o1.begin(), o1.end(), b3) - o1.begin();
    REQUIRE(pos_b1 < pos_b2);
    REQUIRE(pos_b2 < pos_b3);

    // And the order satisfies every topological constraint
    for (const auto& e : s.g.edges()) {
        const auto pf = std::find(o1.begin(), o1.end(), e.from.node);
        const auto pt = std::find(o1.begin(), o1.end(), e.to.node);
        if (pf == o1.end() || pt == o1.end()) continue;
        INFO("edge " << e.from.node.index << " -> " << e.to.node.index);
        REQUIRE(pf < pt);
    }
}

TEST_CASE("graph.domain.plan_splits_by_domain", "[graph][domain]") {
    // baker is bake-only, fast is real-time-only, source is allowed in both domains.
    QP_DOMAIN_SCENE(s);
    const NodeId src = s.add_node("source", "src");
    const NodeId baker = s.add_node("baker", "baker");
    const NodeId fast = s.add_node("fast", "fast");
    s.wire(src, 1, baker, 1);
    s.wire(src, 1, fast, 1);

    // Both outputs declared -> source appears in both domains
    s.declared.add(DeclaredOutput{baker, 1});
    s.declared.add(DeclaredOutput{fast, 1});
    const ExecutionPlan p = s.plan();

    REQUIRE(find_plan_for(p, baker) != nullptr);
    REQUIRE(find_plan_for(p, baker)->domain() == Domain::field);
    REQUIRE(find_plan_for(p, fast) != nullptr);
    REQUIRE(find_plan_for(p, fast)->domain() == Domain::particle);
    // source is allowed in both domains -> it appears in both plans
    REQUIRE(std::find(p.field.nodes().begin(), p.field.nodes().end(), src) !=
            p.field.nodes().end());
    REQUIRE(std::find(p.particle.nodes().begin(), p.particle.nodes().end(), src) !=
            p.particle.nodes().end());
}

TEST_CASE("graph.domain.plan_skips_render_domain", "[graph][domain]") {
    // A render item has no compute, so it enters no field/particle evaluation plan and
    // instead appears in the render **declaration** list (it is never evaluated).
    QP_DOMAIN_SCENE(s);
    const NodeId src = s.add_node("source", "src");
    const NodeId item = s.add_node("draw_field", "item");
    s.wire(src, 1, item, 1);
    s.declared.add(DeclaredOutput{item, 1});

    const ExecutionPlan p = s.plan();
    // The render domain holds declarations only, with no execution order
    REQUIRE(p.render.declared().size() == 1);
    REQUIRE(p.render.declared().front().node == item);
    REQUIRE(p.render.order().empty());
    REQUIRE(p.render.nodes().empty());
    // The evaluation domains hold no render item (it has no compute)
    REQUIRE(find_plan_for(p, item) == nullptr);
    // But its upstream is still there (it needs data)
    REQUIRE(find_plan_for(p, src) != nullptr);
}

TEST_CASE("graph.domain.plan_declared_output_recorded", "[graph][domain]") {
    // A render item's "plan" is the declaration itself: **no port-existence filtering**.
    // Whether a declaration is valid is reported by check_declarations, and the plan does not
    // swallow that news -- otherwise the user asks "I declared an output, where is the result?".
    QP_DOMAIN_SCENE(s);
    const NodeId item = s.add_node("draw_field", "item");
    // A render item has input ports only, no output ports
    s.declared.add(DeclaredOutput{item, 1});
    s.declared.add(DeclaredOutput{item, 2});   // that port does not exist, but must still be recorded

    const ExecutionPlan p = s.plan();
    REQUIRE(p.render.declared().size() == 2);
    REQUIRE(p.render.declared()[0] == DeclaredOutput{item, 1});
    REQUIRE(p.render.declared()[1] == DeclaredOutput{item, 2});
}

TEST_CASE("graph.domain.plan_is_deterministic", "[graph][domain]") {
    QP_DOMAIN_SCENE(s);
    const NodeId a = s.add_node("source");
    const NodeId b = s.add_node("source");
    const NodeId c = s.add_node("source");
    s.wire(a, 1, b, 1);
    s.wire(b, 1, c, 1);
    s.declared.add(DeclaredOutput{c, 1});

    const ExecutionPlan p1 = s.plan();
    const ExecutionPlan p2 = s.plan();
    REQUIRE(p1.field.nodes() == p2.field.nodes());
    REQUIRE(p1.field.order() == p2.field.order());
    REQUIRE(p1.particle.order() == p2.particle.order());
}

// ===========================================================================
// domain permissions
// ===========================================================================

TEST_CASE("graph.domain.allowed_in_domain", "[graph][domain]") {
    const NodeDesc both = make_processor("both", true, true);
    REQUIRE(allowed_in_domain(both, Domain::field));
    REQUIRE(allowed_in_domain(both, Domain::particle));

    const NodeDesc bake_only = make_processor("bake_only", true, false);
    REQUIRE(allowed_in_domain(bake_only, Domain::field));
    REQUIRE_FALSE(allowed_in_domain(bake_only, Domain::particle));

    const NodeDesc fast_only = make_processor("fast_only", false, true);
    REQUIRE_FALSE(allowed_in_domain(fast_only, Domain::field));
    REQUIRE(allowed_in_domain(fast_only, Domain::particle));
}

TEST_CASE("graph.domain.render_always_allowed", "[graph][domain]") {
    // The render domain takes no part in evaluation, so real-time limits do not apply to it
    const NodeDesc nothing = make_processor("neither", false, false);
    REQUIRE_FALSE(allowed_in_domain(nothing, Domain::field));
    REQUIRE_FALSE(allowed_in_domain(nothing, Domain::particle));
    REQUIRE(allowed_in_domain(nothing, Domain::render));
}

TEST_CASE("graph.domain.report_domain_violation", "[graph][domain]") {
    QP_DOMAIN_SCENE(s);
    const NodeId baker = s.add_node("baker", "my_baker");

    // The bake domain: passes
    Report ok = check_domains(s.g, s.ctx(), Domain::field);
    REQUIRE(ok.ok());

    // The real-time domain: a violation
    const Report bad = check_domains(s.g, s.ctx(), Domain::particle);
    REQUIRE_FALSE(bad.ok());
    REQUIRE(bad.size() == 1);
    const Issue& i = bad.issues().front();
    REQUIRE(i.severity == Severity::error);
    REQUIRE(i.node == baker);
    REQUIRE(i.code == qp::diag::ErrorCode::plugin_capability_missing);
    REQUIRE(i.hint.find("runs every frame") != std::string::npos);
    REQUIRE(i.message.find("my_baker") != std::string::npos);
}

TEST_CASE("graph.domain.report_clean", "[graph][domain]") {
    QP_DOMAIN_SCENE(s);
    (void)s.add_node("source", "a");
    (void)s.add_node("source", "b");
    REQUIRE(check_domains(s.g, s.ctx(), Domain::field).ok());
    REQUIRE(check_domains(s.g, s.ctx(), Domain::particle).ok());
    REQUIRE(check_domains(s.g, s.ctx(), Domain::render).ok());
}

TEST_CASE("graph.domain.report_ignores_unknown_types", "[graph][domain]") {
    // Unknown types are reported by the generic check; the domain check does not repeat them
    QP_DOMAIN_SCENE(s);
    (void)s.add_node("no_such_type", "ghost");
    REQUIRE(check_domains(s.g, s.ctx(), Domain::particle).ok());
}

TEST_CASE("graph.domain.report_stale_declaration", "[graph][domain]") {
    QP_DOMAIN_SCENE(s);
    const NodeId a = s.add_node("source", "a");
    s.declared.add(DeclaredOutput{a, 1});            // valid
    s.declared.add(DeclaredOutput{a, 7});            // the node has no port 7
    s.declared.add(DeclaredOutput{NodeId{99, 9}, 1});  // a node that does not exist

    const Report r = check_declarations(s.g, s.ctx(), s.declared);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.size() == 2);

    bool has_unknown_port = false, has_unknown_node = false;
    for (const auto& i : r.issues()) {
        if (i.code == qp::diag::ErrorCode::unknown_port) has_unknown_port = true;
        if (i.code == qp::diag::ErrorCode::unknown_node) has_unknown_node = true;
        REQUIRE(i.hint.find("stale") != std::string::npos);
    }
    REQUIRE(has_unknown_port);
    REQUIRE(has_unknown_node);
}

TEST_CASE("graph.domain.report_stale_declaration_for_deleted_node", "[graph][domain]") {
    QP_DOMAIN_SCENE(s);
    const NodeId a = s.add_node("source", "a");
    const NodeId b = s.add_node("source", "b");
    s.wire(a, 1, b, 1);
    s.declared.add(DeclaredOutput{b, 1});
    REQUIRE(check_declarations(s.g, s.ctx(), s.declared).ok());

    // Deleting a declared node -> the declaration goes stale
    REQUIRE(s.g.remove_node(b));
    const Report r = check_declarations(s.g, s.ctx(), s.declared);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.size() == 1);
    REQUIRE(r.issues().front().code == qp::diag::ErrorCode::unknown_node);
}

TEST_CASE("graph.domain.plan_ignores_stale_declarations", "[graph][domain]") {
    // A stale declaration must not crash: the plan skips it silently (check_declarations reports it)
    QP_DOMAIN_SCENE(s);
    const NodeId a = s.add_node("source", "a");
    s.declared.add(DeclaredOutput{NodeId{99, 9}, 1});
    s.declared.add(DeclaredOutput{a, 1});

    const ExecutionPlan p = s.plan();
    REQUIRE(p.field.size() == 1);
    REQUIRE(p.field.order().front() == a);
}
