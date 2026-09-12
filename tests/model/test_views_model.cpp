/**
 * @file test_views_model.cpp
 * @brief Tests for the Qt-free half of the view layer.
 *
 * Test case ids match the @tests fields in the model headers byte for byte.
 *
 * These tests exist because the alternative was to check the view layer by looking
 * at it. A property panel that gives an unbounded parameter a 0..100 range still
 * draws correctly; it just silently refuses the value the student needs to enter.
 * A canvas that keeps its own node list still draws correctly; it just keeps
 * drawing a node the graph no longer has. Neither is visible in a screenshot, so
 * both are asserted here, in the ordinary suite that runs on both compilers --
 * including GCC, which cannot link Qt at all.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/views/model/demo_library.hpp>
#include <qp/views/model/editor_choice.hpp>
#include <qp/views/model/execution_binders.hpp>
#include <qp/views/model/type_catalog.hpp>

#include <qp/graph/execution/execution.hpp>
#include <qp/graph/ir.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace qp::views;
using qp::authoring::EditorKind;
using qp::graph::PortDesc;
using qp::graph::PortNumber;

namespace {

/// @brief A parameter port of the given type: editable, not connectable.
PortDesc parameter(PortNumber number, qp::ports::PortTypeId type) {
    PortDesc p;
    p.number = number;
    p.name = "p" + std::to_string(number);
    p.type = type;
    p.connectable = false;
    return p;
}

/// @brief A connectable port: a socket, not a value.
PortDesc socket(PortNumber number, qp::ports::PortTypeId type) {
    PortDesc p = parameter(number, type);
    p.connectable = true;
    return p;
}

/// @brief A description that claims to edit `type` as a number.
qp::authoring::PortUiDesc numeric_description(const std::string& type, double low, double high) {
    qp::authoring::PortUiDesc d;
    d.port_type = type;
    d.editor = EditorKind::number;
    d.minimum = low;
    d.maximum = high;
    return d;
}

/// @brief An inert binder, so the mounting rules can be asserted without a plugin.
class InertBinder final : public qp::graph::execution::IOperatorBinder {
public:
    [[nodiscard]] bool can_bind(std::string_view,
                                const qp::graph::execution::StateView&) const noexcept override {
        return false;
    }

    [[nodiscard]] std::unique_ptr<qp::graph::execution::IStateOperator> bind(
        std::string_view, const qp::graph::Node&,
        const qp::graph::execution::StateView&) override {
        return nullptr;
    }
};

}  // namespace

// ===========================================================================
// The type catalog
// ===========================================================================

TEST_CASE("views.catalog.register_and_enumerate", "[views]") {
    TypeCatalog catalog;
    REQUIRE(catalog.size() == 0);
    REQUIRE(catalog.all().empty());

    qp::graph::NodeDesc d;
    d.type_name = "x.one";
    d.label = "One";
    REQUIRE(catalog.add(d).has_value());

    REQUIRE(catalog.size() == 1);
    REQUIRE(catalog.all().size() == 1);
    REQUIRE(catalog.find("x.one") != nullptr);
    REQUIRE(catalog.find("x.one")->label == "One");
    REQUIRE(catalog.find("absent") == nullptr);
    REQUIRE(catalog.find("") == nullptr);

    // The catalog is what a palette enumerates, so the enumeration must be the
    // registration order: a palette that reordered between runs would move a
    // user's node out from under their muscle memory.
    qp::graph::NodeDesc second;
    second.type_name = "x.two";
    REQUIRE(catalog.add(second).has_value());
    REQUIRE(catalog.all()[0]->type_name == "x.one");
    REQUIRE(catalog.all()[1]->type_name == "x.two");

    SECTION("a duplicate type name is refused, not replaced") {
        // The type name is what a saved document refers to. Replacing it would
        // silently change the meaning of every document that already names it,
        // which is far worse than a plugin failing to load.
        qp::graph::NodeDesc clash;
        clash.type_name = "x.one";
        clash.label = "Replacement";
        const auto refused = catalog.add(clash);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == qp::diag::ErrorCode::duplicate_connection);
        REQUIRE(catalog.size() == 2);
        REQUIRE(catalog.find("x.one")->label == "One");
    }

    SECTION("an empty type name is refused") {
        qp::graph::NodeDesc blank;
        REQUIRE_FALSE(catalog.add(blank).has_value());
        REQUIRE(catalog.size() == 2);
    }

    SECTION("removal frees the name") {
        // A plugin that unloads must be reloadable. Leaving the name taken would
        // make an unloaded plugin permanently unloadable.
        REQUIRE(catalog.remove("x.one"));
        REQUIRE(catalog.size() == 1);
        REQUIRE(catalog.find("x.one") == nullptr);
        REQUIRE(catalog.add(d).has_value());
        REQUIRE_FALSE(catalog.remove("never-registered"));
    }
}

TEST_CASE("views.catalog.grouped_by_category", "[views]") {
    TypeCatalog catalog;
    qp::graph::NodeDesc a;
    a.type_name = "a";
    a.category = "models";
    qp::graph::NodeDesc b;
    b.type_name = "b";
    b.category = "models";
    qp::graph::NodeDesc c;
    c.type_name = "c";
    c.category = "sources";
    qp::graph::NodeDesc uncategorised;
    uncategorised.type_name = "d";   // no category
    REQUIRE(catalog.add(a).has_value());
    REQUIRE(catalog.add(b).has_value());
    REQUIRE(catalog.add(c).has_value());
    REQUIRE(catalog.add(uncategorised).has_value());

    const auto groups = catalog.by_category();
    REQUIRE(groups.size() == 3);
    REQUIRE(groups[0].first == "models");
    REQUIRE(groups[0].second.size() == 2);
    REQUIRE(groups[1].first == "sources");
    REQUIRE(groups[1].second.size() == 1);

    // A type with no category appears under a visible placeholder rather than
    // being dropped: a node the palette cannot reach is a node the user cannot
    // place, so "the plugin forgot a category" must not look like "the plugin
    // did not register".
    REQUIRE(groups[2].first == kUncategorised);
    REQUIRE(groups[2].second.size() == 1);
    REQUIRE(groups[2].second[0]->type_name == "d");
}

TEST_CASE("views.demo.library_has_expected_types", "[views]") {
    TypeCatalog catalog;
    REQUIRE(register_demo_library(catalog).has_value());
    REQUIRE(catalog.size() == 5);
    REQUIRE(catalog.find("demo.signal") != nullptr);
    REQUIRE(catalog.find("demo.spring_damper") != nullptr);
    REQUIRE(catalog.find("demo.instrument") != nullptr);
    REQUIRE(catalog.find("demo.export") != nullptr);
    REQUIRE(catalog.find("demo.filter") != nullptr);

    // Registering twice into the same catalog is refused at the first duplicate,
    // and the catalog is left as it was rather than half-overwritten.
    const auto again = register_demo_library(catalog);
    REQUIRE_FALSE(again.has_value());
    REQUIRE(again.error() == qp::diag::ErrorCode::duplicate_connection);
    REQUIRE(catalog.size() == 5);

    // The demonstrators are chosen to cover every editor path, so the library
    // itself has to contain an instance of each: an unbounded parameter (to prove
    // the panel does not invent a range), a bounded one, an enum, a boolean and a
    // string. A demo library missing one would leave that path unexercised.
    int unbounded_numbers = 0;
    int bounded_numbers = 0;
    int enums = 0;
    int booleans = 0;
    int strings = 0;
    int sockets = 0;
    for (const qp::graph::NodeDesc* d : catalog.all()) {
        for (const PortDesc& p : d->inputs) {
            if (p.connectable) {
                ++sockets;
                continue;
            }
            if (p.type == qp::ports::kBool) ++booleans;
            else if (p.type == qp::ports::kString) ++strings;
            else if (!p.choice_labels.empty()) ++enums;
            else if (p.has_range) ++bounded_numbers;
            else ++unbounded_numbers;
        }
    }
    REQUIRE(unbounded_numbers > 0);
    REQUIRE(bounded_numbers > 0);
    REQUIRE(enums > 0);
    REQUIRE(booleans > 0);
    REQUIRE(strings > 0);
    REQUIRE(sockets > 0);

    // And the demonstrators must not claim to be domain-specific: they are the
    // view layer's fixtures, not the platform's physics, which belongs in plugins.
    for (const qp::graph::NodeDesc* d : catalog.all()) {
        REQUIRE(d->type_name.rfind("demo.", 0) == 0);
    }
}

// ===========================================================================
// Editor selection
// ===========================================================================

TEST_CASE("views.editor_choice.parameter_versus_socket", "[views]") {
    const qp::authoring::PortUiDesc none = fallback_description("");

    // A connectable port is wired on the canvas, never edited in the panel. A
    // parameter drawn as a socket invites a connection the model refuses, and a
    // UI that offers what the model forbids is worse than one that omits it.
    const EditorChoice wired = choose_editor(socket(1, qp::ports::kScalarF64), none);
    REQUIRE(wired.kind == EditorKind::read_only);

    // The same type, not connectable, is a value to edit.
    const EditorChoice editable = choose_editor(parameter(1, qp::ports::kScalarF64), none);
    REQUIRE(editable.kind == EditorKind::number);
}

TEST_CASE("views.editor_choice.declared_range_is_bounded", "[views]") {
    const qp::authoring::PortUiDesc none = fallback_description("");

    PortDesc unbounded = parameter(1, qp::ports::kScalarF64);
    const EditorChoice loose = choose_editor(unbounded, none);
    REQUIRE(loose.kind == EditorKind::number);
    // Not bounded. A panel that invented 0..100 for an unbounded parameter would
    // silently reject a legitimate value, and the student would have no way to
    // enter it -- which is the defect this assertion exists to catch.
    REQUIRE_FALSE(loose.bounded);

    PortDesc ranged = parameter(1, qp::ports::kScalarF64);
    ranged.has_range = true;
    ranged.min_value = 0.0;
    ranged.max_value = 10.0;
    const EditorChoice tight = choose_editor(ranged, none);
    REQUIRE(tight.kind == EditorKind::number);
    REQUIRE(tight.bounded);

    // A unit travels with the value: a number without its unit beside it is how a
    // lab report becomes wrong.
    PortDesc with_unit = parameter(1, qp::ports::kScalarF64);
    with_unit.unit_symbol = "N/m";
    REQUIRE(choose_editor(with_unit, none).has_unit);
}

TEST_CASE("views.editor_choice.enum_options_come_from_the_port", "[views]") {
    PortDesc enum_port = parameter(1, qp::ports::kEnum);
    enum_port.choice_names = {"0", "1", "2"};
    enum_port.choice_labels = {"sine", "square", "ramp"};

    // The options come from the port, because the port is what the model accepts.
    // A description offering different options would let a user pick a value the
    // model then rejects.
    const qp::authoring::PortUiDesc conflicting = numeric_description("6", 0.0, 2.0);
    const EditorChoice choice = choose_editor(enum_port, conflicting);
    REQUIRE(choice.kind == EditorKind::choice);
    REQUIRE(choice.from_port_enum);

    SECTION("a mismatched choice list falls back rather than producing an off-by-one") {
        // `choice_labels` longer than `choice_names` would give a combo whose
        // indices do not match the stored values -- a silent off-by-one in the
        // user's data, which is worse than showing a read-only field.
        PortDesc broken = parameter(2, qp::ports::kEnum);
        broken.choice_names = {"0"};
        broken.choice_labels = {"a", "b", "c"};
        REQUIRE_FALSE(has_choices(broken));
        const EditorChoice fallback = choose_editor(broken, fallback_description("6"));
        REQUIRE(fallback.kind != EditorKind::choice);
    }
}

TEST_CASE("views.editor_choice.description_refines_the_control", "[views]") {
    // A registered description lets a plugin decide the control for its own type
    // without the panel knowing the type exists. That is portui's whole purpose,
    // and this is the assertion that keeps it true.
    PortDesc text_port = parameter(1, qp::ports::kString);
    REQUIRE(choose_editor(text_port, fallback_description("5")).kind == EditorKind::text);

    qp::authoring::PortUiDesc as_number = numeric_description("5", 0.0, 1.0);
    const EditorChoice refined = choose_editor(text_port, as_number);
    REQUIRE(refined.kind == EditorKind::number);
    // A description may bound a value the port left unbounded: the plugin knows
    // its own type's useful range.
    REQUIRE(refined.bounded);

    // A dimension in the description means the value carries a unit.
    qp::authoring::PortUiDesc with_dim = numeric_description("1", 0.0, 1.0);
    with_dim.dimension = qp::units::Dim{1, 0, 0, 0, 0, 0, 0};
    REQUIRE(choose_editor(parameter(1, qp::ports::kScalarF64), with_dim).has_unit);

    // A read-only description does not count as a refinement: it is the sentinel
    // meaning "nothing was registered", so the port type's own default applies.
    qp::authoring::PortUiDesc read_only;
    read_only.port_type = "5";
    read_only.editor = EditorKind::read_only;
    REQUIRE(choose_editor(text_port, read_only).kind == EditorKind::text);
}

TEST_CASE("views.editor_choice.unregistered_type_is_read_only", "[views]") {
    // The case that matters most. An unknown port type may hold text, a handle or
    // something with no numeric meaning at all. A numeric control that cannot
    // represent the value would display a 0 where the user's file reference used
    // to be -- and the user would believe it.
    const auto exotic = static_cast<qp::ports::PortTypeId>(900);
    const EditorChoice choice = choose_editor(parameter(1, exotic), fallback_description("900"));
    REQUIRE(choice.kind == EditorKind::read_only);
    REQUIRE(choice.kind != EditorKind::number);

    // A field type is not a text field either: it is a handle to something the
    // panel cannot edit, and showing it as a string would invite a user to type
    // into it.
    REQUIRE(choose_editor(parameter(1, qp::ports::kScalarField), fallback_description("20")).kind ==
            EditorKind::read_only);
    REQUIRE(choose_editor(parameter(1, qp::ports::kDataset), fallback_description("60")).kind ==
            EditorKind::read_only);

    // The fallback itself is always a usable, renderable description.
    const qp::authoring::PortUiDesc fallback = fallback_description("900");
    REQUIRE(fallback.is_valid());
    REQUIRE(fallback.editor == EditorKind::read_only);
    REQUIRE_FALSE(fallback.label.empty());
}

// ===========================================================================
// Where the window gets its binders
// ===========================================================================

TEST_CASE("views.binders.mounted_once_and_in_order", "[views]") {
    // The inversion that keeps the view layer free of the plugin layer: `views/model` declares where
    // binders come from, and whoever assembles the application puts them there. The rules that make that
    // usable rather than merely possible are what is asserted here.
    //
    // This list is process-wide, so the case asserts its own additions and never the list's absolute
    // contents. A case that demanded an empty list would pass alone and fail in a suite, which is the
    // shape of test that gets deleted rather than fixed.
    std::vector<qp::graph::execution::IOperatorBinder*>& binders =
        qp::views::model::execution_binders();

    // The same object every call, because a caller that copied it would be copying borrows.
    REQUIRE(&qp::views::model::execution_binders() == &binders);

    InertBinder first;
    InertBinder second;
    const std::size_t before = binders.size();

    qp::views::model::mount_execution_binder(&first);
    REQUIRE(binders.size() == before + 1);
    REQUIRE(binders.back() == &first);

    // Idempotent per pointer: mounting the same binder twice would have it consulted twice, which is
    // wasted work rather than a wrong answer -- and the deduplication costs one comparison.
    qp::views::model::mount_execution_binder(&first);
    REQUIRE(binders.size() == before + 1);

    // Nothing is reordered: the list is the consultation order, and a first binder that claimed a type
    // must keep claiming it before a later one is asked.
    qp::views::model::mount_execution_binder(&second);
    REQUIRE(binders.size() == before + 2);
    REQUIRE(binders[before] == &first);
    REQUIRE(binders[before + 1] == &second);

    // A null binder is ignored rather than stored. A caller assembling binders from optional plugins will
    // have holes, and a stored null would be dereferenced on every run -- the search does skip nulls, but
    // the list is what a reader looks at to see what is mounted.
    qp::views::model::mount_execution_binder(nullptr);
    REQUIRE(binders.size() == before + 2);

    // The published list is what a run consults, so the pointers in it have to be the ones a caller
    // mounted: a copy would leave the window running binders nobody filled in.
    binders.pop_back();
    binders.pop_back();
    REQUIRE(binders.size() == before);
}
