/**
 * @file reflect.hpp
 * @brief The single source of a type's name: one spelling shared by C++, YAML and scripts.
 *
 * ## The problem this exists to prevent
 *
 * A document names types in text. A YAML experiment says `type: qp.spring_damper`; a
 * script asks the registry for `qp.scalar_f64`; the C++ side has a struct called
 * `SpringDamper`. If those three spellings are produced independently -- a literal
 * here, an `if constexpr` chain there, a hand-written string in a plugin manifest --
 * they will drift. The drift is not a compile error. It shows up as a document that
 * loads on one build and not another, or a plugin that is present but "not found",
 * and the message names a type that plainly exists.
 *
 * So the rule is: **a name is written down once**, next to the type it names, and
 * everything else asks.
 *
 * ## Why the name is not `typeid(T).name()`
 *
 * `typeid` is available and tempting, and it is unusable for a saved document:
 *
 *   - it is **implementation-defined**. MSVC returns `class Qp::SpringDamper`,
 *     GCC returns `N2Qp13SpringDamperE`, and both may change between compiler
 *     versions. A document written by one build would not load in another.
 *   - it is **not namespaced by the project**. Two plugins can each define a
 *     `Filter`, and one would shadow the other.
 *   - it carries **no version**, so nothing can migrate a document when a type's
 *     fields change.
 *
 * Hence an explicit `type_name` member on the type, following the same `qp.`
 * dotted convention the port types already use. The cost is that a type has to say
 * its own name; the benefit is that the name is stable enough to be evidence.
 *
 * ## Why enum names are reflected too
 *
 * For the same reason, one level down. A choice parameter stores an integer, and
 * the document must also record what that integer *meant*: if the enum's order
 * changes, a saved `2` silently becomes a different option. `enum_names<E>()` gives
 * the labels so that a document can carry both, and a mismatch can be detected.
 * The reflection is a `names()` static member rather than a macro or a generated
 * table, because a macro would have to be maintained and a generator would have to
 * run -- and both are exactly the "second source" this module exists to remove.
 *
 * ## What is deliberately absent
 *
 * No reflection of members, no serialisation, no schema. A type says its name and
 * its enum labels; how it is written to a file is a serialisation plugin's
 * business, and a reflection layer that knew about fields would be the serialiser.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   A type's name does not change between builds of the same version
 * @errors      noexcept
 * @frozen      no
 * @tests       reflect.type_name.is_compile_time,
 *              reflect.type_name.is_not_compiler_specific,
 *              reflect.enum_names.are_ordered
 */
#pragma once

#include <qp/units/dim.hpp>
#include <qp/units/unit_symbol.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace qp::reflect {

/**
 * @brief Concept for a type that names itself.
 *
 * Declared as a concept rather than left to a `static_assert` inside the function so
 * that a missing `type_name` produces a readable "constraint not satisfied" naming
 * the type, instead of a wall of template errors pointing at `<string_view>`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Satisfied only by a type with a static `std::string_view type_name`
 * @errors      noexcept
 * @frozen      no
 * @tests       reflect.type_name.is_compile_time
 */
template <class T>
concept Named = requires {
    { T::type_name } -> std::convertible_to<std::string_view>;
};

/**
 * @brief The stable name of a type, as written in a document.
 *
 * @ownership   pure (the name is a static string owned by the type)
 * @thread      any
 * @pre         `T` satisfies Named
 * @post        Returns a non-empty dotted identifier such as "qp.scalar_f64"
 * @invariant   The same name in every build, on every compiler, of this version
 * @errors      noexcept; a type that does not name itself fails to compile, which
 *              is the point of the concept -- the diagnostic names the missing
 *              member rather than failing deep inside a template
 * @frozen      no
 * @tests       reflect.type_name.is_compile_time,
 *              reflect.type_name.is_not_compiler_specific
 */
template <Named T>
[[nodiscard]] constexpr std::string_view type_name() noexcept {
    return T::type_name;
}

/// @brief The stable name of a value's type, deduced rather than spelled.
template <Named T>
[[nodiscard]] constexpr std::string_view type_name(const T&) noexcept {
    return T::type_name;
}

/// @brief Whether `T` can name itself. Useful in a diagnostic or a static_assert.
template <class T>
inline constexpr bool is_named_v = Named<T>;

/**
 * @brief Whether a name is a legal document name.
 *
 * Checked at registration rather than trusted, because a name is the key a saved
 * document is looked up by: an empty name, or one with whitespace in it, produces a
 * document that cannot be parsed back and a plugin that cannot be found -- and the
 * failure appears at load time in someone else's session.
 *
 * The convention is `qp.` followed by lower-case words separated by dots or
 * underscores. It is enforced only as far as it matters: non-empty, ASCII, no
 * whitespace, and no leading or trailing dot.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        None
 * @invariant   Depends only on its argument
 * @errors      noexcept
 * @complexity  O(len)
 * @nondet      none
 * @frozen      no
 * @tests       reflect.type_name.validation
 */
[[nodiscard]] constexpr bool is_valid_name(std::string_view name) noexcept {
    if (name.empty()) return false;
    if (name.front() == '.') return false;
    if (name.back() == '.') return false;
    bool previous_was_dot = false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-';
        if (!ok) return false;
        // A doubled dot would produce an empty path segment, which a YAML reader
        // and a name lookup would disagree about.
        if (c == '.' && previous_was_dot) return false;
        previous_was_dot = (c == '.');
    }
    return true;
}

/**
 * @brief Concept for an enum whose labels are declared beside it.
 *
 * ## Why free functions rather than members
 *
 * An earlier version of this concept required `E::names()` and `E::count()`, which
 * is not expressible in C++: an enum's body may contain enumerators and nothing
 * else. No enum could satisfy it, the module compiled, and the failure would have
 * appeared as "constraint not satisfied" on a perfectly correct plugin enum.
 *
 * So the labels are free functions in the enum's own namespace, found by
 * argument-dependent lookup:
 *
 * ```cpp
 * enum class Waveform { sine, square, ramp };
 * constexpr std::string_view qp_enum_names(Waveform) noexcept { return "sine,square,ramp"; }
 * constexpr std::size_t      qp_enum_count(Waveform) noexcept { return 3; }
 * ```
 *
 * They stay next to the type they describe -- which is what "one spelling" requires
 * -- and nothing has to be specialised or registered.
 *
 * The name is deliberately prefixed. A bare `names` would collide with any other
 * `names` reachable by ADL in the same namespace, and the collision would resolve to
 * whichever overload the compiler preferred rather than to an error.
 *
 * A concept has no signature to be `noexcept` about; the constraint is evaluated at
 * compile time, and an enum that does not list its labels fails to compile with a
 * diagnostic naming the missing function.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Satisfied only by an enum with ADL-visible labels
 * @errors      Compile-time only; no runtime failure path exists
 * @frozen      no
 * @tests       reflect.enum_names.are_ordered
 */
template <class E>
concept Enumerated = std::is_enum_v<E> && requires(E value) {
    { qp_enum_names(value) } -> std::convertible_to<std::string_view>;
    { qp_enum_count(value) } -> std::convertible_to<std::size_t>;
};

/**
 * @brief The labels of `E`, in declaration order, as one comma-separated view.
 *
 * A single string rather than a vector: the labels are a compile-time constant, and
 * a `std::vector` would allocate at every call for a value that never changes. A
 * caller that needs them individually uses `enum_label`, which is what a document
 * writer does once.
 *
 * @ownership   pure
 * @thread      any
 * @pre         `E` satisfies Enumerated
 * @post        Returns as many comma-separated labels as `enum_count<E>()`
 * @invariant   The order matches the enumerators' declaration order
 * @errors      noexcept; an enum without ADL-visible labels fails to compile, so the
 *              diagnostic names the missing function rather than failing inside a
 *              template
 * @frozen      no
 * @tests       reflect.enum_names.are_ordered
 */
template <Enumerated E>
[[nodiscard]] constexpr std::string_view enum_names() noexcept {
    return qp_enum_names(E{});
}

/**
 * @brief The number of labels an enum declares.
 *
 * @ownership   pure
 * @thread      any
 * @pre         `E` satisfies Enumerated
 * @post        Equals the number of comma-separated fields in enum_names of E
 * @invariant   Constant for the type
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       reflect.enum_names.are_ordered
 */
template <Enumerated E>
[[nodiscard]] constexpr std::size_t enum_count() noexcept {
    return qp_enum_count(E{});
}

/**
 * @brief The `index`th label of an enum, or an empty view when out of range.
 *
 * Out of range yields empty rather than terminating: a document can hold an integer
 * a newer build wrote, and "this option is unknown" is a thing a reader must be able
 * to say rather than a reason to abort.
 *
 * @ownership   pure
 * @thread      any
 * @pre         `E` satisfies Enumerated
 * @post        Empty when `index >= enum_count<E>()`
 * @invariant   `enum_label<E>(i)` is the `i`th comma-separated field of enum_names<E>()
 * @errors      noexcept
 * @complexity  O(len)
 * @nondet      none
 * @frozen      no
 * @tests       reflect.enum_names.are_ordered
 */
template <Enumerated E>
[[nodiscard]] constexpr std::string_view enum_label(std::size_t index) noexcept {
    std::string_view text = enum_names<E>();
    // The remainder is consumed before the terminator is tested, so `npos` never
    // takes part in arithmetic. An earlier version did `remove_prefix(npos + 1)`,
    // which wraps to 0 and spins forever on a label list with no comma left -- a
    // hang that only appears on the input nobody tested.
    for (std::size_t current = 0; !text.empty(); ++current) {
        const std::size_t comma = text.find(',');
        if (current == index) {
            return comma == std::string_view::npos ? text : text.substr(0, comma);
        }
        if (comma == std::string_view::npos) return {};
        text.remove_prefix(comma + 1);
    }
    return {};
}

/**
 * @brief The display symbol of a dimension, from the units module.
 *
 * Deliberately a thin forward rather than a second implementation: `unit_symbol`
 * already exists and is golden-tested, and a reflection layer that formatted units
 * its own way would be the second source of a name -- the thing this module exists
 * to remove.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns the same string `qp::units::unit_symbol(d)` returns
 * @invariant   Byte-identical to the units module's answer
 * @errors      May allocate; allocation failure terminates
 * @complexity  O(exponents)
 * @nondet      none
 * @frozen      no
 * @tests       reflect.unit_symbol.delegates_to_units
 */
[[nodiscard]] inline std::string unit_symbol_of(units::Dim dim) { return units::unit_symbol(dim); }

}  // namespace qp::reflect
