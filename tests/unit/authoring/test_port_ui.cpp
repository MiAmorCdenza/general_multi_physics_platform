/**
 * @file test_port_ui.cpp
 * @brief Tests for the port-type UI registry.
 *
 * Test case ids match the @tests fields in the portui headers byte for byte.
 *
 * The claim worth testing hardest is the negative one: **describe() never fails**.
 * A type with no registered description gets a read-only display naming the type,
 * which is the difference between "a plugin's parameter cannot be edited yet" and
 * "the panel will not open". The first is a limitation a user can work around; the
 * second loses the whole session.
 *
 * The second claim is that the fallback is `read_only` and never `number`. An
 * unknown type might hold text, a handle, or something with no numeric meaning,
 * and a number control that cannot represent the value would show a 0 where the
 * user's file reference used to be -- and the user would believe it.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/authoring/portui.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace qp::authoring;

namespace {

/// @brief A well-formed numeric description, so each test varies one field.
PortUiDesc numeric_desc(const std::string& type) {
    PortUiDesc d;
    d.port_type = type;
    d.editor = EditorKind::number;
    d.label = "Gain";
    d.minimum = 0.0;
    d.maximum = 10.0;
    return d;
}

}  // namespace

// ===========================================================================
// Vocabulary
// ===========================================================================

TEST_CASE("portui.description.editor_kinds", "[portui]") {
    STATIC_REQUIRE(static_cast<std::uint8_t>(EditorKind::read_only) == 0);
    STATIC_REQUIRE(std::string(to_string(EditorKind::read_only)) == "read_only");
    STATIC_REQUIRE(std::string(to_string(EditorKind::number)) == "number");
    STATIC_REQUIRE(std::string(to_string(EditorKind::boolean)) == "boolean");
    STATIC_REQUIRE(std::string(to_string(EditorKind::text)) == "text");
    STATIC_REQUIRE(std::string(to_string(EditorKind::choice)) == "choice");
    STATIC_REQUIRE(std::string(to_string(EditorKind::reference)) == "reference");

    for (const EditorKind k : {EditorKind::read_only, EditorKind::number, EditorKind::boolean,
                               EditorKind::text, EditorKind::choice, EditorKind::reference}) {
        REQUIRE(std::string(to_string(k)) != "unknown");
    }
}

TEST_CASE("portui.description.numeric_bounds", "[portui]") {
    SECTION("unbounded accepts anything finite") {
        // An absent bound means "no constraint", not "0..100". A spin box that
        // invented a default range would reject a legitimate value, and the user
        // would have no way to enter it.
        PortUiDesc d;
        d.port_type = "qp.scalar_f64";
        d.editor = EditorKind::number;
        REQUIRE(d.is_valid());
        REQUIRE(d.accepts(0.0));
        REQUIRE(d.accepts(-1e12));
        REQUIRE(d.accepts(1e12));
        REQUIRE_FALSE(d.accepts(std::numeric_limits<double>::quiet_NaN()));
        REQUIRE_FALSE(d.accepts(std::numeric_limits<double>::infinity()));
    }

    SECTION("bounds are inclusive") {
        const PortUiDesc d = numeric_desc("qp.scalar_f64");
        REQUIRE(d.accepts(0.0));
        REQUIRE(d.accepts(10.0));
        REQUIRE(d.accepts(5.0));
        REQUIRE_FALSE(d.accepts(-0.001));
        REQUIRE_FALSE(d.accepts(10.001));
    }

    SECTION("an empty port type is valid, because the fallback needs it") {
        // fallback("") is what a panel renders for a parameter whose type is not
        // registered -- on a machine without the plugin, or while the user is
        // still building the graph. If that description were invalid, the one
        // path that is supposed to always work would be the one that fails.
        PortUiDesc d;
        d.editor = EditorKind::read_only;
        REQUIRE(d.is_valid());

        // A description that claims to edit a value but names no type is still
        // nonsense, and is caught by the caller: the registry keys on the type,
        // and set() of an unnamed type would create a slot nothing can address.
        PortUiDesc editor_without_type;
        editor_without_type.editor = EditorKind::number;
        REQUIRE(editor_without_type.is_valid());
    }

    SECTION("a choice with no options is invalid") {
        // There is nothing to choose between, so the control cannot be drawn.
        // Refusing it here fails the plugin at load, with a reason, instead of
        // producing an empty dropdown.
        PortUiDesc d;
        d.port_type = "qp.enum";
        d.editor = EditorKind::choice;
        REQUIRE_FALSE(d.is_valid());
        d.choices = {"a", "b"};
        REQUIRE(d.is_valid());
    }

    SECTION("inverted or non-numeric bounds are invalid") {
        PortUiDesc d = numeric_desc("qp.scalar_f64");
        d.minimum = 10.0;
        d.maximum = 0.0;
        REQUIRE_FALSE(d.is_valid());   // accepts nothing, which is never intended

        PortUiDesc nan_min = numeric_desc("qp.scalar_f64");
        nan_min.minimum = std::numeric_limits<double>::quiet_NaN();
        REQUIRE_FALSE(nan_min.is_valid());

        PortUiDesc bad_step = numeric_desc("qp.scalar_f64");
        bad_step.step = 0.0;
        REQUIRE_FALSE(bad_step.is_valid());
        bad_step.step = -1.0;
        REQUIRE_FALSE(bad_step.is_valid());
        bad_step.step = 0.5;
        REQUIRE(bad_step.is_valid());
    }

    SECTION("a read-only description with no other fields is valid") {
        // The fallback shape must itself pass validation, or the fallback path
        // would be the one place a malformed description reaches a renderer.
        const PortUiDesc d = PortUiRegistry::fallback("qp.whatever");
        REQUIRE(d.is_valid());
        REQUIRE(d.editor == EditorKind::read_only);
    }
}

// ===========================================================================
// The registry
// ===========================================================================

TEST_CASE("portui.registry.fallback_is_always_available", "[portui]") {
    const PortUiRegistry registry;   // nothing registered at all

    // describe() must not fail for any input. This is the promise that keeps a
    // panel open when a plugin is missing.
    const PortUiDesc d = registry.describe("qp.not_registered");
    REQUIRE(d.is_valid());
    REQUIRE(d.port_type == "qp.not_registered");
    REQUIRE(d.editor == EditorKind::read_only);

    // read_only, not number: an unknown type might hold text, a handle, or
    // something with no numeric meaning. A number control that cannot represent
    // the value would show 0, and the user would believe it.
    REQUIRE(d.editor != EditorKind::number);

    // The fallback names the type, so the panel says what it cannot edit rather
    // than being blank.
    REQUIRE_FALSE(d.label.empty());

    // An empty type name is still renderable: a node may declare a parameter with
    // no registered type while the user is still building the graph.
    const PortUiDesc untyped = registry.describe("");
    REQUIRE(untyped.is_valid());
    REQUIRE_FALSE(untyped.label.empty());

    REQUIRE(registry.size() == 0);
    REQUIRE(registry.port_types().empty());
    REQUIRE_FALSE(registry.has("qp.not_registered"));
}

TEST_CASE("portui.registry.registered_description_wins", "[portui]") {
    PortUiRegistry registry;

    REQUIRE(registry.set(numeric_desc("qp.scalar_f64")).has_value());
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.has("qp.scalar_f64"));
    REQUIRE(registry.port_types() == std::vector<std::string>{"qp.scalar_f64"});

    const PortUiDesc d = registry.describe("qp.scalar_f64");
    REQUIRE(d.editor == EditorKind::number);
    REQUIRE(d.label == "Gain");
    REQUIRE(d.accepts(5.0));
    REQUIRE_FALSE(d.accepts(50.0));

    SECTION("a second registration replaces the first") {
        // Replacement, not refusal, and deliberately unlike the other registries
        // in this project: a plugin overriding the generic editor for a type it
        // owns is the intended use. What makes it safe is that plugin load order
        // is deterministic, so "last wins" is a stable outcome rather than a race.
        PortUiDesc better = numeric_desc("qp.scalar_f64");
        better.editor = EditorKind::number;
        better.label = "Gain (V/V)";
        better.maximum = 100.0;
        REQUIRE(registry.set(better).has_value());
        REQUIRE(registry.size() == 1);
        REQUIRE(registry.port_types().size() == 1);
        REQUIRE(registry.describe("qp.scalar_f64").label == "Gain (V/V)");
        REQUIRE(registry.describe("qp.scalar_f64").accepts(50.0));
    }

    SECTION("a malformed description is refused and changes nothing") {
        PortUiDesc bad;
        bad.port_type = "qp.broken";
        bad.editor = EditorKind::choice;   // no choices
        const auto refused = registry.set(bad);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == qp::diag::ErrorCode::invalid_argument);
        REQUIRE(registry.size() == 1);
        REQUIRE_FALSE(registry.has("qp.broken"));
    }

    SECTION("a unit-carrying description survives") {
        // The unit and dimension are part of the description on purpose: if the
        // editor showed only "1.5", the most common lab mistake -- millimetres
        // entered where metres were meant -- would be invisible.
        PortUiDesc with_unit = numeric_desc("qp.length_f64");
        with_unit.dimension = qp::units::Dim{1, 0, 0, 0, 0, 0, 0};
        REQUIRE(registry.set(with_unit).has_value());
        const PortUiDesc d2 = registry.describe("qp.length_f64");
        REQUIRE(d2.dimension.has_value());
        // Dim is a struct of seven named exponents, not an array: the axes have
        // names so that a reader does not have to remember the order.
        REQUIRE(d2.dimension->L == 1);
        REQUIRE(d2.dimension->M == 0);
    }
}

TEST_CASE("portui.registry.unregister_falls_back", "[portui]") {
    PortUiRegistry registry;
    REQUIRE(registry.set(numeric_desc("qp.scalar_f64")).has_value());
    REQUIRE(registry.set(numeric_desc("qp.scalar_f32")).has_value());
    REQUIRE(registry.size() == 2);

    REQUIRE(registry.remove("qp.scalar_f64").has_value());

    // The type falls back rather than disappearing: a panel that is already open
    // keeps working with the generic display, instead of losing the parameter.
    REQUIRE(registry.size() == 1);
    REQUIRE_FALSE(registry.has("qp.scalar_f64"));
    const PortUiDesc d = registry.describe("qp.scalar_f64");
    REQUIRE(d.is_valid());
    REQUIRE(d.editor == EditorKind::read_only);
    REQUIRE(registry.has("qp.scalar_f32"));

    // Removing something that was never registered is reported, not ignored.
    REQUIRE_FALSE(registry.remove("qp.scalar_f64").has_value());
    REQUIRE_FALSE(registry.remove("qp.never").has_value());
    REQUIRE(registry.size() == 1);
}
