/**
 * @file port_ui.hpp
 * @brief Port-type-driven property panels: a new port type gets an editor for free.
 *
 * ## The promise this module has to keep
 *
 * From the charter: **adding a port type must not require touching any panel
 * code.** The way to keep that promise is not to write a generic panel that
 * guesses; it is to have each port type *declare* how it is edited, and to have
 * exactly one panel that renders any declaration. Then "new type, new editor"
 * stops being a code change in the view layer and becomes a registration.
 *
 * ## What crosses the boundary, and what does not
 *
 * Only **descriptions** cross. Not widgets, not callbacks that build widgets, not
 * Qt types. `core` must contain no Qt header at all, and a description that named
 * a widget class would drag Qt in through the back door.
 *
 * So a description says "this is a number, in this range, in this unit" and the
 * Qt view decides that means a spin box. The consequence is that the same
 * description renders in a Qt panel today and in any other front end later
 * without the port type knowing either exists.
 *
 * ## Why a bound editor is not a special case
 *
 * An instrument reports values in a unit; a model takes a parameter in a unit. If
 * the editor showed only "1.5" the user would have to remember which unit, and
 * the most common lab mistake -- entering millimetres where metres were meant --
 * becomes invisible. So the unit and the dimension are part of the description,
 * and a control that cannot express them is not an acceptable renderer.
 *
 * @ownership   pure (contract) / owns (the registry)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   No Qt type appears anywhere in this module
 * @errors      noexcept
 * @complexity  --
 * @nondet      none
 * @frozen      no
 * @tests       portui.registry.fallback_is_always_available,
 *              portui.registry.registered_description_wins,
 *              portui.description.numeric_bounds
 */
#pragma once

#include <qp/diag/result.hpp>
#include <qp/ports/port_type.hpp>
#include <qp/units/dim.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qp::authoring {

/**
 * @brief Which kind of control can edit a value of this type.
 *
 * A closed enumeration, and that is the point: a plugin cannot name a widget, so
 * a plugin cannot make the core depend on a UI toolkit. Adding a genuinely new
 * kind of editor is a core change, and it should be -- it is a change to what the
 * platform can express.
 *
 * @ownership   pure
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   A renderer that cannot express `text` must not silently substitute
 * @errors      noexcept
 * @frozen      no
 * @tests       portui.description.editor_kinds
 */
enum class EditorKind : std::uint8_t {
    /// A read-only display. The safe default: it is always renderable.
    read_only = 0,
    /// A number, optionally bounded and optionally carrying a unit.
    number = 1,
    /// A checkbox.
    boolean = 2,
    /// A single line of text.
    text = 3,
    /// A choice among a fixed list of labels.
    choice = 4,
    /// A reference to something outside the panel (a file, a dataset, a node).
    reference = 5,
};

/**
 * @brief Stable short name of an editor kind, for logs and diagnostics.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Returns a non-empty ASCII identifier
 * @invariant   Distinct kinds never share a name
 * @errors      noexcept
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       portui.description.editor_kinds
 */
[[nodiscard]] constexpr const char* to_string(EditorKind k) noexcept {
    switch (k) {
        case EditorKind::read_only: return "read_only";
        case EditorKind::number: return "number";
        case EditorKind::boolean: return "boolean";
        case EditorKind::text: return "text";
        case EditorKind::choice: return "choice";
        case EditorKind::reference: return "reference";
    }
    return "unknown";
}

/**
 * @brief How one port type is edited.
 *
 * Every field beyond `editor` is optional, and an absent field means "no
 * constraint". A renderer must treat an absent constraint as "anything this kind
 * can hold" rather than inventing a default range: a spin box that silently
 * limited an unbounded parameter to 0..100 would reject a legitimate value.
 *
 * @ownership   owns (the label and the choice list)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   `editor == choice` implies `choices` is non-empty
 * @errors      noexcept
 * @frozen      no
 * @tests       portui.description.numeric_bounds
 */
struct PortUiDesc final {
    /// The port type this describes. Must match a registered type.
    std::string port_type{};
    EditorKind editor = EditorKind::read_only;
    /// Human-readable label. Empty means "use the port's own name".
    std::string label{};
    /// Lowest accepted value, for `number`.
    std::optional<double> minimum{};
    /// Highest accepted value, for `number`.
    std::optional<double> maximum{};
    /// Increment a control should use, for `number`. Absent means "unspecified".
    std::optional<double> step{};
    /// The dimension of the value, so the editor can show a unit.
    std::optional<units::Dim> dimension{};
    /// Labels for `choice`, in order. The index is the stored value.
    std::vector<std::string> choices{};

    /**
     * @brief Whether the description is usable as written.
     *
     * Checked at registration so that a malformed description fails while the
     * plugin loads, not when the user opens the panel.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        False when a choice has no options, a bound is not a finite
     *              number, or the bounds are inverted. **An empty port type is
     *              valid**: the fallback description for a type that is not
     *              registered must itself be renderable, or the one path that is
     *              supposed to always work would be the one path that does not.
     * @invariant   A valid description can always be rendered as `read_only`
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       portui.description.numeric_bounds
     */
    [[nodiscard]] bool is_valid() const noexcept;

    /**
     * @brief Whether `value` is inside the declared bounds, if any.
     *
     * @ownership   pure
     * @thread      main
     * @pre         none
     * @post        An absent bound imposes no constraint
     * @invariant   NaN and infinity are never accepted by a bounded description
     * @errors      noexcept
     * @complexity  O(1)
     * @nondet      none
     * @frozen      no
     * @tests       portui.description.numeric_bounds
     */
    [[nodiscard]] bool accepts(double value) const noexcept;
};

/**
 * @brief Registry of port-type UI descriptions.
 *
 * @ownership   owns (the descriptions, never a widget)
 * @thread      main
 * @pre         none
 * @post        none
 * @invariant   describe() always returns a usable description
 * @errors      noexcept
 * @frozen      no
 * @tests       portui.registry.fallback_is_always_available
 */
class PortUiRegistry final {
public:
    PortUiRegistry() = default;
    PortUiRegistry(const PortUiRegistry&) = delete;
    PortUiRegistry& operator=(const PortUiRegistry&) = delete;

    /**
     * @brief Registers a description, replacing any previous one for that type.
     *
     * Replacing rather than refusing a duplicate, unlike the other registries in
     * this project. The reason is that the promise here is "every registered type
     * has an editor": a plugin **overriding** the generic editor for a type it
     * owns is the intended use, not an accident. The last registration wins, and
     * because plugins load in a dependency order that is deterministic, so does
     * the outcome.
     *
     * @ownership   owns (copies the description)
     * @thread      main
     * @pre         `desc.is_valid()`
     * @post        describe(desc.port_type) returns `desc`
     * @invariant   Registering the same type twice leaves one entry
     * @errors      Returns invalid_argument for a malformed description
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       portui.registry.registered_description_wins
     */
    [[nodiscard]] diag::Result<void> set(const PortUiDesc& desc);

    /**
     * @brief The description for `port_type`, or a safe fallback.
     *
     * **Never fails.** A type with no registered description gets a read-only
     * display naming the type. That is the difference between "a plugin's
     * parameter cannot be edited yet" and "the panel will not open": the first is
     * a limitation a user can work around, the second loses the whole session.
     *
     * @ownership   pure (returns a copy; the fallback is constructed on demand)
     * @thread      main
     * @pre         none
     * @post        Always returns a description whose is_valid() is true
     * @invariant   A registered description is returned unchanged
     * @errors      noexcept
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       portui.registry.fallback_is_always_available,
     *              portui.registry.registered_description_wins
     */
    [[nodiscard]] PortUiDesc describe(const std::string& port_type) const noexcept;

    /**
     * @brief Removes a description, for a plugin that is unloading.
     *
     * After this, `describe` falls back rather than failing, so a panel that is
     * already open keeps working with the generic display.
     *
     * @ownership   owns
     * @thread      main
     * @pre         none
     * @post        describe(port_type) returns the fallback
     * @invariant   Other descriptions are unaffected
     * @errors      Returns unknown_node when the type had no description
     * @complexity  O(n)
     * @nondet      none
     * @frozen      no
     * @tests       portui.registry.unregister_falls_back
     */
    [[nodiscard]] diag::Result<void> remove(const std::string& port_type);

    /// @brief Whether `port_type` has a registered description.
    [[nodiscard]] bool has(const std::string& port_type) const noexcept;

    /// @brief Registered port types, in registration order.
    [[nodiscard]] std::vector<std::string> port_types() const noexcept;

    /// @brief Number of registered descriptions.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /**
     * @brief The description used for a type that has none.
     *
     * `read_only`, not `number`: an unknown type might hold text, a handle, or
     * something with no numeric meaning at all, and a number control that cannot
     * represent the value would show a 0 where the user's reference used to be.
     *
     * @ownership   pure (constructs a new description)
     * @thread      main
     * @pre         none
     * @post        The result's is_valid() is true for any input, including empty
     * @invariant   Never returns an editor kind that can misrepresent a value
     * @errors      noexcept
     * @complexity  O(len)
     * @nondet      none
     * @frozen      no
     * @tests       portui.registry.fallback_is_always_available
     */
    [[nodiscard]] static PortUiDesc fallback(const std::string& port_type) noexcept;

private:
    struct Entry final {
        std::string port_type{};
        PortUiDesc desc{};
    };

    std::vector<Entry> entries_;
};

}  // namespace qp::authoring
