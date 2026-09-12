/**
 * @file test_reflect.cpp
 * @brief Tests for the type-name reflection.
 *
 * Test case ids match the @tests fields in the reflect headers byte for byte.
 *
 * The claim under test is not "a static member can be read" -- that is a container.
 * It is that **the name is one spelling, stable enough to be evidence**:
 *
 *   - written down once, next to the type, so C++, YAML and a script cannot drift;
 *   - not `typeid`, which is implementation-defined and would make a document
 *     unreadable by a different build;
 *   - validated at the point it enters the system, because the name is the key a
 *     saved document is looked up by.
 *
 * The enum half carries the same argument one level down: a choice parameter stores
 * an integer, and a document must also record what that integer meant, or a
 * reordered enum silently turns a saved `2` into a different option.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/reflect/reflect.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

using namespace qp::reflect;

namespace {

/// @brief A type that names itself, the way a plugin's node descriptor would.
struct SpringDamper final {
    static constexpr std::string_view type_name = "qp.spring_damper";
};

/// @brief A second type, so that two names can be compared for distinctness.
struct SignalSource final {
    static constexpr std::string_view type_name = "qp.signal_source";
};

/// @brief An enum that lists its own labels, in declaration order.
enum class Waveform {
    sine = 0,
    square = 1,
    ramp = 2,
};

/// @brief A type that does NOT name itself, to prove the concept rejects it.
struct Anonymous final {
    int value = 0;
};

}  // namespace

// ===========================================================================
// Type names
// ===========================================================================

TEST_CASE("reflect.type_name.is_compile_time", "[reflect]") {
    // The name is usable in a constant expression, which is what lets a contract or
    // a static layout assertion refer to it without a runtime lookup.
    static_assert(type_name<SpringDamper>() == "qp.spring_damper");
    STATIC_REQUIRE(type_name<SpringDamper>() == "qp.spring_damper");
    STATIC_REQUIRE(type_name<SignalSource>() == "qp.signal_source");

    // The deduced overload returns the same thing as the explicit one, so a caller
    // with a value does not have to spell the type.
    const SpringDamper damper;
    REQUIRE(type_name(damper) == type_name<SpringDamper>());
    REQUIRE(type_name(damper) == "qp.spring_damper");

    // Two different types never share a name: distinctness is what a document's
    // lookup depends on, and a collision would resolve to whichever was registered
    // first.
    STATIC_REQUIRE(type_name<SpringDamper>() != type_name<SignalSource>());

    // The concept accepts a named type and rejects an unnamed one. Asserted rather
    // than assumed, because the concept is the error message a plugin author sees
    // when they forget to name their type -- and a concept that accepted everything
    // would turn that into an unreadable template error inside <string_view>.
    STATIC_REQUIRE(is_named_v<SpringDamper>);
    STATIC_REQUIRE(is_named_v<SignalSource>);
    STATIC_REQUIRE_FALSE(is_named_v<Anonymous>);
    STATIC_REQUIRE_FALSE(is_named_v<int>);
    STATIC_REQUIRE_FALSE(is_named_v<std::string>);
}

TEST_CASE("reflect.type_name.is_not_compiler_specific", "[reflect]") {
    const std::string_view name = type_name<SpringDamper>();

    // The property that makes a name usable in a saved document: it is the same on
    // every compiler. `typeid(T).name()` is not -- MSVC returns
    // "class Qp::SpringDamper" and GCC returns a mangled form, and both may change
    // between versions -- so a document written by one build would not load in
    // another. This test would still pass with `typeid` on a single compiler, which
    // is why it checks the *shape* the convention requires rather than only
    // comparing against a literal.
    REQUIRE(is_valid_name(name));

    // No mangling artefacts: a mangled name starts with 'N' on Itanium ABI, and
    // MSVC's spelling carries the keyword "class"/"struct".
    REQUIRE(name.rfind("N", 0) != 0);
    REQUIRE(name.find("class ") == std::string_view::npos);
    REQUIRE(name.find("struct ") == std::string_view::npos);
    REQUIRE(name.find(' ') == std::string_view::npos);

    // Lower-case with a project prefix, matching the port-type convention the rest
    // of the platform already uses.
    REQUIRE(name.rfind("qp.", 0) == 0);
    for (const char c : name) {
        // The operand is parenthesised because Catch2 intercepts `&&` inside an
        // assertion and refuses a chained comparison rather than silently evaluating
        // it as a single boolean.
        REQUIRE_FALSE((c >= 'A' && c <= 'Z'));
    }
}

TEST_CASE("reflect.type_name.validation", "[reflect]") {
    // The validator runs where a name enters the system, because the name is the key
    // a document is looked up by: an unusable one produces a document that cannot be
    // parsed back, and the failure appears later, in someone else's session.
    REQUIRE(is_valid_name("qp.spring_damper"));
    REQUIRE(is_valid_name("qp.scalar_f64"));
    REQUIRE(is_valid_name("a"));
    REQUIRE(is_valid_name("qp.a.b.c"));
    REQUIRE(is_valid_name("qp.with-dash"));
    REQUIRE(is_valid_name("qp.with123digits"));

    // Empty, or a leading or trailing dot: a name lookup would split it into an
    // empty segment, and a YAML reader and the registry would disagree about where
    // the type starts.
    REQUIRE_FALSE(is_valid_name(""));
    REQUIRE_FALSE(is_valid_name(".leading"));
    REQUIRE_FALSE(is_valid_name("trailing."));

    // A doubled dot produces an empty path segment, which is the same disagreement
    // in a form a reader is more likely to accept silently.
    REQUIRE_FALSE(is_valid_name("qp..double"));

    // Whitespace: a name with a space cannot round-trip through a YAML key without
    // quoting, and two readers would not agree on whether it did.
    REQUIRE_FALSE(is_valid_name("qp.has space"));
    REQUIRE_FALSE(is_valid_name(" qp.leading_space"));
    REQUIRE_FALSE(is_valid_name("qp.trailing_space "));
    REQUIRE_FALSE(is_valid_name("qp.has\ttab"));

    // Uppercase is refused so that the convention is one spelling rather than two:
    // case-insensitive lookup would make `qp.CSV` and `qp.csv` the same key, and
    // then which one a document recorded would depend on the writer.
    REQUIRE_FALSE(is_valid_name("QP.upper"));
    REQUIRE_FALSE(is_valid_name("qp.Mixed"));

    // Non-ASCII is refused: a name is a key, and a key that depends on a console
    // code page is the failure mode the dialect gate exists to prevent elsewhere.
    REQUIRE_FALSE(is_valid_name("qp.\xc3\xa9"));
}

// ===========================================================================
// Enumerations
// ===========================================================================

namespace {

/// @brief A named enum whose labels are declared beside it, the way a plugin's
///        choice parameter would.
///
/// One comma-separated compile-time string rather than a vector: the labels never
/// change, and a vector would allocate on every call for a constant. A caller that
/// needs them individually uses `enum_label`, which is what a document writer does.
enum class WaveformNamed {
    sine = 0,
    square = 1,
    ramp = 2,
};

// The labels live beside the enum as free functions, found by ADL. An enum's body
// may contain enumerators and nothing else, so a member function is not an option --
// a first version of this concept required one and no enum could satisfy it.
constexpr std::string_view qp_enum_names(WaveformNamed) noexcept { return "sine,square,ramp"; }
constexpr std::size_t qp_enum_count(WaveformNamed) noexcept { return 3; }

/// @brief A second labelled enum with a single option, to catch a separator
///        assumption in the splitter.
enum class OnOff {
    off = 0,
    on = 1,
};

constexpr std::string_view qp_enum_names(OnOff) noexcept { return "off,on"; }
constexpr std::size_t qp_enum_count(OnOff) noexcept { return 2; }

/// @brief A labelled enum with one option: no separator appears at all.
enum class Always {
    on = 0,
};

constexpr std::string_view qp_enum_names(Always) noexcept { return "on"; }
constexpr std::size_t qp_enum_count(Always) noexcept { return 1; }

}  // namespace

TEST_CASE("reflect.enum_names.are_ordered", "[reflect]") {
    // The whole string, and the count that must agree with it.
    STATIC_REQUIRE(enum_names<WaveformNamed>() == "sine,square,ramp");
    STATIC_REQUIRE(enum_count<WaveformNamed>() == 3);
    STATIC_REQUIRE(enum_count<OnOff>() == 2);
    STATIC_REQUIRE(enum_count<Always>() == 1);

    // The splitting accessor is what a document writer uses, and its order must
    // match the enumerators' declaration order -- because a choice parameter stores
    // an integer, and a reordered enum would silently turn a saved `2` into a
    // different option. So the labels are checked against the enumerators' own
    // values, not merely against a literal.
    REQUIRE(enum_label<WaveformNamed>(static_cast<std::size_t>(WaveformNamed::sine)) == "sine");
    REQUIRE(enum_label<WaveformNamed>(static_cast<std::size_t>(WaveformNamed::square)) ==
            "square");
    REQUIRE(enum_label<WaveformNamed>(static_cast<std::size_t>(WaveformNamed::ramp)) == "ramp");

    // Every declared label is reachable, and nothing beyond the count is: a splitter
    // that dropped the last field or ran off the end would fail one of these.
    for (std::size_t i = 0; i < enum_count<WaveformNamed>(); ++i) {
        REQUIRE_FALSE(enum_label<WaveformNamed>(i).empty());
    }
    // Out of range yields empty rather than terminating: a document can hold an
    // integer a newer build wrote, and "this option is unknown" is a thing a reader
    // must be able to say.
    REQUIRE(enum_label<WaveformNamed>(3).empty());
    REQUIRE(enum_label<WaveformNamed>(99).empty());

    // A single-option enum has no separator at all, which an implementation that
    // assumed one would return empty for.
    REQUIRE(enum_label<Always>(0) == "on");
    REQUIRE(enum_label<Always>(1).empty());

    // And the labels are what the enumerators mean, one level down from the type
    // name: `OnOff::off` is 0, so label 0 is "off".
    REQUIRE(enum_label<OnOff>(0) == "off");
    REQUIRE(enum_label<OnOff>(1) == "on");
    REQUIRE(enum_label<OnOff>(2).empty());

    // The concepts. An enum without a `names()` member is not Enumerated, so the
    // error a plugin author sees names the missing member rather than failing deep
    // inside a template -- which is the whole reason the constraint is a concept.
    STATIC_REQUIRE(Enumerated<WaveformNamed>);
    STATIC_REQUIRE(Enumerated<OnOff>);
    STATIC_REQUIRE(Enumerated<Always>);
    STATIC_REQUIRE_FALSE(Enumerated<Waveform>);     // an enum, but it does not list labels
    STATIC_REQUIRE_FALSE(Enumerated<SpringDamper>); // named, but not an enum
    STATIC_REQUIRE_FALSE(Enumerated<int>);
    STATIC_REQUIRE_FALSE(Enumerated<std::string>);

    // A named enum is not automatically Named, and vice versa: the two concepts
    // answer different questions, and conflating them would force every enum to
    // carry a type name it does not need.
    STATIC_REQUIRE_FALSE(Named<WaveformNamed>);
    STATIC_REQUIRE(Named<SpringDamper>);
}

// ===========================================================================
// Units
// ===========================================================================

TEST_CASE("reflect.unit_symbol.delegates_to_units", "[reflect]") {
    // Deliberately a forward, not a second implementation: units already owns the
    // golden-tested symbol spelling, and a reflection layer that formatted units its
    // own way would be the second source of a name -- the thing this module exists
    // to remove.
    const qp::units::Dim length{1, 0, 0, 0, 0, 0, 0};
    const qp::units::Dim force{1, 1, -2, 0, 0, 0, 0};
    const qp::units::Dim none{};

    REQUIRE(unit_symbol_of(length) == qp::units::unit_symbol(length));
    REQUIRE(unit_symbol_of(force) == qp::units::unit_symbol(force));
    REQUIRE(unit_symbol_of(none) == qp::units::unit_symbol(none));

    // And the answers are the ones a physicist expects, so a drift in either module
    // is visible here as well as in the units golden test.
    REQUIRE(unit_symbol_of(length) == "m");
    REQUIRE(unit_symbol_of(force) == "kg*m/s^2");
    // Dimensionless is spelled "1", not empty: the units golden test fixes that
    // spelling, and an empty symbol would render a column of blanks where a reader
    // needs to see that the quantity is a pure ratio.
    REQUIRE(unit_symbol_of(none) == "1");
}
